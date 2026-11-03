// Terrestrial bridge (C++ port of gui/bridge_server.py, external-IQ mode): the backend side of the terrestrial
// module on the edge board. No WebSocket, no Module 1/B210 control -- ZeroMQ only.
//
//   IQ        SUB  SECURECOM_IQ_PUB_ADDR (the Ingest), topics iq/<band>/     -> spectrum / activity / channel power
//   audio     SUB  decoder taps tcp://127.0.0.1:9101-9109 (float32)           -> terr/audio/<band>, recordings
//   TETRA     SUB  tcp://127.0.0.1:9105 (PMT dict)                            -> terr/tetra
//   results   PUB  SECURECOM_PUB_ADDR (default tcp://127.0.0.1:5560), topics terr/...  (see docs/ZMQ_INTERFACE.md)
//   commands  REP  SECURECOM_CMD_ADDR (default tcp://127.0.0.1:5561)
//
// It also starts/stops the decoder process (SECURECOM_DECODER_DIR/allinone.py until the decoder itself is C++).
#include <zmq.h>

#include <nlohmann/json.hpp>
#include <pmt/pmt.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;
using cf = std::complex<float>;

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop = true; }

static double now_s() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
static std::string env(const char* k, const char* d) { const char* v = std::getenv(k); return v ? v : d; }
static double round_to(double x, int dec) { double m = std::pow(10.0, dec); return std::round(x * m) / m; }

// ------------------------------------------------------------------ configuration (same tables as the Python bridge)
struct Band { std::string key, name; double freq_mhz; int port; int channels; std::string tap; int audio_rate; };
static const std::vector<Band> BANDS = {
    {"dmr", "DMR446", 446.118528, 7001, 16, "tcp://127.0.0.1:9101", 48000},
    {"nxdn", "NXDN96", 451.09375, 7002, 16, "tcp://127.0.0.1:9102", 48000},
    {"p25", "P25-Ph1", 450.09375, 7003, 10, "tcp://127.0.0.1:9103", 48000},
    {"tetra", "TETRA", 420.0, 7004, 1, "tcp://127.0.0.1:9104", 8000},
    {"nxdn48", "NXDN48", 451.00625, 7005, 32, "tcp://127.0.0.1:9106", 48000},
    {"dstar", "DSTAR", 439.4125, 7006, 16, "tcp://127.0.0.1:9107", 48000},
    {"x2tdma", "X2TDMA", 452.0, 7007, 16, "tcp://127.0.0.1:9108", 48000},
    {"provoice", "PROVOICE", 453.0, 7008, 8, "tcp://127.0.0.1:9109", 48000},
};
static const char* TETRA_EVENTS_ADDR = "tcp://127.0.0.1:9105";
static const double SAMP_RATE = 200000.0;
static const int FFT_N = 512;
static const double ACTIVE_THRESH_DBFS = -60.0, NOISE_MARGIN_DB = 6.0, CROSS_BAND_ALLOW_DB = 10.0, CHAN_SNR_DB = 8.0;
static const size_t NOISE_WIN = 1200, NOISE_MIN_FRAMES = 50, CROSS_BAND_MIN_FRAMES = 10;
static const double REC_ON_DBFS = -38.0, VOICE_HANG_S = 1.5, REC_MIN_VOICE_S = 0.5;
static const double IQ_FRAME_GAP_S = 0.09;      // ~11 spectrum frames/s per band
static const double IQ_TIMEOUT_S = 3.0;         // no message this long = band not connected
static const size_t REC_LIST_PUBLISH_MAX = 200;

static std::string PUB_ADDR, CMD_ADDR, IQ_ADDR, REC_DIR, DECODER_DIR;

// ------------------------------------------------------------------ publisher (thread safe)
class Pub {
public:
    Pub(void* ctx, const std::string& addr) {
        sock_ = zmq_socket(ctx, ZMQ_PUB);
        int hwm = 20000, linger = 0;      // audio is ~1,100 msg/s: room for ~18 s per slow subscriber
        zmq_setsockopt(sock_, ZMQ_SNDHWM, &hwm, sizeof hwm);
        zmq_setsockopt(sock_, ZMQ_LINGER, &linger, sizeof linger);
        if (zmq_bind(sock_, addr.c_str()) != 0) { std::fprintf(stderr, "bind %s: %s\n", addr.c_str(), zmq_strerror(zmq_errno())); std::exit(1); }
    }
    // Never blocks: a subscriber that cannot keep up loses messages (visible as a seq gap)
    void publish(const std::string& topic, json hdr = json::object(), const void* payload = nullptr, size_t n = 0) {
        std::lock_guard<std::mutex> lk(mu_);
        long long s = ++seq_[topic];
        json h = {{"seq", s}, {"t", now_s()}};
        h.update(hdr);
        std::string hs = h.dump();
        if (zmq_send(sock_, topic.data(), topic.size(), ZMQ_SNDMORE | ZMQ_DONTWAIT) < 0) { ++dropped; return; }
        zmq_send(sock_, hs.data(), hs.size(), ZMQ_SNDMORE);
        zmq_send(sock_, payload, n, 0);
    }
    long long dropped = 0;
    void close() { zmq_close(sock_); }
private:
    void* sock_;
    std::mutex mu_;
    std::map<std::string, long long> seq_;
};

