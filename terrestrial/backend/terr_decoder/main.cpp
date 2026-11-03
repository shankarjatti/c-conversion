// Terrestrial decoder (C++ port of allinone.grc / allinone.py, headless): a GNU Radio C++ flowgraph.
//
//   IQ (ZeroMQ SUB on the Ingest, iq/<band>/, 200 kSps) per band
//   7 DSD chains:  pfb channelizer -> sum of all channels -> rational resampler to 48 kS/s -> low-pass -> squelch ->
//                  FM demod -> dsd_block_ff (the DSD library) -> x6 resampler -> volume -> ZeroMQ audio tap (float32)
//   TETRA chain:   freq-xlating FIR -> MMSE resampler (36 kS/s) -> AGC -> FLL -> PFB clock sync -> CMA equaliser -> diff phasor ->
//                  pi/4-DQPSK constellation decoder -> bits -> tetra-kit (libtetrakit_capi.so) + ACELP codec (libtetra_codec.so)
//                  -> 4 timeslot audio streams (mixed on tap 9104) and call-log events (PMT on PUB 9105)
//   Audio taps: PUB tcp://127.0.0.1:91xx, the interface the terrestrial bridge already reads.
//   No audio sinks: the edge board plays nothing.
#include <dsd/dsd_block_ff.h>
#include <gnuradio/analog/quadrature_demod_cf.h>
#include <gnuradio/analog/feedforward_agc_cc.h>
#include <gnuradio/analog/simple_squelch_cc.h>
#include <gnuradio/blocks/add_blk.h>
#include <gnuradio/blocks/file_sink.h>
#include <gnuradio/blocks/file_source.h>
#include <gnuradio/blocks/throttle.h>
#include <gnuradio/blocks/unpack_k_bits_bb.h>
#include <gnuradio/digital/adaptive_algorithm_cma.h>
#include <gnuradio/digital/constellation.h>
#include <gnuradio/digital/constellation_decoder_cb.h>
#include <gnuradio/digital/diff_phasor_cc.h>
#include <gnuradio/digital/fll_band_edge_cc.h>
#include <gnuradio/digital/linear_equalizer.h>
#include <gnuradio/digital/map_bb.h>
#include <gnuradio/digital/pfb_clock_sync_ccf.h>
#include <gnuradio/filter/freq_xlating_fir_filter.h>
#include <gnuradio/filter/mmse_resampler_cc.h>
#include <gnuradio/block.h>
#include <gnuradio/zeromq/pub_msg_sink.h>
#include <nlohmann/json.hpp>
#include <pmt/pmt.h>
#include <gnuradio/blocks/add_blk.h>
#include <gnuradio/blocks/multiply_const.h>
#include <gnuradio/blocks/stream_to_streams.h>
#include <gnuradio/filter/fir_filter_blk.h>
#include <gnuradio/filter/firdes.h>
#include <gnuradio/filter/pfb_channelizer_ccf.h>
#include <gnuradio/filter/rational_resampler.h>
#include <gnuradio/top_block.h>
#include <gnuradio/zeromq/pub_sink.h>

#include "gr_blocks.hpp"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <zlib.h>

#include <atomic>
#include <csignal>
#include <condition_variable>
#include <map>
#include <set>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop = true; }
static std::string env(const char* k, const char* d) { const char* v = std::getenv(k); return v ? v : d; }

struct DsdBand {
    const char* key;
    int nchans;                       // channelizer outputs (200 kSps / nchans per channel)
    int interp, decim;                // channel-sum rate -> 48 kS/s
    double lpf_cutoff, lpf_width;     // Hz, at 48 kS/s
    double gain_demod;
    gr::dsd::dsd_frame_mode frame;
    gr::dsd::dsd_modulation_optimizations mod;
    float volume;
    int tap_port;
};

