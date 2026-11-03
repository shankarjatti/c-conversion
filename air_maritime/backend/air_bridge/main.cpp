// Air & maritime bridge (C++ port of airmaritime/airmaritime_bridge.py): the backend side of the AIS + ADS-B module.
// ZeroMQ only (no WebSocket, no Module 1 control).
//
//   decoder   SUB  tcp://127.0.0.1:9201 AIS sentences (PMT), :9202 gr-adsb decoded dicts (PMT), :9203 raw CRC-valid Mode-S frames
//   IQ        SUB  SECURECOM_IQ_PUB_ADDR (the Ingest), topics iq/ais/ and iq/adsb/   -> spectrum
//   results   PUB  SECURECOM_AIR_PUB_ADDR (default tcp://127.0.0.1:5565), topics air/...  (see docs/ZMQ_INTERFACE.md)
//   commands  REP  SECURECOM_AIR_CMD_ADDR (default tcp://127.0.0.1:5566)
//
// It also starts/stops the decoder (SECURECOM_DECODER_DIR/airmaritime_decoder_live.py until the decoder itself is C++).
#include "common.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstring>
#include <deque>
#include <filesystem>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

using namespace sc;
namespace fs = std::filesystem;
using cf = std::complex<float>;

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop = true; }

// ------------------------------------------------------------------ configuration
static const double ADSB_CONFIRM_S = 10.0, AIS_CONFIRM_S = 600.0, ADSB_STALE_S = 60.0, AIS_STALE_S = 600.0;
static const int IQ_FFT_N = 512;
static const double SPEC_WINDOW_S = 0.1, IQ_ACTIVE_THRESH_DBFS = -60.0, IQ_TIMEOUT_S = 3.0;
struct Band { std::string key, name; double freq_mhz, samp_rate_hz; };
static const std::vector<Band> BANDS = {{"ais", "AIS", 162.000, 250000.0}, {"adsb", "ADS-B", 1090.000, 6000000.0}};
static std::string PUB_ADDR, CMD_ADDR, IQ_ADDR, DECODER_DIR, DECODER_BIN;
static int DEC_PORTS[3] = {9201, 9202, 9203};          // decoder message sockets: AIS, ADS-B decoded, ADS-B raw frames
static Pub* PUB = nullptr;
static std::atomic<double> LAST_IQ_RX[2];

// ------------------------------------------------------------------ targets (shared state)
static std::mutex g_mu;
static std::map<std::string, json> AIS_VESSELS, ADSB_AIRCRAFT;
static std::map<std::string, double> AIS_FIRST_MSG, ADSB_FIRST_FRAME;
static std::set<std::string> AIS_CONFIRMED, ADSB_CONFIRMED;
static std::map<std::string, double> AIS_SEEN;

// ------------------------------------------------------------------ AIS (ITU-R M.1371) decode
static const char* AIS_CHARS = "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_ !\"#$%&'()*+,-./0123456789:;<=>?";

static std::vector<int> armor_to_bits(const std::string& payload) {
    std::vector<int> bits;
    for (unsigned char ch : payload) {
        int c = (int)ch - 48;
        if (c > 40) c -= 8;
        for (int i = 0; i < 6; ++i) bits.push_back((c >> (5 - i)) & 1);
    }
    return bits;
}
static long long bits_uint(const std::vector<int>& b, size_t start, size_t len) {
    long long v = 0;
    for (size_t i = start; i < start + len && i < b.size(); ++i) v = (v << 1) | b[i];
    return v;
}
static long long bits_int(const std::vector<int>& b, size_t start, size_t len) {
    long long v = bits_uint(b, start, len);
    if (b[start] == 1) v -= (1LL << len);
    return v;
}
static std::string bits_text(const std::vector<int>& b, size_t start, size_t len) {
    std::string out;
    for (size_t i = start; i < start + len; i += 6) {
        if (i + 6 > b.size()) break;
        long long v = bits_uint(b, i, 6);
        out += v < 64 ? AIS_CHARS[v] : '?';
    }
    while (!out.empty() && out.back() == '@') out.pop_back();
    size_t a = out.find_first_not_of(" \t\n\r"), z = out.find_last_not_of(" \t\n\r");
    return a == std::string::npos ? "" : out.substr(a, z - a + 1);
}

