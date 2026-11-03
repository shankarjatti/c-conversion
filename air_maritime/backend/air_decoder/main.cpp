// Air & maritime decoder (C++ port of airmaritime/airmaritime_decoder_live.py, headless): a GNU Radio C++ flowgraph.
//
//   IQ  (ZeroMQ SUB on the Ingest: iq/ais/ 250 kSps, iq/adsb/ 6 MSps)
//   AIS   2 channels (161.975 / 162.025 MHz = -/+25 kHz) x (base + 4 carrier-offset hypotheses) = 10 branches:
//         freq-xlating FIR (decimate 5) -> GMSK demod (FM demod + M&M symbol sync) -> NRZI -> invert -> HDLC -> NMEA
//   ADS-B Mode-S demodulator (CRC-24) -> raw frames + decoded fields
//   OUT PMT messages on PUB 9201 (AIS sentences), 9202 (ADS-B decoded dicts), 9203 (raw CRC-valid frames) -- the
//       interface the air bridge already reads (same serialisation as gr-zeromq's pub_msg_sink)
#include <gnuradio/analog/quadrature_demod_cf.h>
#include <gnuradio/blocks/null_sink.h>
#include <gnuradio/digital/binary_slicer_fb.h>
#include <gnuradio/digital/constellation.h>
#include <gnuradio/digital/diff_decoder_bb.h>
#include <gnuradio/digital/hdlc_deframer_bp.h>
#include <gnuradio/digital/symbol_sync_ff.h>
#include <gnuradio/filter/firdes.h>
#include <gnuradio/filter/freq_xlating_fir_filter.h>
#include <gnuradio/filter/rational_resampler.h>
#include <gnuradio/io_signature.h>
#include <gnuradio/sync_block.h>
#include <gnuradio/top_block.h>
#include <gnuradio/zeromq/pub_msg_sink.h>
#include <pmt/pmt.h>
#include <zmq.h>

#include "adsb_fields.hpp"
#include "modes_demod.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop = true; }
static std::string env(const char* k, const char* d) { const char* v = std::getenv(k); return v ? v : d; }
static double now_s() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

#include "gr_blocks.hpp"
using sc::iq_zmq_source;

// ---------------------------------------------------------------- AIS blocks (ports of gr-ais invert / pdu_to_nmea)
class ais_invert : public gr::sync_block {
public:
    using sptr = std::shared_ptr<ais_invert>;
    static sptr make() { return gnuradio::make_block_sptr<ais_invert>(); }
    ais_invert() : gr::sync_block("ais_invert", gr::io_signature::make(1, 1, 1), gr::io_signature::make(1, 1, 1)) {}
    int work(int n, gr_vector_const_void_star& in, gr_vector_void_star& out) override {
        const char* i = (const char*)in[0];
        char* o = (char*)out[0];
        for (int k = 0; k < n; ++k) o[k] = (i[k] ^ 0x01) & 0x01;
        return n;
    }
};

// HDLC frame (bytes) -> "!AIVDM,<frags>,<n>,,<designator>,<6-bit payload>,<pad>*<checksum>" as a PDU (nil . u8vector)
class pdu_to_nmea : public gr::block {
public:
    using sptr = std::shared_ptr<pdu_to_nmea>;
    static sptr make(const std::string& d) { return gnuradio::make_block_sptr<pdu_to_nmea>(d); }
    explicit pdu_to_nmea(const std::string& designator)
        : gr::block("pdu_to_nmea", gr::io_signature::make(0, 0, 0), gr::io_signature::make(0, 0, 0)), d_(designator) {
        message_port_register_in(pmt::mp("print"));
        set_msg_handler(pmt::mp("print"), [this](pmt::pmt_t msg) { on_frame(msg); });
        message_port_register_out(pmt::mp("out"));
    }
private:
    void on_frame(pmt::pmt_t msg) {
        const uint8_t* p = (const uint8_t*)pmt::blob_data(pmt::cdr(msg));
        int nbits = (int)pmt::blob_length(pmt::cdr(msg)) * 8;
        int npad = (6 - (nbits % 6)) % 6;
        std::vector<uint8_t> up((nbits + npad) / 6, 0);
        for (int i = 0; i < nbits; ++i) up[i / 6] |= (uint8_t)(((p[i / 8] >> (7 - (i % 8))) & 1) << (5 - (i % 6)));
        for (int i = 0; i < npad; ++i) up[nbits / 6] <<= 1;
        std::string ascii(up.begin(), up.end());
        for (auto& c : ascii) { if (c > 39) c += 8; c += 48; }
        const size_t max = 56;
        size_t nfrag = 1 + (ascii.size() - 1) / max, off = 0;
        std::string sentence;
        for (size_t fid = 1; fid <= nfrag; ++fid) {
            if (fid > 1) sentence += "\n";
            std::string s = "!AIVDM," + std::to_string(nfrag) + "," + std::to_string(fid) + ",," + d_ + ",";
            std::string frag = ascii.substr(off, max);
            off += frag.size();
            s += frag + "," + std::to_string(npad);
            uint8_t sum = 0;
            for (size_t i = 1; i < s.size(); ++i) sum ^= (uint8_t)s[i];
            char cs[4];
            std::snprintf(cs, sizeof cs, "%02X", sum);
            sentence += s + "*" + cs;
        }
        message_port_pub(pmt::mp("out"), pmt::cons(pmt::PMT_NIL, pmt::init_u8vector(sentence.size(), (const uint8_t*)sentence.c_str())));
    }
    std::string d_;
};