static const DsdBand BANDS[] = {
    {"dmr", 16, 96, 25, 6250, 2000, 1.0, gr::dsd::dsd_FRAME_DMR_MOTOTRBO, gr::dsd::dsd_MOD_C4FM, 0.2f, 9101},
    {"nxdn", 16, 96, 25, 6250, 2000, 1.0, gr::dsd::dsd_FRAME_NXDN96, gr::dsd::dsd_MOD_GFSK, 0.5f, 9102},
    {"p25", 10, 12, 5, 9500, 2000, 1.0, gr::dsd::dsd_FRAME_P25_PHASE_1, gr::dsd::dsd_MOD_C4FM, 0.5f, 9103},
    {"nxdn48", 32, 192, 25, 3000, 1000, 1.0, gr::dsd::dsd_FRAME_NXDN48_IDAS, gr::dsd::dsd_MOD_GFSK, 3.0f, 9106},
    {"dstar", 16, 96, 25, 6250, 2000, 1.0, gr::dsd::dsd_FRAME_DSTAR, gr::dsd::dsd_MOD_GFSK, 0.5f, 9107},
    {"x2tdma", 16, 96, 25, 6250, 2000, 1.0, gr::dsd::dsd_FRAME_X2_TDMA, gr::dsd::dsd_MOD_C4FM, 0.5f, 9108},
    {"provoice", 8, 48, 25, 9500, 2000, 1.0, gr::dsd::dsd_FRAME_PROVOICE, gr::dsd::dsd_MOD_GFSK, 0.5f, 9109},
};


// ---------------------------------------------------------------- TETRA decoder block (port of tetra_decoder_block.py)
// Raw channel bits in (one per item) -> tetra-kit (C API) -> reports over UDP 127.0.0.1:42100 (hard-coded in tetra-kit) ->
// TCH_S frames through the ACELP codec -> 4 timeslot PCM streams; everything else -> "call_log" PMT messages.
using json = nlohmann::json;

static std::string py_repr(const json& j) {                   // Python's str()/repr() of the parsed JSON value (what the Python block publishes)
    if (j.is_string()) { std::string s = j.get<std::string>(); return s.find('\'') == std::string::npos ? "'" + s + "'" : "\"" + s + "\""; }
    if (j.is_null()) return "None";
    if (j.is_boolean()) return j.get<bool>() ? "True" : "False";
    if (j.is_array()) { std::string r = "["; for (size_t i = 0; i < j.size(); ++i) r += (i ? ", " : "") + py_repr(j[i]); return r + "]"; }
    if (j.is_object()) { std::string r = "{"; bool f = true; for (auto& [k, v] : j.items()) { r += (f ? "" : ", ") + py_repr(k) + ": " + py_repr(v); f = false; } return r + "}"; }
    return j.dump();
}
static std::string py_str(const json& j) { return j.is_string() ? j.get<std::string>() : py_repr(j); }