static Pub* PUB = nullptr;
static void log_event(const char* band, const std::string& text) {
    PUB->publish("terr/log", {{"band", band ? json(band) : json(nullptr)}, {"t", now_s()}, {"text", text}});
}

// ------------------------------------------------------------------ shared state
static std::atomic<double> LAST_IQ_RX[8];       // time of the last IQ message per band
static std::mutex g_proc_mu;
static pid_t g_dec_pid = 0;
static int g_dec_gen = 0;

static bool decoder_running() {                    // g_dec_pid is cleared by whoever reaps the process (reader thread or stop)
    std::lock_guard<std::mutex> lk(g_proc_mu);
    return g_dec_pid > 0;
}

static json status_snapshot() {
    json ic = json::object();
    double now = now_s();
    for (size_t i = 0; i < BANDS.size(); ++i) ic[BANDS[i].key] = (now - LAST_IQ_RX[i].load()) < IQ_TIMEOUT_S;
    return {{"source", "external"}, {"decoder_running", decoder_running()}, {"iq_connected", ic},
            {"iq_transport", "zmq"}, {"bands", (int)BANDS.size()}};
}

static json bands_info() {
    json a = json::array();
    for (auto& b : BANDS)
        a.push_back({{"key", b.key}, {"name", b.name}, {"freq_mhz", b.freq_mhz}, {"iq_port", b.port},
                     {"iq_rate_sps", (int)SAMP_RATE}, {"channels", b.channels}, {"chan_bw_hz", (int)(SAMP_RATE / b.channels)},
                     {"audio_topic", "terr/audio/" + b.key}, {"audio_rate_hz", b.audio_rate >= 44100 ? b.audio_rate / 2 : b.audio_rate},
                     {"iq_topic", "terr/iq/" + b.key}});
    return a;
}

// ------------------------------------------------------------------ recordings (folder of 16-bit mono WAV files)
static bool ends_wav(const std::string& f) {
    if (f.size() < 4) return false;
    std::string e = f.substr(f.size() - 4);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    return e == ".wav";
}

// duration in seconds from the WAV header (fmt + data chunks), or nullopt
static std::optional<double> wav_duration(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    char h[12];
    if (!f.read(h, 12) || std::memcmp(h, "RIFF", 4) || std::memcmp(h + 8, "WAVE", 4)) return std::nullopt;
    unsigned rate = 0, block = 0;
    for (;;) {
        char ck[8];
        if (!f.read(ck, 8)) return std::nullopt;
        uint32_t sz;
        std::memcpy(&sz, ck + 4, 4);
        if (!std::memcmp(ck, "fmt ", 4)) {
            char fm[16];
            if (sz < 16 || !f.read(fm, 16)) return std::nullopt;
            uint16_t ch, bits;
            std::memcpy(&ch, fm + 2, 2); std::memcpy(&rate, fm + 4, 4); std::memcpy(&bits, fm + 14, 2);
            block = ch * (bits / 8);
            f.seekg((long)sz - 16 + (sz & 1), std::ios::cur);
        } else if (!std::memcmp(ck, "data", 4)) {
            if (!rate || !block) return std::nullopt;
            return (double)(sz / block) / rate;
        } else {
            f.seekg((long)sz + (sz & 1), std::ios::cur);
        }
    }
}

static json list_recordings() {
    std::vector<json> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(REC_DIR, ec)) {
        std::string fn = e.path().filename().string();
        if (!ends_wav(fn)) continue;
        struct stat st {};
        if (::stat(e.path().c_str(), &st) != 0) continue;
        auto pos = fn.find('_');
        auto d = wav_duration(e.path().string());
        out.push_back({{"file", fn}, {"band", pos != std::string::npos ? fn.substr(0, pos) : "?"},
                       {"bytes", (long long)st.st_size}, {"mtime", (double)st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9},
                       {"duration", d ? json(round_to(*d, 2)) : json(nullptr)}});
    }
    std::sort(out.begin(), out.end(), [](const json& a, const json& b) { return a["mtime"].get<double>() > b["mtime"].get<double>(); });
    return json(out);
}

static json recordings_header(const json* items = nullptr) {
    json all = items ? *items : list_recordings();
    json head = json::array();
    for (size_t i = 0; i < all.size() && i < REC_LIST_PUBLISH_MAX; ++i) head.push_back(all[i]);
    return {{"total", (long long)all.size()}, {"items", head}};
}