// ---------------------------------------------------------------- ADS-B blocks
class modes_demod : public gr::sync_block {
public:
    using sptr = std::shared_ptr<modes_demod>;
    static sptr make(double fs) { return gnuradio::make_block_sptr<modes_demod>(fs); }
    explicit modes_demod(double fs)
        : gr::sync_block("modes_demod", gr::io_signature::make(1, 1, sizeof(gr_complex)), gr::io_signature::make(0, 0, 0)),
          core_(fs, now_s()) {
        message_port_register_out(pmt::mp("demodulated"));
    }
    int work(int n, gr_vector_const_void_star& in, gr_vector_void_star&) override {
        core_.process((const std::complex<float>*)in[0], (size_t)n, [this](const airdec::Frame& f) {
            pmt::pmt_t meta = pmt::make_dict();
            meta = pmt::dict_add(meta, pmt::mp("timestamp"), pmt::from_double(f.timestamp));
            meta = pmt::dict_add(meta, pmt::mp("snr"), pmt::from_double(f.snr_db));
            message_port_pub(pmt::mp("demodulated"), pmt::cons(meta, pmt::init_u8vector(f.bits.size(), f.bits.data())));
        });
        return n;
    }
private:
    airdec::ModesDemodCore core_;
};

// (meta . 112 bits) -> (dict of fields . bits), the dict gr-adsb's decoder emits and the bridge reads
class adsb_decoder : public gr::block {
public:
    using sptr = std::shared_ptr<adsb_decoder>;
    static sptr make() { return gnuradio::make_block_sptr<adsb_decoder>(); }
    adsb_decoder() : gr::block("adsb_decoder", gr::io_signature::make(0, 0, 0), gr::io_signature::make(0, 0, 0)) {
        message_port_register_in(pmt::mp("demodulated"));
        set_msg_handler(pmt::mp("demodulated"), [this](pmt::pmt_t m) { on_frame(m); });
        message_port_register_out(pmt::mp("decoded"));
    }
private:
    void on_frame(pmt::pmt_t m) {
        pmt::pmt_t meta = pmt::car(m);
        std::vector<uint8_t> bits = pmt::u8vector_elements(pmt::cdr(m));
        airdec::AdsbFields f = airdec::decode_adsb(bits);
        if (!f.any()) return;                                         // status / other message types carry nothing for the map
        const pmt::pmt_t nan = pmt::from_double(std::nan(""));
        pmt::pmt_t d = pmt::make_dict();
        auto put = [&](const char* k, pmt::pmt_t v) { d = pmt::dict_add(d, pmt::mp(k), v); };
        put("df", pmt::from_long(f.df));
        put("icao", pmt::mp(f.icao));
        put("timestamp", pmt::dict_ref(meta, pmt::mp("timestamp"), nan));
        put("snr", pmt::dict_ref(meta, pmt::mp("snr"), nan));
        put("callsign", f.callsign ? pmt::mp(*f.callsign) : pmt::PMT_NIL);
        put("altitude", f.altitude_ft ? pmt::from_long(*f.altitude_ft) : nan);
        put("speed", f.speed_kt ? pmt::from_double(*f.speed_kt) : nan);
        put("heading", f.heading_deg ? pmt::from_double(*f.heading_deg) : nan);
        put("vertical_rate", f.vertical_rate_fpm ? pmt::from_long(*f.vertical_rate_fpm) : nan);
        put("latitude", nan);
        put("longitude", nan);
        message_port_pub(pmt::mp("decoded"), pmt::cons(d, pmt::cdr(m)));
    }
};