class tetra_decoder : public gr::block {
public:
    using sptr = std::shared_ptr<tetra_decoder>;
    static sptr make(const std::string& runtime) { return gnuradio::make_block_sptr<tetra_decoder>(runtime); }
    explicit tetra_decoder(const std::string& runtime)
        : gr::block("tetra_decoder", gr::io_signature::make(1, 1, 1), gr::io_signature::make(4, 4, sizeof(float))) {
        message_port_register_out(pmt::mp("call_log"));
        std::string capi = runtime + "/gr_tetra_decoder/libtetrakit_capi.so", codec = runtime + "/tetra-kit/recorder/audio/libtetra_codec.so";
        void* hc = dlopen(capi.c_str(), RTLD_NOW);
        void* hk = dlopen(codec.c_str(), RTLD_NOW);
        if (!hc || !hk) { std::fprintf(stderr, "TETRA libraries: %s\n", dlerror()); std::exit(1); }
        create_ = (void* (*)(int, int, int, int))dlsym(hc, "tetrakit_create");
        rx_ = (void (*)(void*, unsigned char))dlsym(hc, "tetrakit_rx_symbol");
        destroy_ = (void (*)(void*))dlsym(hc, "tetrakit_destroy");
        codec_create_ = (void* (*)())dlsym(hk, "tetra_codec_create");
        codec_destroy_ = (void (*)(void*))dlsym(hk, "tetra_codec_destroy");
        codec_frame_ = (int (*)(void*, int16_t*, int16_t*))dlsym(hk, "tetra_codec_process_frame");
        codec_ = codec_create_();
        // report socket: tetra-kit sendto()s to 127.0.0.1:42100 on every report (hard-coded in its report.cc), so we bind there
        rx_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        int one = 1;
        setsockopt(rx_sock_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(42100);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (bind(rx_sock_, (sockaddr*)&a, sizeof a) != 0) { std::perror("bind 127.0.0.1:42100 (another TETRA decoder running?)"); std::exit(1); }
        timeval tv{0, 500000};
        setsockopt(rx_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        writer_sock_ = socket(AF_INET, SOCK_DGRAM, 0);            // any valid UDP fd: tetra-kit names the destination per sendto()
        handle_ = create_(writer_sock_, 1, 1, 0);                  // remove_fill_bits, log_level 1, no wireshark output
        thread_ = std::thread([this] { rx_loop(); });
    }
    ~tetra_decoder() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        close(rx_sock_);
        close(writer_sock_);
        codec_destroy_(codec_);
        destroy_(handle_);
    }
    int general_work(int noutput_items, gr_vector_int& ninput_items, gr_vector_const_void_star& in, gr_vector_void_star& out) override {
        const uint8_t* i = (const uint8_t*)in[0];
        for (int k = 0; k < ninput_items[0]; ++k) rx_(handle_, i[k] & 0x01);
        consume(0, ninput_items[0]);
        std::lock_guard<std::mutex> lk(q_mu_);
        int max_produced = 0;
        return fill_outputs(out, noutput_items, max_produced);          // one produced count for all outputs, like the Python block
    }
private:
    int fill_outputs(gr_vector_void_star& out, int cap, int& max_produced) {
        for (int slot = 0; slot < 4; ++slot) {
            float* o = (float*)out[slot];
            int produced = 0;
            auto& q = audio_q_[slot];
            while (produced < cap && !q.empty()) {
                auto& front = q.front();
                int take = std::min<int>(cap - produced, (int)front.size() - carry_pos_[slot]);
                std::memcpy(o + produced, front.data() + carry_pos_[slot], take * sizeof(float));
                produced += take;
                carry_pos_[slot] += take;
                if (carry_pos_[slot] >= (int)front.size()) { q.pop_front(); carry_pos_[slot] = 0; }
            }
            if (produced < cap) std::memset(o + produced, 0, (cap - produced) * sizeof(float));   // keep every output in step
            max_produced = std::max(max_produced, produced);
        }
        return max_produced;
    }
    static bool b64_decode(const std::string& in, std::vector<uint8_t>& out) {
        static int T[256];
        static bool init = false;
        if (!init) { std::fill(T, T + 256, -1); const char* c = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; for (int i = 0; i < 64; ++i) T[(unsigned char)c[i]] = i; init = true; }
        int val = 0, bits = -8;
        for (unsigned char ch : in) {
            if (ch == '=') break;
            if (T[ch] < 0) continue;
            val = (val << 6) + T[ch];
            bits += 6;
            if (bits >= 0) { out.push_back((uint8_t)((val >> bits) & 0xFF)); bits -= 8; }
        }
        return !out.empty();
    }
    void rx_loop() {
        std::vector<char> buf(65536);
        while (!stop_) {
            ssize_t n = recvfrom(rx_sock_, buf.data(), buf.size(), 0, nullptr, nullptr);
            if (n <= 0) continue;
            json msg = json::parse(buf.data(), buf.data() + n, nullptr, false);
            if (msg.is_discarded() || !msg.is_object()) continue;
            std::string service = msg.value("service", std::string()), pdu = msg.value("pdu", std::string());
            if (service == "UPLANE" && pdu == "TCH_S") { handle_tch_s(msg); continue; }
            publish_call_log(msg);
        }
    }
    void handle_tch_s(const json& msg) {
        if (!msg.contains("tn") || !msg["tn"].is_number_integer()) return;
        int tn = msg["tn"].get<int>();
        if (tn < 1 || tn > 4 || !msg.contains("frame") || !msg["frame"].is_string()) return;
        std::vector<uint8_t> z;
        if (!b64_decode(msg["frame"].get<std::string>(), z)) return;
        std::vector<uint8_t> raw(1380 * 8);
        uLongf rl = raw.size();
        if (uncompress(raw.data(), &rl, z.data(), z.size()) != Z_OK || rl < 1380) return;
        int16_t sf[690], pcm[480];
        std::memcpy(sf, raw.data(), 1380);
        if (codec_frame_(codec_, sf, pcm) != 1) return;
        std::vector<float> f(480);
        for (int k = 0; k < 480; ++k) f[k] = pcm[k] / 32768.0f;
        std::lock_guard<std::mutex> lk(q_mu_);
        audio_q_[tn - 1].push_back(std::move(f));
    }
    // DecoderEvent.from_tetra_json + to_pmt (decoder_framework/decoder_event.py)
    void publish_call_log(const json& msg) {
        if (!msg.contains("service") || !msg.contains("pdu") || !msg["pdu"].is_string()) return;
        std::string pdu = msg["pdu"].get<std::string>();
        static const std::map<std::string, std::string> ET = {
            {"D-SETUP", "call_setup"}, {"D-CONNECT", "call_setup"}, {"D-TX GRANTED", "call_active"}, {"D-CONNECT ACK", "call_active"},
            {"D-RELEASE", "call_end"}, {"D-DISCONNECT", "call_end"}, {"D-TX CEASED", "call_end"}, {"D-LOCATION UPDATE ACCEPT", "registration"},
            {"D-ATTACH/DETACH GROUP IDENTITY", "registration"}, {"D-NWRK-BROADCAST", "broadcast"}, {"D-NWRK-BROADCAST-EXTENSION", "broadcast"},
            {"D-STATUS", "status"}, {"D-SDS-DATA", "status"}};
        std::string et;
        auto it = ET.find(pdu);
        if (it != ET.end()) et = it->second;
        else { et = pdu; for (auto& c : et) { c = (char)std::tolower((unsigned char)c); if (c == ' ' || c == '/') c = '_'; } }
        std::string talkgroup;
        for (const char* k : {"actual ssi", "ssi", "ussi", "smi"}) if (msg.contains(k)) { talkgroup = py_str(msg[k]); break; }
        long channel = (msg.contains("tn") && msg["tn"].is_number_integer()) ? msg["tn"].get<long>() : -1;
        std::vector<std::pair<std::string, json>> fields;
        if (msg.contains("fn")) fields.push_back({"fn", msg["fn"]});
        if (msg.contains("mn")) fields.push_back({"mn", msg["mn"]});
        if (msg.contains("usage marker")) fields.push_back({"usage_marker", msg["usage marker"]});
        if (msg.contains("encryption mode")) {
            const json& e = msg["encryption mode"];
            bool b = e.is_boolean() ? e.get<bool>() : e.is_number() ? e.get<double>() != 0 : e.is_string() ? !e.get<std::string>().empty() : !e.is_null();
            fields.push_back({"encryption", json(b)});
        }
        static const std::set<std::string> known = {"service", "pdu", "tn", "fn", "mn", "ssi", "actual ssi", "ussi", "smi", "time", "usage marker", "encryption mode", "address_type"};
        for (auto& [k, v] : msg.items()) if (!known.count(k)) fields.push_back({k, v});
        std::map<std::string, json> flat = {{"protocol", "TETRA"}, {"event_type", et}, {"talkgroup", talkgroup}, {"channel", channel},
                                            {"timestamp", std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()}};
        for (auto& [k, v] : fields) flat[k] = v;
        pmt::pmt_t d = pmt::make_dict();
        for (auto& [k, v] : flat) {
            pmt::pmt_t val;
            if (v.is_boolean()) val = pmt::from_bool(v.get<bool>());
            else if (v.is_number_integer()) val = pmt::from_long(v.get<long>());
            else if (v.is_number_float()) val = pmt::from_double(v.get<double>());
            else val = pmt::intern(py_str(v));
            d = pmt::dict_add(d, pmt::intern(k), val);
        }
        message_port_pub(pmt::mp("call_log"), d);
    }

    void* (*create_)(int, int, int, int);
    void (*rx_)(void*, unsigned char);
    void (*destroy_)(void*);
    void* (*codec_create_)();
    void (*codec_destroy_)(void*);
    int (*codec_frame_)(void*, int16_t*, int16_t*);
    void* handle_ = nullptr;
    void* codec_ = nullptr;
    int rx_sock_ = -1, writer_sock_ = -1;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex q_mu_;
    std::deque<std::vector<float>> audio_q_[4];
    int carry_pos_[4] = {0, 0, 0, 0};
};

// one DSD chain from a source block to the volume block (the output to be tapped)
static gr::basic_block_sptr build_dsd_chain(gr::top_block_sptr tb, const DsdBand& b, gr::basic_block_sptr src, double samp_rate, int maxbuf) {
    auto taps = gr::filter::firdes::low_pass((double)b.nchans, samp_rate, (samp_rate / b.nchans) / 2.0, 1000);   // GRC's pfb.channelizer_ccf hier block = stream_to_streams + pfb_channelizer_ccf
    auto s2ss = gr::blocks::stream_to_streams::make(sizeof(gr_complex), b.nchans);
    auto chan = gr::filter::pfb_channelizer_ccf::make(b.nchans, taps, 1.0f);
    // (in the Python flowgraph the buffer limit set on the pfb.channelizer_ccf HIER block does not reach its inner blocks)
    if (env("SECURECOM_TERR_CHAN_MAXBUF", "0") == "1") chan->set_max_output_buffer(maxbuf);
    tb->connect(src, 0, s2ss, 0);
    auto sum = gr::blocks::add_cc::make(1);
    sum->set_max_output_buffer(maxbuf);
    for (int i = 0; i < b.nchans; ++i) {
        tb->connect(s2ss, i, chan, i);
        tb->connect(chan, i, sum, i);
    }
    auto rs = gr::filter::rational_resampler<gr_complex, gr_complex, float>::make(b.interp, b.decim, std::vector<float>(), 0.4);
    rs->set_max_output_buffer(maxbuf);
    auto lpf = gr::filter::fir_filter_ccf::make(1, gr::filter::firdes::low_pass(1.0, 48000, b.lpf_cutoff, b.lpf_width, gr::fft::window::WIN_HAMMING, 6.76));
    lpf->set_max_output_buffer(maxbuf);
    auto sq = gr::analog::simple_squelch_cc::make(-73, 0.05);
    sq->set_max_output_buffer(maxbuf);
    auto fm = gr::analog::quadrature_demod_cf::make((float)b.gain_demod);
    fm->set_max_output_buffer(maxbuf);
    auto dsd = gr::dsd::dsd_block_ff::make(b.frame, b.mod, 3, true, 0);
    dsd->set_max_output_buffer(std::atoi(env("SECURECOM_TERR_DSD_MAXBUF", std::to_string(maxbuf).c_str()).c_str()));   // test hook: gr-dsd's result depends on the chunk size
    if (int m = std::atoi(env("SECURECOM_TERR_DSD_MULT", "0").c_str())) dsd->set_output_multiple(m);   // test hook: gr-dsd works on whole chunks
    auto ares = gr::filter::rational_resampler<float, float, float>::make(6, 1, std::vector<float>(), 0.4);
    ares->set_max_output_buffer(maxbuf);
    auto vol = gr::blocks::multiply_const_ff::make(b.volume);
    vol->set_max_output_buffer(maxbuf);
    tb->connect(sum, 0, rs, 0);
    tb->connect(rs, 0, lpf, 0);
    tb->connect(lpf, 0, sq, 0);
    tb->connect(sq, 0, fm, 0);
    tb->connect(fm, 0, dsd, 0);
    tb->connect(dsd, 0, ares, 0);
    tb->connect(ares, 0, vol, 0);
    return vol;
}

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const std::string iq = env("SECURECOM_IQ_PUB_ADDR", "tcp://127.0.0.1:5570");
    const int port_shift = std::atoi(env("SECURECOM_TERR_TAP_PORT_SHIFT", "0").c_str());   // test hook: run beside another decoder
    const double samp_rate = 200000;
    const int maxbuf = 1024;                          // the GRC flowgraph limits every block's output buffer to 1024 items
    auto tb = gr::make_top_block("terr_decoder");