static std::optional<json> parse_aivdm(const std::string& payload) {
    auto bits = armor_to_bits(payload);
    if (bits.size() < 38) return std::nullopt;
    long long mt = bits_uint(bits, 0, 6);
    json out = {{"msg_type", mt}, {"mmsi", bits_uint(bits, 8, 30)}};
    auto pos_fields = [&](size_t lon0, size_t lat0, size_t sog0, size_t cog0, size_t hdg0) {
        double lon = bits_int(bits, lon0, 28) / 600000.0, lat = bits_int(bits, lat0, 27) / 600000.0;
        long long sog = bits_uint(bits, sog0, 10), cog = bits_uint(bits, cog0, 12), hdg = bits_uint(bits, hdg0, 9);
        out["speed"] = sog != 1023 ? json(sog / 10.0) : json(nullptr);
        out["lon"] = (std::fabs(lon) <= 180 && lon != 181.0) ? json(lon) : json(nullptr);
        out["lat"] = (std::fabs(lat) <= 90 && lat != 91.0) ? json(lat) : json(nullptr);
        out["course"] = cog != 3600 ? json(cog / 10.0) : json(nullptr);
        out["heading"] = hdg != 511 ? json(hdg) : json(nullptr);
    };
    if ((mt == 1 || mt == 2 || mt == 3) && bits.size() >= 143) {
        out["nav_status"] = bits_uint(bits, 38, 4);
        pos_fields(61, 89, 50, 116, 128);
    } else if (mt == 18 && bits.size() >= 139) {
        pos_fields(57, 85, 46, 112, 124);
    } else if (mt == 5 && bits.size() >= 422) {                 // static & voyage data: callsign/name, never a position
        std::string cs = bits_text(bits, 70, 42), nm = bits_text(bits, 112, 120);
        out["callsign"] = cs.empty() ? json(nullptr) : json(cs);
        out["name"] = nm.empty() ? json(nullptr) : json(nm);
    }
    return out;
}

// Only overwrites what the message carried; a vessel is shown after 2 messages within AIS_CONFIRM_S. Caller holds g_mu.
static std::optional<json> update_ais_target(const json& p) {
    std::string mmsi = std::to_string(p["mmsi"].get<long long>());
    double t = now_s();
    if (!AIS_CONFIRMED.count(mmsi)) {
        auto it = AIS_FIRST_MSG.find(mmsi);
        if (it == AIS_FIRST_MSG.end() || t - it->second > AIS_CONFIRM_S) { AIS_FIRST_MSG[mmsi] = t; return std::nullopt; }
        AIS_FIRST_MSG.erase(it);
        AIS_CONFIRMED.insert(mmsi);
    }
    json& v = AIS_VESSELS.try_emplace(mmsi, json{{"mmsi", mmsi}}).first->second;
    for (const char* f : {"lat", "lon", "speed", "course", "heading", "nav_status", "callsign", "name"})
        if (p.contains(f) && !p[f].is_null()) v[f] = p[f];
    v["last_seen"] = t;
    v["msg_type"] = p["msg_type"];
    return v;
}