// ---------------------------------------------------------------- flowgraph
// GMSK demodulator, as gnuradio.digital.gmsk_demod builds it (a Python hier block, so rebuilt here block by block)
struct Gmsk { gr::basic_block_sptr in, out; };
static void connect_gmsk(gr::top_block_sptr tb, gr::basic_block_sptr src, double sps, gr::basic_block_sptr& tail) {
    const double gain_mu = 0.175, rel_limit = 0.005, freq_error = 0.0;
    double omega = sps * (1 + freq_error), gain_omega = 0.25 * gain_mu * gain_mu;
    double loop_bw = -std::log((gain_mu + gain_omega) / (-2.0) + 1), max_dev = rel_limit * sps;
    double sensitivity = (M_PI / 2) / sps;
    auto fm = gr::analog::quadrature_demod_cf::make((float)(1.0 / sensitivity));
    auto sync = gr::digital::symbol_sync_ff::make(gr::digital::TED_MUELLER_AND_MULLER, (float)omega, (float)loop_bw, 1.0f, 1.0f,
                                                  (float)max_dev, 1, gr::digital::constellation_bpsk::make()->base(),
                                                  gr::digital::IR_MMSE_8TAP, 128, std::vector<float>());
    auto slicer = gr::digital::binary_slicer_fb::make();
    tb->connect(src, 0, fm, 0);
    tb->connect(fm, 0, sync, 0);
    tb->connect(sync, 0, slicer, 0);
    tail = slicer;
}

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::string iq = env("SECURECOM_IQ_PUB_ADDR", "tcp://127.0.0.1:5571");
    std::string host = "tcp://127.0.0.1:";
    int p_ais = std::atoi(env("SECURECOM_AIR_DEC_AIS_PORT", "9201").c_str()), p_adsb = std::atoi(env("SECURECOM_AIR_DEC_ADSB_PORT", "9202").c_str()),
        p_raw = std::atoi(env("SECURECOM_AIR_DEC_RAW_PORT", "9203").c_str());
    const int ais_rate = 250000;
    const double AIS_SPS = 250000.0 / 5 / 9600;                       // 5.208 samples/symbol (the clock recovery may only drift 0.5 %)
    const double adsb_source_rate = 6000000.0, fs = 6e6;

    auto tb = gr::make_top_block("air_decoder");
    std::string a_ais = host + std::to_string(p_ais), a_adsb = host + std::to_string(p_adsb), a_raw = host + std::to_string(p_raw);
    auto sink_ais = gr::zeromq::pub_msg_sink::make(a_ais.data(), 10, true);
    auto sink_adsb = gr::zeromq::pub_msg_sink::make(a_adsb.data(), 10, true);
    auto sink_raw = gr::zeromq::pub_msg_sink::make(a_raw.data(), 10, true);

    // ---- AIS
    auto src_ais = iq_zmq_source::make(iq, "iq/ais", ais_rate, 500);
    auto taps = gr::filter::firdes::low_pass(1.0, ais_rate, 11000, 1000, gr::fft::window::WIN_HAMMING, 6.76);
    const double offs[5] = {0, -900, -450, 450, 900};                 // base channel + extra carrier-offset hypotheses
    const struct { double centre; const char* des; } chans[2] = {{-25000, "A"}, {25000, "B"}};
    int branches = 0;
    for (auto& ch : chans) {
        auto nmea = pdu_to_nmea::make(ch.des);
        tb->msg_connect(nmea, "out", sink_ais, "in");
        for (double off : offs) {
            auto xl = gr::filter::freq_xlating_fir_filter_ccf::make(5, taps, ch.centre + off, ais_rate);
            tb->connect(src_ais, 0, xl, 0);
            gr::basic_block_sptr bits;
            connect_gmsk(tb, xl, AIS_SPS, bits);
            auto nrzi = gr::digital::diff_decoder_bb::make(2);          // AIS is NRZI: transition -> 0 after the invert
            auto inv = ais_invert::make();
            auto hd = gr::digital::hdlc_deframer_bp::make(11, 64);
            tb->connect(bits, 0, nrzi, 0);
            tb->connect(nrzi, 0, inv, 0);
            tb->connect(inv, 0, hd, 0);
            tb->msg_connect(hd, "out", nmea, "print");
            ++branches;
        }
    }

    // ---- ADS-B
    auto src_adsb = iq_zmq_source::make(iq, "iq/adsb", 6000000, 100);   // 100 x 20 ms x 6 MSps = ~100 MB worst case
    auto demod = modes_demod::make(fs);
    auto dec = adsb_decoder::make();
    int g = std::gcd((long)adsb_source_rate, (long)fs);
    if ((long)adsb_source_rate != (long)fs) {
        auto rs = gr::filter::rational_resampler<gr_complex, gr_complex, float>::make((int)((long)fs / g), (int)((long)adsb_source_rate / g), std::vector<float>(), 0.4);
        tb->connect(src_adsb, 0, rs, 0);
        tb->connect(rs, 0, demod, 0);
    } else {
        tb->connect(src_adsb, 0, demod, 0);
    }
    tb->msg_connect(demod, "demodulated", dec, "demodulated");
    tb->msg_connect(demod, "demodulated", sink_raw, "in");
    tb->msg_connect(dec, "decoded", sink_adsb, "in");

    std::printf("air decoder: %d AIS branches + ADS-B; IQ from %s; PMT out PUB %d (AIS) %d (ADS-B decoded) %d (raw frames)\n", branches, iq.c_str(),
                p_ais, p_adsb, p_raw);
    std::fflush(stdout);
    tb->start();
    while (!g_stop) usleep(200000);
    tb->stop();
    tb->wait();
    return 0;
}