    // offline test mode: ONE band from a complex64 file to a float32 file (compare with the Python flowgraph, sample by sample)
    const std::string test_band = env("SECURECOM_TERR_TEST_BAND", "");
    if (!test_band.empty()) {
        for (const DsdBand& b : BANDS) {
            if (test_band != b.key) continue;
            auto fsrc = gr::blocks::file_source::make(sizeof(gr_complex), env("SECURECOM_TERR_TEST_IN", "").c_str(), false);
            fsrc->set_max_output_buffer(maxbuf);
            gr::basic_block_sptr head = fsrc;
            if (env("SECURECOM_TERR_TEST_THROTTLE", "0") == "1") {      // real-time pacing, like the live replay
                auto th = gr::blocks::throttle::make(sizeof(gr_complex), samp_rate);
                th->set_max_output_buffer(maxbuf);
                tb->connect(fsrc, 0, th, 0);
                head = th;
            }
            auto last = build_dsd_chain(tb, b, head, samp_rate, maxbuf);
            auto fsink = gr::blocks::file_sink::make(sizeof(float), env("SECURECOM_TERR_TEST_OUT", "/tmp/terr_test_out.f32").c_str());
            tb->connect(last, 0, fsink, 0);
            tb->run();
            return 0;
        }
        std::fprintf(stderr, "unknown test band %s\n", test_band.c_str());
        return 1;
    }