// ------------------------------------------------------------------ ADS-B CPR (ICAO Doc 9871), from adsb_cpr.py
namespace cpr {
static const double PAIR_WINDOW_S = 10.0, CONFIRM_WINDOW_S = 60.0, REF_MAX_AGE_S = 60.0, MAX_SPEED_KT = 1000.0, MARGIN_KM = 3.0;
static const int REJECT_LIMIT = 3;
static double pymod(double a, double b) { return a - b * std::floor(a / b); }   // Python's % (sign of the divisor)
static long long pymod_i(long long a, long long b) { long long r = a % b; return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r; }

static int nl(double lat) {
    if (lat == 0) return 59;
    if (std::fabs(lat) == 87) return 2;
    if (std::fabs(lat) > 87) return 1;
    double a = 1 - std::cos(M_PI / (2 * 15)), b = std::pow(std::cos(M_PI / 180 * std::fabs(lat)), 2);
    return (int)std::floor(2 * M_PI / std::acos(1 - a / b));
}
static double dist_km(double lat1, double lon1, double lat2, double lon2) {
    double p1 = lat1 * M_PI / 180, p2 = lat2 * M_PI / 180, dp = p2 - p1, dl = (lon2 - lon1) * M_PI / 180;
    double h = std::pow(std::sin(dp / 2), 2) + std::cos(p1) * std::cos(p2) * std::pow(std::sin(dl / 2), 2);
    return 2 * 6371.0 * std::asin(std::sqrt(h));
}
static double reachable_km(double dt) { return MAX_SPEED_KT * 1.852 / 3600.0 * std::fabs(dt) + MARGIN_KM; }

struct Fr { double lat = 0, lon = 0, t = 0; bool set = false; };
struct Pos { double lat, lon; };

static std::optional<Pos> global_decode(const Fr& ev, const Fr& od, int newest) {
    double le = ev.lat / 131072.0, lo = od.lat / 131072.0, ne = ev.lon / 131072.0, no = od.lon / 131072.0;
    long long j = (long long)std::floor(59 * le - 60 * lo + 0.5);
    double lat_e = 360.0 / 60 * (pymod_i(j, 60) + le), lat_o = 360.0 / 59 * (pymod_i(j, 59) + lo);
    if (lat_e >= 270) lat_e -= 360;
    if (lat_o >= 270) lat_o -= 360;
    if (std::fabs(lat_e) > 90 || std::fabs(lat_o) > 90) return std::nullopt;
    if (nl(lat_e) != nl(lat_o)) return std::nullopt;          // pair straddles a zone boundary
    double lat = newest ? lat_o : lat_e;
    int n = nl(lat), ni = std::max(n - newest, 1);
    long long m = (long long)std::floor(ne * (n - 1) - no * n + 0.5);
    double lon = (360.0 / ni) * (pymod_i(m, ni) + (newest ? no : ne));
    if (lon >= 180) lon -= 360;
    return Pos{lat, lon};
}
static Pos local_decode(double rlat, double rlon, double lat_cpr, double lon_cpr, int odd) {
    double yz = lat_cpr / 131072.0, xz = lon_cpr / 131072.0, dlat = 360.0 / (odd ? 59 : 60);
    double j = std::floor(rlat / dlat) + std::floor(0.5 + pymod(rlat, dlat) / dlat - yz);
    double lat = dlat * (j + yz);
    int ni = std::max(nl(lat) - (odd ? 1 : 0), 1);
    double dlon = 360.0 / ni;
    double m = std::floor(rlon / dlon) + std::floor(0.5 + pymod(rlon, dlon) / dlon - xz);
    double lon = dlon * (m + xz);
    if (lon >= 180) lon -= 360;
    return {lat, lon};
}

struct State { Fr frames[2], cand[2], ref; int rej = 0; bool has_ref = false; };

class Tracker {
public:
    static long long bits(const std::vector<uint8_t>& b, size_t a, size_t n) { long long v = 0; for (size_t i = a; i < a + n && i < b.size(); ++i) v = (v << 1) | b[i]; return v; }

    std::optional<Pos> feed(const std::string& icao, const std::vector<uint8_t>& b, double t) {
        long long tc = bits(b, 32, 5);
        if (!((9 <= tc && tc <= 18) || (20 <= tc && tc <= 22))) return std::nullopt;     // not an airborne position message
        int odd = (int)bits(b, 53, 1);
        double lat_cpr = (double)bits(b, 54, 17), lon_cpr = (double)bits(b, 71, 17);
        State& s = ac_[icao];
        s.frames[odd] = {lat_cpr, lon_cpr, t, true};
        if (s.has_ref && t - s.ref.t <= REF_MAX_AGE_S) {
            Pos p = local_decode(s.ref.lat, s.ref.lon, lat_cpr, lon_cpr, odd);
            if (dist_km(s.ref.lat, s.ref.lon, p.lat, p.lon) <= reachable_km(t - s.ref.t)) {
                s.ref = {p.lat, p.lon, t, true};
                s.rej = 0;
                return p;
            }
            if (++s.rej >= REJECT_LIMIT) { s.has_ref = false; s.cand[0] = s.cand[1] = Fr(); s.rej = 0; }   // track lost: confirm again
            return std::nullopt;
        }
        s.has_ref = false;
        const Fr &ev = s.frames[0], &od = s.frames[1];
        if (!ev.set || !od.set || std::fabs(ev.t - od.t) > PAIR_WINDOW_S) return std::nullopt;
        auto pos = global_decode(ev, od, odd);
        if (!pos) return std::nullopt;
        const Fr& c = s.cand[odd];        // compare like with like: decodes anchored on the same parity
        if (c.set && t - c.t <= CONFIRM_WINDOW_S && t > c.t && dist_km(c.lat, c.lon, pos->lat, pos->lon) <= reachable_km(t - c.t)) {
            s.ref = {pos->lat, pos->lon, t, true};
            s.has_ref = true;
            s.cand[0] = s.cand[1] = Fr();
            return pos;
        }
        s.cand[odd] = {pos->lat, pos->lon, t, true};
        return std::nullopt;
    }
    void forget(const std::string& icao) { ac_.erase(icao); }
private:
    std::unordered_map<std::string, State> ac_;
};
}  // namespace cpr
static cpr::Tracker CPR;