static std::optional<std::string> recording_path(const std::string& fn) {      // a bare .wav name inside REC_DIR only
    std::string safe = fs::path(fn).filename().string();
    if (!ends_wav(safe)) return std::nullopt;
    return (fs::path(REC_DIR) / safe).string();
}

struct Recorder {
    FILE* f = nullptr;
    std::string path;
    double started = 0, peak_db = -200.0;
    long long voiced = 0, frames = 0;
    bool had_audio = false;
    int rate = 0;
};
static std::map<std::string, Recorder> g_rec;       // band -> open recording
static std::mutex g_rec_mu;

static void wav_patch(Recorder& r) {                 // keep a valid header after every write (a crash leaves a playable file)
    uint32_t data = (uint32_t)(r.frames * 2), riff = 36 + data;
    std::fseek(r.f, 4, SEEK_SET); std::fwrite(&riff, 4, 1, r.f);
    std::fseek(r.f, 40, SEEK_SET); std::fwrite(&data, 4, 1, r.f);
    std::fseek(r.f, 0, SEEK_END);
}

static Recorder& open_recording(const std::string& band, int rate) {
    char ts[32];
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    std::strftime(ts, sizeof ts, "%Y%m%dT%H%M%S", &tmv);
    Recorder r;
    r.path = (fs::path(REC_DIR) / (band + "_" + ts + ".wav")).string();
    r.f = std::fopen(r.path.c_str(), "wb");
    r.started = now_s();
    r.rate = rate;
    uint32_t v;
    uint16_t s;
    std::fwrite("RIFF", 1, 4, r.f); v = 36; std::fwrite(&v, 4, 1, r.f); std::fwrite("WAVEfmt ", 1, 8, r.f);
    v = 16; std::fwrite(&v, 4, 1, r.f); s = 1; std::fwrite(&s, 2, 1, r.f); s = 1; std::fwrite(&s, 2, 1, r.f);
    v = rate; std::fwrite(&v, 4, 1, r.f); v = rate * 2; std::fwrite(&v, 4, 1, r.f); s = 2; std::fwrite(&s, 2, 1, r.f);
    s = 16; std::fwrite(&s, 2, 1, r.f); std::fwrite("data", 1, 4, r.f); v = 0; std::fwrite(&v, 4, 1, r.f);
    return g_rec[band] = r;
}

// closes the band's recording; returns {path, duration} or nullopt. Caller holds g_rec_mu.
static std::optional<std::pair<std::string, double>> close_recording(const std::string& band) {
    auto it = g_rec.find(band);
    if (it == g_rec.end()) return std::nullopt;
    Recorder r = it->second;
    g_rec.erase(it);
    wav_patch(r);
    std::fclose(r.f);
    return std::make_pair(r.path, round_to(now_s() - r.started, 1));
}

// ------------------------------------------------------------------ IQ: spectrum, activity, channel power
static std::vector<double> g_hann(FFT_N);
static std::map<std::string, std::deque<double>> g_hist;
static std::map<std::string, double> g_ref_db, g_floor_db;
static std::map<std::string, bool> g_has_floor;

static double percentile(const std::deque<double>& h, double p) {            // numpy's default (linear) method
    std::vector<double> v(h.begin(), h.end());
    std::sort(v.begin(), v.end());
    double pos = p / 100.0 * (v.size() - 1);
    size_t lo = (size_t)std::floor(pos), hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (pos - lo);
}

static bool band_active(const std::string& key, double rms_db) {
    if (rms_db <= -110.0) return false;
    auto& hist = g_hist[key];
    hist.push_back(rms_db);
    if (hist.size() > NOISE_WIN) hist.pop_front();
    if (hist.size() >= CROSS_BAND_MIN_FRAMES) g_ref_db[key] = percentile(hist, 5);
    std::optional<double> lowest;                                             // quietest OTHER band
    for (auto& [k, v] : g_ref_db) if (k != key && (!lowest || v < *lowest)) lowest = v;
    if (hist.size() < NOISE_MIN_FRAMES) {
        double thr = ACTIVE_THRESH_DBFS;
        if (lowest) thr = std::min(thr, *lowest + CROSS_BAND_ALLOW_DB + NOISE_MARGIN_DB);
        return rms_db > thr;
    }
    double floor = percentile(hist, 5);
    g_floor_db[key] = floor;
    g_has_floor[key] = true;
    if (lowest) floor = std::min(floor, *lowest + CROSS_BAND_ALLOW_DB);
    return rms_db > floor + NOISE_MARGIN_DB;
}