    for (const DsdBand& b : BANDS) {
        auto src = sc::iq_zmq_source::make(iq, std::string("iq/") + b.key, 200000, 500);
        src->set_max_output_buffer(maxbuf);
        auto last = build_dsd_chain(tb, b, src, samp_rate, maxbuf);
        std::string addr = "tcp://127.0.0.1:" + std::to_string(b.tap_port + port_shift);
        auto pub = gr::zeromq::pub_sink::make(sizeof(float), 1, addr.data(), 100, false, -1);
        tb->connect(last, 0, pub, 0);
    }
    // ---- TETRA
    {
        const int sps = 2, nfilts = 32, decim = 5, channel_rate = 36000;
        const double freq_offset_hz = -1.714 * 1e3;
        auto src = sc::iq_zmq_source::make(iq, "iq/tetra", 200000, 500);
        src->set_max_output_buffer(maxbuf);
        auto xl = gr::filter::freq_xlating_fir_filter_ccf::make(decim, gr::filter::firdes::low_pass(1, samp_rate, 12500, 12500 * 0.2), freq_offset_hz, samp_rate);
        auto mm = gr::filter::mmse_resampler_cc::make(0.0f, (float)(samp_rate / ((double)decim * channel_rate)));
        auto agc = gr::analog::feedforward_agc_cc::make(8, 1.0f);
        auto fll = gr::digital::fll_band_edge_cc::make(sps, 0.35f, 45, 0.03141592653589793f);
        auto rrc = gr::filter::firdes::root_raised_cosine(nfilts, nfilts, 1.0 / (double)sps, 0.35, 11 * sps * nfilts);
        auto sync = gr::digital::pfb_clock_sync_ccf::make(sps, 0.06283185307179586f, rrc, nfilts, nfilts / 2.0f, 1.5f, sps);
        auto constel = gr::digital::constellation_dqpsk::make();
        constel->gen_soft_dec_lut(8);
        auto alg = gr::digital::adaptive_algorithm_cma::make(constel->base(), 10e-3f, 1.0f);
        auto eq = gr::digital::linear_equalizer::make(15, sps, alg, true, std::vector<gr_complex>(), "corr_est");
        auto dp = gr::digital::diff_phasor_cc::make();
        auto cd = gr::digital::constellation_decoder_cb::make(constel->base());
        auto mp = gr::digital::map_bb::make(constel->pre_diff_code());
        auto unpack = gr::blocks::unpack_k_bits_bb::make(constel->bits_per_symbol());
        auto tetra = tetra_decoder::make(env("TETRA_RUNTIME", "/home/shankar/Downloads/tutorials/delhi_testing/tetra_test"));
        auto mix = gr::blocks::add_ff::make(1);
        mix->set_max_output_buffer(maxbuf);
        std::string a_aud = "tcp://127.0.0.1:" + std::to_string(9104 + port_shift), a_ev = "tcp://127.0.0.1:" + std::to_string(9105 + port_shift);
        auto pub = gr::zeromq::pub_sink::make(sizeof(float), 1, a_aud.data(), 100, false, -1);
        auto evpub = gr::zeromq::pub_msg_sink::make(a_ev.data(), 100, true);
        tb->connect(src, 0, xl, 0);
        tb->connect(xl, 0, mm, 0);
        tb->connect(mm, 0, agc, 0);
        tb->connect(agc, 0, fll, 0);
        tb->connect(fll, 0, sync, 0);
        tb->connect(sync, 0, eq, 0);
        tb->connect(eq, 0, dp, 0);
        tb->connect(dp, 0, cd, 0);
        tb->connect(cd, 0, mp, 0);
        tb->connect(mp, 0, unpack, 0);
        tb->connect(unpack, 0, tetra, 0);
        for (int i = 0; i < 4; ++i) tb->connect(tetra, i, mix, i);
        tb->connect(mix, 0, pub, 0);
        tb->msg_connect(tetra, "call_log", evpub, "in");
    }
    std::printf("terrestrial decoder: %zu DSD chains + TETRA; IQ from %s; audio taps on PUB 9101-9109 (+%d)\n", sizeof(BANDS) / sizeof(BANDS[0]), iq.c_str(),
                port_shift);
    std::fflush(stdout);
    tb->start();
    while (!g_stop) usleep(200000);
    tb->stop();
    tb->wait();
    return 0;
}