// ------------------------------------------------------------------ decoder messages (PMT)
static void publish_ais(const json& t) { PUB->publish("air/ais/target", {{"target", t}}); }
static void publish_adsb(const json& t) { PUB->publish("air/adsb/target", {{"target", t}}); }

static void on_ais(const std::string& raw) {
    std::string sentence;
    try {
        pmt::pmt_t p = pmt::deserialize_str(raw);
        if (pmt::is_pair(p)) {                               // PDU (metadata . u8vector of the ASCII "!AIVDM,..." sentence)
            pmt::pmt_t body = pmt::cdr(p);
            if (pmt::is_u8vector(body)) { auto v = pmt::u8vector_elements(body); sentence.assign(v.begin(), v.end()); }
            else if (pmt::is_symbol(pmt::car(p))) sentence = pmt::symbol_to_string(pmt::car(p));
        } else if (pmt::is_symbol(p)) {
            sentence = pmt::symbol_to_string(p);
        }
    } catch (...) { return; }
    if (sentence.empty()) return;
    // payload = field 5 of "!AIVDM,1,1,,A,<payload>,0*5C"
    std::vector<std::string> parts;
    size_t a = 0;
    for (;;) { size_t c = sentence.find(',', a); parts.push_back(sentence.substr(a, c == std::string::npos ? c : c - a)); if (c == std::string::npos) break; a = c + 1; }
    std::string payload;
    if (parts.size() >= 6 && (parts[0] == "!AIVDM" || parts[0] == "!AIVDO")) payload = parts[5];
    json target;
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        double now = now_s();
        if (!payload.empty()) {                              // the decoder's carrier-offset branches deliver a frame 2-4 times
            auto it = AIS_SEEN.find(payload);
            if (it != AIS_SEEN.end() && now - it->second < 2.0) return;
            AIS_SEEN[payload] = now;
            if (AIS_SEEN.size() > 2000)
                for (auto i = AIS_SEEN.begin(); i != AIS_SEEN.end();) i = (now - i->second > 10.0) ? AIS_SEEN.erase(i) : std::next(i);
        }
        auto parsed = payload.empty() ? std::nullopt : parse_aivdm(payload);
        std::printf("[AIS] %s\n", sentence.c_str());
        std::fflush(stdout);
        if (parsed) {
            auto t = update_ais_target(*parsed);
            if (t) { target = *t; have = true; }
        }
    }
    if (have) publish_ais(target);
}

static void on_adsb_decoded(const std::string& raw) {       // gr-adsb dict: icao, callsign, altitude, speed, heading, vertical_rate
    json fields;
    try {
        pmt::pmt_t p = pmt::deserialize_str(raw);
        if (!pmt::is_pair(p)) return;
        fields = pmt_to_json(pmt::car(p));
    } catch (...) { return; }
    if (!fields.is_object() || !fields.contains("icao") || fields["icao"].is_null()) return;
    std::string icao;
    if (fields["icao"].is_string()) icao = fields["icao"].get<std::string>();
    else { char b[16]; std::snprintf(b, sizeof b, "%06llX", (unsigned long long)fields["icao"].get<long long>()); icao = b; }
    if (icao.empty()) return;
    std::transform(icao.begin(), icao.end(), icao.begin(), [](unsigned char c) { return std::toupper(c); });
    json target;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!ADSB_CONFIRMED.count(icao)) return;
        json& a = ADSB_AIRCRAFT.try_emplace(icao, json{{"icao", icao}}).first->second;
        // gr-adsb's own lat/lon are NOT used (see adsb_cpr.py): positions come only from the validated raw-frame decode
        const char* map[][2] = {{"callsign", "callsign"}, {"altitude", "alt"}, {"speed", "speed"}, {"heading", "heading"}, {"vertical_rate", "vertical_rate"}};
        for (auto& m : map) {
            if (!fields.contains(m[0]) || fields[m[0]].is_null()) continue;
            json val = fields[m[0]];
            if (val.is_number_float() && std::isnan(val.get<double>())) continue;
            if (val.is_string()) {
                std::string s = val.get<std::string>();
                size_t x = s.find_first_not_of(" \t\r\n"), y = s.find_last_not_of(" \t\r\n");
                if (x == std::string::npos) continue;
                val = s.substr(x, y - x + 1);
            }
            if (std::string(m[1]) == "heading" && val.is_number()) { double h = val.get<double>(); val = h - 360.0 * std::floor(h / 360.0); }   // gr-adsb reports e.g. -169
            a[m[1]] = val;
        }
        a["last_seen"] = now_s();
        target = a;
    }
    publish_adsb(target);
}