static void fft(std::vector<std::complex<double>>& a) {                      // radix-2, in place
    size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = -2 * M_PI / len;
        std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// per-channelizer-channel power (dB): channel k is centred at k*fs/N for k <= N/2, (k-N)*fs/N above (pfb.channelizer_ccf)
static std::vector<float> channel_powers(const std::vector<double>& spec_pow, int n_chan) {
    if (n_chan <= 1) {
        double s = 0;
        for (double v : spec_pow) s += v;
        return {(float)(10 * std::log10(std::max(s, 1e-18)))};
    }
    int nb = (int)spec_pow.size();
    double bpc = (double)nb / n_chan;
    std::vector<float> out;
    for (int k = 0; k < n_chan; ++k) {
        int off = k <= n_chan / 2 ? k : k - n_chan;
        double centre = nb / 2.0 + off * bpc;
        int lo = (int)std::nearbyint(centre - bpc / 2), hi = (int)std::nearbyint(centre + bpc / 2);
        double s = 0;
        for (int i = lo; i < hi; ++i) s += spec_pow[((i % nb) + nb) % nb];
        out.push_back((float)(10 * std::log10(std::max(s, 1e-18))));
    }
    return out;
}

static void process_chunk(size_t bi, const cf* x) {
    const Band& b = BANDS[bi];
    double ms = 0;
    for (int i = 0; i < FFT_N; ++i) ms += (double)std::norm(x[i]);
    double rms = std::sqrt(ms / FFT_N);
    double rms_db = rms > 1e-12 ? 20 * std::log10(rms) : -120.0;
    bool active = band_active(b.key, rms_db);

    std::vector<std::complex<double>> w(FFT_N);
    for (int i = 0; i < FFT_N; ++i) w[i] = std::complex<double>(x[i].real(), x[i].imag()) * g_hann[i];
    fft(w);
    std::vector<double> pw(FFT_N);                       // fftshift: index 0 = -fs/2
    for (int i = 0; i < FFT_N; ++i) pw[(i + FFT_N / 2) % FFT_N] = std::norm(w[i]);

    auto chans = channel_powers(pw, b.channels);
    int best = (int)(std::max_element(chans.begin(), chans.end()) - chans.begin());
    std::vector<float> sorted(chans);
    std::sort(sorted.begin(), sorted.end());
    size_t m = sorted.size();
    double noise = m % 2 ? sorted[m / 2] : 0.5 * ((double)sorted[m / 2 - 1] + sorted[m / 2]);
    json occupied = json::array();
    if (active)
        for (size_t i = 0; i < chans.size(); ++i) if (chans[i] - noise > CHAN_SNR_DB) occupied.push_back((int)i);

    std::vector<float> payload;
    for (int i = 0; i < FFT_N; i += 2) payload.push_back((float)(10 * std::log10(std::max(pw[i], 1e-18))));   // 256 bins
    for (int i = 0; i < 64; ++i) payload.push_back(x[i * 4].real());                                           // 64 time samples
    for (float c : chans) payload.push_back(c);
    json hdr = {{"band", b.key}, {"active", active}, {"rms_db", round_to(rms_db, 1)},
                {"floor_db", g_has_floor[b.key] ? json(round_to(g_floor_db[b.key], 1)) : json(nullptr)},
                {"n_spec", 256}, {"n_time", 64}, {"n_chan", (int)chans.size()},
                {"chan_best", active ? best : -1},
                {"chan_best_db", active ? json(round_to(chans[best] - noise, 1)) : json(nullptr)},
                {"chan_occupied", occupied}, {"chan_bw_hz", (int)(SAMP_RATE / b.channels)},
                {"payload", "float32le: spectrum[n_spec], time[n_time], chan_db[n_chan]"}};
    PUB->publish("terr/iq/" + b.key, hdr, payload.data(), payload.size() * 4);
}

static void iq_thread(void* ctx) {
    for (int i = 0; i < FFT_N; ++i) g_hann[i] = 0.5 - 0.5 * std::cos(2 * M_PI * i / (FFT_N - 1));   // numpy.hanning
    void* sub = zmq_socket(ctx, ZMQ_SUB);
    int hwm = 500, linger = 0;
    zmq_setsockopt(sub, ZMQ_RCVHWM, &hwm, sizeof hwm);
    zmq_setsockopt(sub, ZMQ_LINGER, &linger, sizeof linger);
    for (auto& b : BANDS) { std::string t = "iq/" + b.key + "/"; zmq_setsockopt(sub, ZMQ_SUBSCRIBE, t.data(), t.size()); }
    zmq_connect(sub, IQ_ADDR.c_str());
    std::vector<std::vector<cf>> buf(BANDS.size());
    std::vector<double> last_send(BANDS.size(), 0.0);
    zmq_pollitem_t it{sub, 0, ZMQ_POLLIN, 0};
    while (!g_stop) {
        if (zmq_poll(&it, 1, 200) <= 0) continue;
        std::vector<std::string> fr;
        for (;;) {
            zmq_msg_t m;
            zmq_msg_init(&m);
            if (zmq_msg_recv(&m, sub, 0) < 0) { zmq_msg_close(&m); break; }
            fr.emplace_back((const char*)zmq_msg_data(&m), zmq_msg_size(&m));
            int more = zmq_msg_more(&m);
            zmq_msg_close(&m);
            if (!more) break;
        }
        if (fr.size() != 3 || fr[2].size() % 8 || fr[2].empty()) continue;             // malformed: never feed it to the FFT
        const std::string& topic = fr[0];
        size_t bi = BANDS.size();
        for (size_t i = 0; i < BANDS.size(); ++i) if (topic == "iq/" + BANDS[i].key + "/") bi = i;
        if (bi == BANDS.size()) continue;
        LAST_IQ_RX[bi] = now_s();
        auto& bf = buf[bi];
        size_t n = fr[2].size() / 8, old = bf.size();
        bf.resize(old + n);
        std::memcpy(bf.data() + old, fr[2].data(), fr[2].size());
        size_t used = 0;
        while (bf.size() - used >= (size_t)FFT_N) {
            double now = now_s();
            if (now - last_send[bi] >= IQ_FRAME_GAP_S) { last_send[bi] = now; process_chunk(bi, bf.data() + used); }
            used += FFT_N;
        }
        bf.erase(bf.begin(), bf.begin() + used);
    }
    zmq_close(sub);
}

// ------------------------------------------------------------------ decoded audio taps, recorder
static void audio_thread(void* ctx) {
    std::vector<void*> socks;
    std::vector<zmq_pollitem_t> items;
    for (auto& b : BANDS) {
        void* s = zmq_socket(ctx, ZMQ_SUB);
        int hwm = 5000, linger = 0;
        zmq_setsockopt(s, ZMQ_RCVHWM, &hwm, sizeof hwm);
        zmq_setsockopt(s, ZMQ_LINGER, &linger, sizeof linger);
        zmq_setsockopt(s, ZMQ_SUBSCRIBE, "", 0);
        zmq_connect(s, b.tap.c_str());
        socks.push_back(s);
        items.push_back({s, 0, ZMQ_POLLIN, 0});
    }
    std::vector<double> voice_last(BANDS.size(), 0.0);
    std::vector<char> raw;
    while (!g_stop) {
        if (zmq_poll(items.data(), (int)items.size(), 200) <= 0) continue;
        for (size_t bi = 0; bi < BANDS.size(); ++bi) {
            while (items[bi].revents & ZMQ_POLLIN) {
                zmq_msg_t m;
                zmq_msg_init(&m);
                if (zmq_msg_recv(&m, socks[bi], ZMQ_DONTWAIT) < 0) { zmq_msg_close(&m); break; }
                size_t nb = zmq_msg_size(&m) / 4;
                std::vector<float> s(nb);
                std::memcpy(s.data(), zmq_msg_data(&m), nb * 4);
                zmq_msg_close(&m);
                if (s.empty()) continue;
                const Band& b = BANDS[bi];
                double ss = 0;
                long long voiced = 0;
                for (float v : s) { ss += (double)v * v; if (std::fabs(v) > 1e-4f) ++voiced; }   // DSD emits exact silence unless it decoded voice
                double rms = std::sqrt(ss / s.size());
                double rms_db = rms > 1e-9 ? 20 * std::log10(rms) : -120.0;
                bool hot = rms_db > REC_ON_DBFS;
                double now = now_s();
                if (voiced > 0) voice_last[bi] = now;
                bool in_dwell = (now - voice_last[bi]) < VOICE_HANG_S;
                std::vector<int16_t> pcm(s.size());
                for (size_t i = 0; i < s.size(); ++i) pcm[i] = (int16_t)std::clamp((double)s[i] * 32767.0, -32768.0, 32767.0);

                {
                    std::lock_guard<std::mutex> lk(g_rec_mu);
                    if (in_dwell) {
                        auto it = g_rec.find(b.key);
                        if (it == g_rec.end()) { open_recording(b.key, b.audio_rate); it = g_rec.find(b.key); }
                        Recorder& r = it->second;
                        std::fwrite(pcm.data(), 2, pcm.size(), r.f);
                        r.frames += (long long)pcm.size();
                        wav_patch(r);
                        r.voiced += voiced;
                        if (hot) r.had_audio = true;
                        r.peak_db = std::max(r.peak_db, rms_db);
                    } else if (g_rec.count(b.key)) {
                        Recorder r = g_rec[b.key];
                        double voice_s = (double)r.voiced / b.audio_rate;
                        auto info = close_recording(b.key);
                        // keep a recording only when DSD decoded voice in it
                        if (info && voice_s < REC_MIN_VOICE_S) { std::remove(info->first.c_str()); info.reset(); }
                        if (info && info->second >= 0.2) {
                            std::string fn = fs::path(info->first).filename().string();
                            PUB->publish("terr/recording", {{"band", b.key}, {"file", fn}, {"duration", info->second},
                                         {"had_audio", r.had_audio}, {"peak_db", round_to(r.peak_db, 1)}, {"t", now_s()}});
                            char txt[256];
                            std::snprintf(txt, sizeof txt, "  \xe2\x86\xb3 saved %s (%.1fs, %.1f s decoded voice, peak %.1f dBFS)",
                                          fn.c_str(), info->second, voice_s, r.peak_db);
                            log_event(b.key.c_str(), txt);
                        } else if (info) {
                            std::remove(info->first.c_str());
                        }
                    }
                }
                int step = b.audio_rate >= 44100 ? 2 : 1;
                std::vector<int16_t> out;
                for (size_t i = 0; i < pcm.size(); i += step) out.push_back(pcm[i]);
                PUB->publish("terr/audio/" + b.key, {{"band", b.key}, {"rate", b.audio_rate / step}, {"active", voiced > 0},
                             {"rms_db", round_to(rms_db, 1)}, {"n", (int)out.size()}, {"payload", "int16le mono"}},
                             out.data(), out.size() * 2);
            }
        }
    }
    for (auto s : socks) zmq_close(s);
}

// ------------------------------------------------------------------ TETRA call-log events (PMT)
static json pmt_to_json(const pmt::pmt_t& p) {
    if (pmt::is_null(p)) return nullptr;
    if (pmt::is_bool(p)) return pmt::to_bool(p);
    if (pmt::is_symbol(p)) return pmt::symbol_to_string(p);
    if (pmt::is_integer(p)) return pmt::to_long(p);
    if (pmt::is_uint64(p)) return pmt::to_uint64(p);
    if (pmt::is_real(p)) return pmt::to_double(p);
    if (pmt::is_dict(p)) {
        json o = json::object();
        pmt::pmt_t items = pmt::dict_items(p);
        for (size_t i = 0; i < pmt::length(items); ++i) {
            pmt::pmt_t kv = pmt::nth(i, items);
            json k = pmt_to_json(pmt::car(kv));
            o[k.is_string() ? k.get<std::string>() : k.dump()] = pmt_to_json(pmt::cdr(kv));
        }
        return o;
    }
    if (pmt::is_tuple(p)) {
        json a = json::array();
        for (size_t i = 0; i < pmt::length(p); ++i) a.push_back(pmt_to_json(pmt::tuple_ref(p, i)));
        return a;
    }
    if (pmt::is_vector(p)) {
        json a = json::array();
        for (size_t i = 0; i < pmt::length(p); ++i) a.push_back(pmt_to_json(pmt::vector_ref(p, i)));
        return a;
    }
    if (pmt::is_pair(p)) return json::array({pmt_to_json(pmt::car(p)), pmt_to_json(pmt::cdr(p))});
    return pmt::write_string(p);
}

static void tetra_thread(void* ctx) {
    void* sub = zmq_socket(ctx, ZMQ_SUB);
    int linger = 0;
    zmq_setsockopt(sub, ZMQ_LINGER, &linger, sizeof linger);
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, "", 0);
    zmq_connect(sub, TETRA_EVENTS_ADDR);
    zmq_pollitem_t it{sub, 0, ZMQ_POLLIN, 0};
    std::vector<char> buf(1 << 16);
    while (!g_stop) {
        if (zmq_poll(&it, 1, 200) <= 0) continue;
        int n = zmq_recv(sub, buf.data(), buf.size(), 0);
        if (n <= 0) continue;
        try {
            pmt::pmt_t msg = pmt::deserialize_str(std::string(buf.data(), (size_t)std::min<size_t>((size_t)n, buf.size())));
            json d = pmt_to_json(msg);
            PUB->publish("terr/tetra", {{"data", d}, {"t", now_s()}});
            log_event("tetra", "[TETRA] " + d.dump());
        } catch (const std::exception& e) {
            log_event("tetra", std::string("[BRIDGE] tetra event undecodable: ") + e.what());
        }
    }
    zmq_close(sub);
}