static void on_adsb_raw(const std::string& raw) {            // every CRC-valid Mode-S frame -> validated positions
    std::string icao;
    double t_msg;
    std::vector<uint8_t> bits;
    try {
        pmt::pmt_t p = pmt::deserialize_str(raw);
        pmt::pmt_t meta = pmt::car(p);
        t_msg = pmt::to_double(pmt::dict_ref(meta, pmt::mp("timestamp"), pmt::PMT_NIL));
        bits = pmt::u8vector_elements(pmt::cdr(p));
    } catch (...) { return; }
    if (bits.size() < 32) return;
    char b[16];
    std::snprintf(b, sizeof b, "%06llX", (unsigned long long)cpr::Tracker::bits(bits, 8, 24));
    icao = b;
    json target;
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto pos = CPR.feed(icao, bits, t_msg);
        if (!ADSB_CONFIRMED.count(icao)) {                   // 2 CRC-valid frames within ADSB_CONFIRM_S before an address is shown
            auto it = ADSB_FIRST_FRAME.find(icao);
            if (it == ADSB_FIRST_FRAME.end() || t_msg - it->second > ADSB_CONFIRM_S) { ADSB_FIRST_FRAME[icao] = t_msg; return; }
            ADSB_FIRST_FRAME.erase(it);
            ADSB_CONFIRMED.insert(icao);
        }
        json& a = ADSB_AIRCRAFT.try_emplace(icao, json{{"icao", icao}}).first->second;
        a["msgs"] = a.value("msgs", 0LL) + 1;
        a["last_seen"] = now_s();
        if (pos) { a["lat"] = pos->lat; a["lon"] = pos->lon; target = a; have = true; }
    }
    if (have) publish_adsb(target);
}

static void decoder_msgs_thread(void* ctx) {
    std::string addrs[3];
    for (int i = 0; i < 3; ++i) addrs[i] = "tcp://127.0.0.1:" + std::to_string(DEC_PORTS[i]);
    void* s[3];
    zmq_pollitem_t it[3];
    for (int i = 0; i < 3; ++i) {
        s[i] = zmq_socket(ctx, ZMQ_SUB);
        int linger = 0;
        zmq_setsockopt(s[i], ZMQ_LINGER, &linger, sizeof linger);
        zmq_setsockopt(s[i], ZMQ_SUBSCRIBE, "", 0);
        zmq_connect(s[i], addrs[i].c_str());
        it[i] = {s[i], 0, ZMQ_POLLIN, 0};
    }
    std::string buf;
    while (!g_stop) {
        if (zmq_poll(it, 3, 200) <= 0) continue;
        for (int i = 0; i < 3; ++i) {
            while (it[i].revents & ZMQ_POLLIN) {
                zmq_msg_t m;
                zmq_msg_init(&m);
                if (zmq_msg_recv(&m, s[i], ZMQ_DONTWAIT) < 0) { zmq_msg_close(&m); break; }
                buf.assign((const char*)zmq_msg_data(&m), zmq_msg_size(&m));
                zmq_msg_close(&m);
                if (i == 0) on_ais(buf); else if (i == 1) on_adsb_decoded(buf); else on_adsb_raw(buf);
            }
        }
    }
    for (auto x : s) zmq_close(x);
}

static void prune_thread() {
    while (!g_stop) {
        for (int i = 0; i < 50 && !g_stop; ++i) usleep(100000);
        double now = now_s();
        std::vector<std::string> gone_adsb, gone_ais;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (ADSB_FIRST_FRAME.size() > 5000) ADSB_FIRST_FRAME.clear();
            for (auto i = ADSB_AIRCRAFT.begin(); i != ADSB_AIRCRAFT.end();)
                if (now - i->second.value("last_seen", now) > ADSB_STALE_S) {
                    gone_adsb.push_back(i->first);
                    CPR.forget(i->first);
                    ADSB_CONFIRMED.erase(i->first);
                    i = ADSB_AIRCRAFT.erase(i);
                } else ++i;
            for (auto i = AIS_VESSELS.begin(); i != AIS_VESSELS.end();)
                if (now - i->second.value("last_seen", now) > AIS_STALE_S) {
                    gone_ais.push_back(i->first);
                    AIS_CONFIRMED.erase(i->first);
                    i = AIS_VESSELS.erase(i);
                } else ++i;
        }
        for (auto& k : gone_adsb) PUB->publish("air/adsb/remove", {{"icao", k}});
        for (auto& k : gone_ais) PUB->publish("air/ais/remove", {{"mmsi", k}});
    }
}

// ------------------------------------------------------------------ IQ spectrum (Welch mean + peak over every sample of a 0.1 s window)
class Fft {
public:
    explicit Fft(int n) : n_(n), tw_(n / 2) {
        for (int k = 0; k < n / 2; ++k) tw_[k] = std::polar(1.0f, (float)(-2 * M_PI * k / n));
    }
    void run(std::vector<cf>& a) const {
        for (int i = 1, j = 0; i < n_; ++i) {
            int bit = n_ >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (int len = 2; len <= n_; len <<= 1) {
            int step = n_ / len;
            for (int i = 0; i < n_; i += len)
                for (int k = 0; k < len / 2; ++k) {
                    cf u = a[i + k], v = a[i + k + len / 2] * tw_[k * step];
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                }
        }
    }
private:
    int n_;
    std::vector<cf> tw_;
};

static void spectrum_thread(void* ctx, size_t bi) {
    const Band& band = BANDS[bi];
    Fft fft(IQ_FFT_N);
    std::vector<float> hann(IQ_FFT_N);
    for (int i = 0; i < IQ_FFT_N; ++i) hann[i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / (IQ_FFT_N - 1)));
    void* sub = zmq_socket(ctx, ZMQ_SUB);
    int hwm = 100, linger = 0;                                // ADS-B messages are ~1 MB: keep the display queue short
    zmq_setsockopt(sub, ZMQ_RCVHWM, &hwm, sizeof hwm);
    zmq_setsockopt(sub, ZMQ_LINGER, &linger, sizeof linger);
    std::string topic = "iq/" + band.key + "/";
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, topic.data(), topic.size());
    zmq_connect(sub, IQ_ADDR.c_str());
    size_t win = std::max<size_t>(IQ_FFT_N, (size_t)(band.samp_rate_hz * SPEC_WINDOW_S) / IQ_FFT_N * IQ_FFT_N);
    std::vector<cf> buf;
    double last_idle = 0;
    zmq_pollitem_t it{sub, 0, ZMQ_POLLIN, 0};
    std::vector<cf> row(IQ_FFT_N);
    std::vector<double> pmean(IQ_FFT_N);
    std::vector<float> pmax(IQ_FFT_N);
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
        if (fr.size() != 3 || fr[2].size() % 8 || fr[2].empty()) continue;      // malformed: never feed it to the FFT
        LAST_IQ_RX[bi] = now_s();
        size_t old = buf.size(), n = fr[2].size() / 8;
        buf.resize(old + n);
        std::memcpy(buf.data() + old, fr[2].data(), fr[2].size());
        while (buf.size() >= win) {
            // display only: keep the newest window, drop older backlog in whole FFT chunks
            if (buf.size() > 2 * win) buf.erase(buf.begin(), buf.begin() + ((buf.size() / IQ_FFT_N) * IQ_FFT_N - win));
            size_t rows = win / IQ_FFT_N, good = 0;
            double psum = 0;
            std::fill(pmean.begin(), pmean.end(), 0.0);
            std::fill(pmax.begin(), pmax.end(), 0.0f);
            for (size_t r = 0; r < rows; ++r) {
                const cf* x = buf.data() + r * IQ_FFT_N;
                bool finite = true;
                float mx = 0;
                for (int i = 0; i < IQ_FFT_N; ++i) {      // rows of zero-fill or corrupted data are not signal
                    if (!std::isfinite(x[i].real()) || !std::isfinite(x[i].imag())) { finite = false; break; }
                    mx = std::max(mx, std::abs(x[i]));
                }
                if (!finite || !(mx > 0)) continue;
                ++good;
                for (int i = 0; i < IQ_FFT_N; ++i) { psum += std::norm(x[i]); row[i] = x[i] * hann[i]; }
                fft.run(row);
                for (int i = 0; i < IQ_FFT_N; ++i) {
                    float p = std::norm(row[i]);
                    int k = (i + IQ_FFT_N / 2) % IQ_FFT_N;         // fftshift
                    pmean[k] += p;
                    pmax[k] = std::max(pmax[k], p);
                }
            }
            buf.erase(buf.begin(), buf.begin() + win);
            double now = now_s();
            if (good == 0) {
                if (now - last_idle > 0.5) {
                    last_idle = now;
                    PUB->publish("air/spectrum/" + band.key, {{"band", band.key}, {"active", false}, {"rms_db", -120.0}, {"ffts", 0}, {"n_spec", 0}, {"n_peak", 0}});
                }
                continue;
            }
            double pwr = psum / (double)(good * IQ_FFT_N);
            double rms_db = pwr > 1e-24 ? 10 * std::log10(pwr) : -120.0;
            std::vector<float> out(2 * IQ_FFT_N);
            for (int i = 0; i < IQ_FFT_N; ++i) {
                out[i] = (float)round_to(10 * std::log10(std::max(pmean[i] / good, 1e-18)), 1);
                out[IQ_FFT_N + i] = (float)round_to(10 * std::log10(std::max((double)pmax[i], 1e-18)), 1);
            }
            PUB->publish("air/spectrum/" + band.key, {{"band", band.key}, {"active", rms_db > IQ_ACTIVE_THRESH_DBFS}, {"rms_db", round_to(rms_db, 1)},
                         {"ffts", (long long)good}, {"n_spec", IQ_FFT_N}, {"n_peak", IQ_FFT_N}}, out.data(), out.size() * 4);
        }
    }
    zmq_close(sub);
}