// ------------------------------------------------------------------ decoder process
static void decoder_reader(int fd, pid_t pid, int gen) {
    FILE* f = fdopen(fd, "r");
    std::deque<std::string> last;
    char line[8192];
    while (std::fgets(line, sizeof line, f)) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.find_first_not_of(" \t") == std::string::npos) continue;
        last.push_back(l);
        if (last.size() > 5) last.pop_front();
        log_event(nullptr, "[M2] " + l);
    }
    std::fclose(f);
    bool mine;
    {
        std::lock_guard<std::mutex> lk(g_proc_mu);
        mine = (g_dec_pid == pid && g_dec_gen == gen);
    }
    if (!mine) return;                                // stopped on purpose: stop_decoder reports it
    int st = 0;
    waitpid(pid, &st, 0);
    {
        std::lock_guard<std::mutex> lk(g_proc_mu);
        if (g_dec_pid == pid) g_dec_pid = 0;
    }
    std::string tail;
    for (size_t i = last.size() > 3 ? last.size() - 3 : 0; i < last.size(); ++i) tail += (tail.empty() ? "" : " | ") + last[i];
    log_event(nullptr, "[BRIDGE] allinone.py exited on its own (exit code " + std::to_string(WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st)) +
                           "); last output: " + tail.substr(0, 300));
    PUB->publish("terr/decoder", {{"running", false}});
}