// ------------------------------------------------------------------ decoder process
static std::mutex g_proc_mu;
static pid_t g_pid = 0;

static bool decoder_running() {
    std::lock_guard<std::mutex> lk(g_proc_mu);
    if (g_pid > 0) {
        int st;
        if (waitpid(g_pid, &st, WNOHANG) != 0) g_pid = 0;            // exited (crash, killed from outside) or already gone
    }
    return g_pid > 0;
}

static std::pair<bool, std::string> start_decoder() {
    if (decoder_running()) return {true, "already running"};
    // SECURECOM_AIR_DECODER_BIN = the C++ decoder (securecom_air_decoder); without it the Python flowgraph is started
    std::string py = DECODER_BIN.empty() ? (fs::path(DECODER_DIR) / "airmaritime_decoder_live.py").string() : DECODER_BIN;
    if (!fs::exists(py)) return {false, "Script not found: " + py};
    pid_t pid = fork();
    if (pid == 0) {                                                  // headless: no display on the edge board
        setsid();
        if (env("SECURECOM_DECODER_GUI", "") != "1") setenv("QT_QPA_PLATFORM", "offscreen", 1);
        if (chdir(DECODER_DIR.c_str()) != 0) _exit(127);
        if (!DECODER_BIN.empty()) execl(py.c_str(), py.c_str(), (char*)nullptr);
        else execlp("python3", "python3", py.c_str(), (char*)nullptr);
        _exit(127);
    }
    {
        std::lock_guard<std::mutex> lk(g_proc_mu);
        g_pid = pid;
    }
    std::printf("[BRIDGE] Started flowgraph script %s (pid %d)\n", py.c_str(), (int)pid);
    std::fflush(stdout);
    PUB->publish("air/status", json{{"decoder_running", true}});
    return {true, ""};
}

static void stop_decoder() {
    pid_t pid;
    {
        std::lock_guard<std::mutex> lk(g_proc_mu);
        pid = g_pid;
        g_pid = 0;
    }
    if (pid > 0) {
        kill(pid, SIGTERM);
        int st = 0;
        bool done = false;
        for (int i = 0; i < 50 && !done; ++i) { done = waitpid(pid, &st, WNOHANG) == pid; if (!done) usleep(100000); }
        if (!done) { kill(pid, SIGKILL); waitpid(pid, &st, 0); }
        std::printf("[BRIDGE] Stopped flowgraph script\n");
        std::fflush(stdout);
    }
}

// ------------------------------------------------------------------ status, heartbeat, commands
static json status_snapshot() {
    json ic = json::object();
    double now = now_s();
    for (size_t i = 0; i < BANDS.size(); ++i) ic[BANDS[i].key] = (now - LAST_IQ_RX[i].load()) < IQ_TIMEOUT_S;
    std::lock_guard<std::mutex> lk(g_mu);
    return {{"decoder_running", decoder_running()}, {"iq_transport", "zmq"}, {"iq_connected", ic},
            {"ais_targets", (long long)AIS_VESSELS.size()}, {"adsb_targets", (long long)ADSB_AIRCRAFT.size()}};
}
static json bands_info() {
    json a = json::array();
    for (auto& b : BANDS) a.push_back({{"key", b.key}, {"name", b.name}, {"freq_mhz", b.freq_mhz}, {"samp_rate_hz", b.samp_rate_hz},
                                       {"n_spec", IQ_FFT_N}, {"spectrum_topic", "air/spectrum/" + b.key}});
    return a;
}
static json targets(const std::map<std::string, json>& m) { json a = json::array(); for (auto& [k, v] : m) a.push_back(v); return a; }

static void heartbeat_thread() {
    int n = 0;
    while (!g_stop) {
        PUB->publish("air/status", status_snapshot());
        if (n % 5 == 0) {
            json ais, adsb;
            { std::lock_guard<std::mutex> lk(g_mu); ais = targets(AIS_VESSELS); adsb = targets(ADSB_AIRCRAFT); }
            PUB->publish("air/bands", {{"bands", bands_info()}});
            PUB->publish("air/ais/targets", {{"targets", ais}});
            PUB->publish("air/adsb/targets", {{"targets", adsb}});
        }
        ++n;
        for (int i = 0; i < 20 && !g_stop; ++i) usleep(100000);
    }
}

static json handle(const json& req) {
    std::string cmd = req.value("cmd", std::string());
    if (cmd == "get_state") {
        json ais, adsb;
        { std::lock_guard<std::mutex> lk(g_mu); ais = targets(AIS_VESSELS); adsb = targets(ADSB_AIRCRAFT); }
        return {{"interface_version", "1.0"}, {"status", status_snapshot()}, {"bands", bands_info()}, {"ais_vessels", ais}, {"adsb_aircraft", adsb}};
    }
    if (cmd == "start_decoder") { auto r = start_decoder(); return {{"ok", r.first}, {"decoder_running", decoder_running()}, {"note", r.second}}; }
    if (cmd == "stop_decoder") { stop_decoder(); PUB->publish("air/status", status_snapshot()); return {{"decoder_running", decoder_running()}}; }
    return {{"ok", false}, {"error", "unknown cmd '" + cmd + "'"}, {"known", json::array({"get_state", "start_decoder", "stop_decoder"})}};
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
        for (;;) { int more = 0; size_t ms = sizeof more; zmq_getsockopt(rep, ZMQ_RCVMORE, &more, &ms); if (!more) break; zmq_recv(rep, buf.data(), buf.size(), 0); }
        json r;
        try { r = handle(json::parse(std::string(buf.data(), (size_t)std::max(n, 0)))); }
        catch (const std::exception& e) { r = {{"ok", false}, {"error", std::string("Error: ") + e.what()}}; }   // a bad request must never take the backend down
        r.emplace("ok", true);
        std::string h = r.dump();
        zmq_send(rep, h.data(), h.size(), ZMQ_SNDMORE);
        zmq_send(rep, "", 0, 0);
    }
    zmq_close(rep);
}

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    PUB_ADDR = env("SECURECOM_AIR_PUB_ADDR", "tcp://127.0.0.1:5565");
    CMD_ADDR = env("SECURECOM_AIR_CMD_ADDR", "tcp://127.0.0.1:5566");
    IQ_ADDR = env("SECURECOM_IQ_PUB_ADDR", "tcp://127.0.0.1:5571");
    DECODER_DIR = env("SECURECOM_DECODER_DIR", "airmaritime");
    DECODER_BIN = env("SECURECOM_AIR_DECODER_BIN", "");
    DEC_PORTS[0] = std::atoi(env("SECURECOM_AIR_DEC_AIS_PORT", "9201").c_str());
    DEC_PORTS[1] = std::atoi(env("SECURECOM_AIR_DEC_ADSB_PORT", "9202").c_str());
    DEC_PORTS[2] = std::atoi(env("SECURECOM_AIR_DEC_RAW_PORT", "9203").c_str());
    for (auto& a : LAST_IQ_RX) a = 0.0;
    void* ctx = zmq_ctx_new();
    PUB = new Pub(ctx, PUB_ADDR);
    std::printf("ZeroMQ: PUB %s (topics air/...), REP %s (commands); IQ from %s\n", PUB_ADDR.c_str(), CMD_ADDR.c_str(), IQ_ADDR.c_str());
    std::fflush(stdout);
    std::vector<std::thread> th;
    th.emplace_back(decoder_msgs_thread, ctx);
    th.emplace_back(prune_thread);
    th.emplace_back(spectrum_thread, ctx, 0);
    th.emplace_back(spectrum_thread, ctx, 1);
    th.emplace_back(heartbeat_thread);
    th.emplace_back(command_thread, ctx);
    while (!g_stop) usleep(200000);
    for (auto& t : th) t.join();
    stop_decoder();
    PUB->close();
    zmq_ctx_term(ctx);
    return 0;
}