static bool start_decoder() {
    if (decoder_running()) return true;
    // SECURECOM_TERR_DECODER_BIN = the C++ decoder (securecom_terr_decoder); without it the Python flowgraph allinone.py is started
    const char* bin = std::getenv("SECURECOM_TERR_DECODER_BIN");
    std::string py = (bin && *bin) ? std::string(bin) : (fs::path(DECODER_DIR) / "allinone.py").string();
    if (!fs::exists(py)) { log_event(nullptr, "[BRIDGE] ERROR: " + py + " not found"); return false; }
    int p[2];
    if (pipe(p) != 0) return false;
    pid_t pid = fork();
    if (pid == 0) {                                   // child: headless (no display on the edge board)
        setsid();
        dup2(p[1], 1); dup2(p[1], 2); close(p[0]); close(p[1]);
        if (env("SECURECOM_DECODER_GUI", "") != "1") setenv("QT_QPA_PLATFORM", "offscreen", 1);
        if (chdir(DECODER_DIR.c_str()) != 0) _exit(127);
        if (bin && *bin) execl(py.c_str(), py.c_str(), (char*)nullptr);
        else execlp("python3", "python3", py.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(p[1]);
    int gen;
    {
        std::lock_guard<std::mutex> lk(g_proc_mu);
        g_dec_pid = pid;
        gen = ++g_dec_gen;
    }
    log_event(nullptr, "[BRIDGE] started allinone.py / Module 2 (pid " + std::to_string(pid) + ")");
    PUB->publish("terr/decoder", {{"running", true}});
    std::thread(decoder_reader, p[0], pid, gen).detach();
    return true;
}

static void stop_decoder() {
    pid_t pid;
    {
        std::lock_guard<std::mutex> lk(g_proc_mu);
        pid = g_dec_pid;
        g_dec_pid = 0;                                // the reader sees it is no longer current
    }
    if (pid > 0) {
        kill(pid, SIGTERM);
        int st = 0;
        bool done = false;
        for (int i = 0; i < 50 && !done; ++i) { done = waitpid(pid, &st, WNOHANG) == pid; if (!done) usleep(100000); }
        if (!done) { kill(pid, SIGKILL); waitpid(pid, &st, 0); }
    }
    {
        std::lock_guard<std::mutex> lk(g_rec_mu);
        std::vector<std::string> keys;
        for (auto& [k, v] : g_rec) keys.push_back(k);
        for (auto& k : keys) close_recording(k);
    }
    log_event(nullptr, "[BRIDGE] stopped allinone.py / Module 2");
    PUB->publish("terr/decoder", {{"running", false}});
}

// ------------------------------------------------------------------ heartbeat + commands
static void heartbeat_thread() {
    int n = 0;
    while (!g_stop) {
        PUB->publish("terr/status", status_snapshot());
        if (n % 5 == 0) {
            PUB->publish("terr/bands", {{"bands", bands_info()}});
            PUB->publish("terr/recordings", recordings_header());
        }
        ++n;
        for (int i = 0; i < 20 && !g_stop; ++i) usleep(100000);
    }
}

using Reply = std::pair<json, std::string>;

static void publish_recordings() { PUB->publish("terr/recordings", recordings_header()); }

static Reply handle(const json& req) {
    std::string cmd = req.value("cmd", std::string());
    if (cmd == "get_state") return {{{"interface_version", "1.0"}, {"status", status_snapshot()}, {"bands", bands_info()}, {"recordings_total", (long long)list_recordings().size()}}, ""};
    if (cmd == "start_decoder") { start_decoder(); return {{{"decoder_running", decoder_running()}}, ""}; }
    if (cmd == "stop_decoder") { stop_decoder(); return {{{"decoder_running", decoder_running()}}, ""}; }
    if (cmd == "list_recordings") {
        json items = list_recordings();
        long long off = req.value("offset", 0LL), lim = req.value("limit", 200LL);
        json page = json::array();
        for (long long i = off; i < (long long)items.size() && i < off + lim; ++i) if (i >= 0) page.push_back(items[i]);
        return {{{"total", (long long)items.size()}, {"offset", off}, {"items", page}}, ""};
    }
    if (cmd == "get_recording") {
        auto p = recording_path(req.value("file", std::string()));
        if (!p || !fs::is_regular_file(*p)) return {{{"ok", false}, {"error", "no such recording"}}, ""};
        long long off = req.value("offset", 0LL), len = std::min<long long>(req.value("length", 262144LL), 1 << 20);
        long long size = (long long)fs::file_size(*p);
        std::ifstream f(*p, std::ios::binary);
        f.seekg(off);
        std::string data((size_t)std::max<long long>(len, 0), '\0');
        f.read(data.data(), (std::streamsize)data.size());
        data.resize((size_t)f.gcount());
        return {{{"file", fs::path(*p).filename().string()}, {"size", size}, {"offset", off}, {"length", (long long)data.size()},
                 {"eof", off + (long long)data.size() >= size}}, data};
    }
    if (cmd == "delete_recording") {
        std::string fn = req.value("file", std::string());
        auto p = recording_path(fn);
        if (!p) return {{{"ok", false}, {"error", "refused: not a .wav inside the recordings folder"}}, ""};
        std::string safe = fs::path(*p).filename().string();
        std::error_code ec;
        bool ok = fs::remove(*p, ec);
        log_event(nullptr, ok ? "[BRIDGE] deleted " + safe : "[BRIDGE] delete failed " + safe + ": " + (ec ? ec.message() : "No such file"));
        publish_recordings();
        return {{{"file", safe}, {"deleted", !fs::exists(*p)}}, ""};
    }
    if (cmd == "delete_all_recordings") {
        json items = list_recordings();
        long long n = 0;
        for (auto& it : items) { std::error_code ec; if (fs::remove(fs::path(REC_DIR) / it["file"].get<std::string>(), ec)) ++n; }
        log_event(nullptr, "[BRIDGE] deleted " + std::to_string(n) + " recording(s)");
        publish_recordings();
        return {{{"deleted", n}}, ""};
    }
    json known = json::array({"delete_all_recordings", "delete_recording", "get_recording", "get_state", "list_recordings", "start_decoder", "stop_decoder"});
    return {{{"ok", false}, {"error", "unknown cmd '" + cmd + "'"}, {"known", known}}, ""};
}

static void command_thread(void* ctx) {
    void* rep = zmq_socket(ctx, ZMQ_REP);
    int linger = 0;
    zmq_setsockopt(rep, ZMQ_LINGER, &linger, sizeof linger);
    if (zmq_bind(rep, CMD_ADDR.c_str()) != 0) { std::fprintf(stderr, "bind %s: %s\n", CMD_ADDR.c_str(), zmq_strerror(zmq_errno())); std::exit(1); }
    zmq_pollitem_t it{rep, 0, ZMQ_POLLIN, 0};
    std::vector<char> buf(1 << 16);
    while (!g_stop) {
        if (zmq_poll(&it, 1, 200) <= 0) continue;
        int n = zmq_recv(rep, buf.data(), buf.size(), 0);
        while (n >= 0) { int more = 0; size_t ms = sizeof more; zmq_getsockopt(rep, ZMQ_RCVMORE, &more, &ms); if (!more) break; zmq_recv(rep, buf.data(), buf.size(), 0); }
        Reply r;
        try {
            json req = json::parse(std::string(buf.data(), (size_t)std::max(n, 0)));
            r = handle(req);
        } catch (const std::exception& e) {                  // a bad request must never take the backend down
            r = {{{"ok", false}, {"error", std::string("Error: ") + e.what()}}, ""};
        }
        r.first.emplace("ok", true);
        std::string h = r.first.dump();
        zmq_send(rep, h.data(), h.size(), ZMQ_SNDMORE);
        zmq_send(rep, r.second.data(), r.second.size(), 0);
    }
    zmq_close(rep);
}

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    PUB_ADDR = env("SECURECOM_PUB_ADDR", "tcp://127.0.0.1:5560");
    CMD_ADDR = env("SECURECOM_CMD_ADDR", "tcp://127.0.0.1:5561");
    IQ_ADDR = env("SECURECOM_IQ_PUB_ADDR", "tcp://127.0.0.1:5570");
    REC_DIR = env("SECURECOM_RECORDINGS_DIR", "recordings");
    DECODER_DIR = env("SECURECOM_DECODER_DIR", ".");
    fs::create_directories(REC_DIR);
    for (auto& a : LAST_IQ_RX) a = 0.0;

    void* ctx = zmq_ctx_new();
    PUB = new Pub(ctx, PUB_ADDR);
    std::printf("ZeroMQ: PUB %s (topics terr/...), REP %s (commands); IQ from %s; recordings %s\n",
                PUB_ADDR.c_str(), CMD_ADDR.c_str(), IQ_ADDR.c_str(), REC_DIR.c_str());
    std::fflush(stdout);
    std::vector<std::thread> th;
    th.emplace_back(iq_thread, ctx);
    th.emplace_back(audio_thread, ctx);
    th.emplace_back(tetra_thread, ctx);
    th.emplace_back(heartbeat_thread);
    th.emplace_back(command_thread, ctx);
    while (!g_stop) usleep(200000);
    for (auto& t : th) t.join();
    stop_decoder();
    PUB->close();
    zmq_ctx_term(ctx);
    return 0;
}
