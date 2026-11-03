// IQ ingest for the edge board (C++ port of ingest/ingest.py): ONE ZeroMQ subscriber for every module.
//
//   RTOS (PUB, tcp://...:5580) --> [this process] --> local PUB (tcp://127.0.0.1:5570), topic "iq/<band>/"
//
// Input: 3 frames [topic, JSON header, complex64 payload]; header fields used: centre_hz, rate_sps, optionally seq
// (one counter of the sender over all messages) and t. Messages arrive one by one, in any order, from any band;
// nothing here assumes a schedule. A message is routed by its topic (routes.json "by_topic") or, for a generic
// topic, by the header's centre_hz (nearest route within its tol_hz). Anything else is counted as `unknown` and
// dropped, never guessed. The payload bytes are forwarded untouched (zero copy); the header is rewritten:
// per-band seq, t, band, module, centre_hz, rate_sps, n, src_seq. Status: topic "ingest/status" every 2 s.
// routes.json is re-read when it changes. Config: env SECURECOM_RTOS_ADDR, SECURECOM_IQ_PUB_ADDR,
// SECURECOM_INGEST_ROUTES (or argv[1]).
#include <zmq.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <vector>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

static volatile std::sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

static double now_s() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
static std::string env(const char* k, const char* d) { const char* v = std::getenv(k); return v ? v : d; }

struct Route {
    std::string band, module;
    double centre_hz = 0, tol_hz = 0;
    std::optional<double> rate_sps;
};

class Table {
public:
    explicit Table(std::string path) : path_(std::move(path)) { reload(true); }

    void reload(bool force = false) {
        struct stat st {};
        if (::stat(path_.c_str(), &st) != 0) {
            std::printf("[ingest] routes.json not (re)loaded, keeping the previous table: cannot stat %s\n", path_.c_str());
            std::fflush(stdout);
            return;
        }
        long long m = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
        if (!force && m == mtime_) return;
        try {
            std::ifstream f(path_);
            json d = json::parse(f);
            std::vector<Route> routes;
            for (auto& r : d.value("routes", json::array())) {
                if (!r.contains("band") || !r.contains("centre_hz")) continue;
                Route x;
                x.band = r["band"].get<std::string>();
                x.module = r.value("module", std::string("?"));
                x.centre_hz = r["centre_hz"].get<double>();
                x.tol_hz = r.value("tol_hz", 0.0);
                if (r.contains("rate_sps") && !r["rate_sps"].is_null() && r["rate_sps"].get<double>() != 0)
                    x.rate_sps = r["rate_sps"].get<double>();
                routes.push_back(std::move(x));
            }
            std::vector<Route> ign;
            for (auto& r : d.value("ignore", json::array()))
                if (r.contains("centre_hz")) { Route x; x.centre_hz = r["centre_hz"].get<double>(); x.tol_hz = r.value("tol_hz", 0.0); ign.push_back(x); }
            ignore_ = std::move(ign);
            std::map<std::string, std::string> bt;
            for (auto& [k, v] : d.value("by_topic", json::object()).items()) bt[k] = v.get<std::string>();
            routes_ = std::move(routes);
            by_topic_ = std::move(bt);
            mtime_ = m;
            std::printf("[ingest] routes loaded: %zu by frequency, %zu by topic\n", routes_.size(), by_topic_.size());
        } catch (const std::exception& e) {
            std::printf("[ingest] routes.json not (re)loaded, keeping the previous table: %s\n", e.what());
        }
        std::fflush(stdout);
    }

    // frequencies that belong to ANOTHER module: dropped silently and counted as `ignored` (not as `unknown`)
    bool is_ignored(const json& hdr) const {
        if (!hdr.contains("centre_hz") || !hdr["centre_hz"].is_number()) return false;
        double fc = hdr["centre_hz"].get<double>();
        for (auto& r : ignore_) if (std::fabs(fc - r.centre_hz) <= r.tol_hz) return true;
        return false;
    }

    // topic match first, else nearest centre frequency within tolerance
    std::optional<Route> pick(const std::string& topic, const json& hdr) const {
        auto it = by_topic_.find(topic);
        if (it != by_topic_.end()) {
            for (auto& r : routes_) if (r.band == it->second) return r;
            Route x;
            x.band = it->second;
            x.module = hdr.contains("module") && hdr["module"].is_string() ? hdr["module"].get<std::string>() : "?";
            return x;
        }
        if (!hdr.contains("centre_hz") || !hdr["centre_hz"].is_number()) return std::nullopt;
        double fc = hdr["centre_hz"].get<double>();
        const Route* best = nullptr;
        double dist = 0;
        for (auto& r : routes_) {
            double d = std::fabs(fc - r.centre_hz);
            if (d <= r.tol_hz && (!best || d < dist)) { best = &r; dist = d; }
        }
        return best ? std::optional<Route>(*best) : std::nullopt;
    }

private:
    std::string path_;
    long long mtime_ = -1;
    std::vector<Route> routes_, ignore_;
    std::map<std::string, std::string> by_topic_;
};

struct Stats {
    long long in = 0, unknown = 0, ignored = 0, bad = 0, rate_mismatch = 0, src_lost = 0, out_dropped = 0;
    std::map<std::string, long long> routed, seq;                 // seq = per-band counter of the messages published
    mutable long long status_seq = 0;                             // the standard header `seq` of the ingest/status topic itself
    json to_json(double t) const {
        return json{{"t", t}, {"in", in}, {"routed", routed}, {"unknown", unknown}, {"ignored", ignored}, {"bad", bad},
                    {"rate_mismatch", rate_mismatch}, {"src_lost", src_lost}, {"out_dropped", out_dropped}, {"band_seq", seq}, {"seq", ++status_seq}};
    }
};

// one multipart message: topic + header as strings, payload kept as a zmq_msg_t so it is forwarded without copying
struct Incoming {
    std::string topic, header;
    zmq_msg_t payload;
    int nframes = 0;
    bool payload_valid = false;
    ~Incoming() { if (payload_valid) zmq_msg_close(&payload); }
};

static bool recv_message(void* sock, Incoming& m) {
    for (;;) {
        zmq_msg_t f;
        zmq_msg_init(&f);
        if (zmq_msg_recv(&f, sock, 0) < 0) { zmq_msg_close(&f); return false; }
        int more = zmq_msg_more(&f);
        ++m.nframes;
        if (m.nframes == 1) m.topic.assign((const char*)zmq_msg_data(&f), zmq_msg_size(&f));
        else if (m.nframes == 2) m.header.assign((const char*)zmq_msg_data(&f), zmq_msg_size(&f));
        else if (m.nframes == 3) { zmq_msg_init(&m.payload); zmq_msg_move(&m.payload, &f); m.payload_valid = true; }
        zmq_msg_close(&f);
        if (!more) return true;
    }
}

static bool send_frame(void* sock, const void* d, size_t n, int flags) {
    return zmq_send(sock, d, n, flags) >= 0;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::string in_addr = env("SECURECOM_RTOS_ADDR", "tcp://127.0.0.1:5580");
    std::string out_addr = env("SECURECOM_IQ_PUB_ADDR", "tcp://127.0.0.1:5570");
    std::string routes = argc > 1 ? argv[1] : env("SECURECOM_INGEST_ROUTES", "ingest/routes.json");

    void* ctx = zmq_ctx_new();
    void* sub = zmq_socket(ctx, ZMQ_SUB);
    int hwm = 20000, linger = 0;
    zmq_setsockopt(sub, ZMQ_RCVHWM, &hwm, sizeof hwm);
    zmq_setsockopt(sub, ZMQ_LINGER, &linger, sizeof linger);
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, "", 0);
    if (zmq_connect(sub, in_addr.c_str()) != 0) { std::fprintf(stderr, "connect %s: %s\n", in_addr.c_str(), zmq_strerror(zmq_errno())); return 1; }
    void* pub = zmq_socket(ctx, ZMQ_PUB);
    zmq_setsockopt(pub, ZMQ_SNDHWM, &hwm, sizeof hwm);
    zmq_setsockopt(pub, ZMQ_LINGER, &linger, sizeof linger);
    if (zmq_bind(pub, out_addr.c_str()) != 0) { std::fprintf(stderr, "bind %s: %s\n", out_addr.c_str(), zmq_strerror(zmq_errno())); return 1; }

    Table table(routes);
    Stats st;
    std::optional<long long> last_src;
    auto t_stat = Clock::now(), t_print = t_stat, t_reload = t_stat;
    std::printf("[ingest] SUB %s -> PUB %s (topics iq/<band>/ -- trailing slash, so iq/nxdn does not also match iq/nxdn48)\n",
                in_addr.c_str(), out_addr.c_str());
    std::fflush(stdout);

    zmq_pollitem_t item{sub, 0, ZMQ_POLLIN, 0};
    while (!g_stop) {
        auto now = Clock::now();
        auto since = [&](Clock::time_point t) { return std::chrono::duration<double>(now - t).count(); };
        if (since(t_reload) > 2.0) { table.reload(); t_reload = now; }
        if (since(t_stat) > 2.0) {
            std::string h = st.to_json(now_s()).dump();
            send_frame(pub, "ingest/status", 13, ZMQ_SNDMORE);
            send_frame(pub, h.data(), h.size(), ZMQ_SNDMORE);
            send_frame(pub, "", 0, 0);
            t_stat = now;
        }
        if (since(t_print) > 10.0) {
            std::printf("[ingest] in=%lld routed=%s unknown=%lld ignored=%lld bad=%lld rate_mismatch=%lld src_lost=%lld out_dropped=%lld\n",
                        st.in, json(st.routed).dump().c_str(), st.unknown, st.ignored, st.bad, st.rate_mismatch, st.src_lost, st.out_dropped);
            std::fflush(stdout);
            t_print = now;
        }
        if (zmq_poll(&item, 1, 200) <= 0) continue;

        Incoming m;
        if (!recv_message(sub, m)) continue;
        ++st.in;
        if (m.nframes != 3 || !m.payload_valid || zmq_msg_size(&m.payload) == 0 || zmq_msg_size(&m.payload) % 8) { ++st.bad; continue; }
        json hdr = json::parse(m.header, nullptr, false);
        if (hdr.is_discarded() || !hdr.is_object()) { ++st.bad; continue; }

        auto r = table.pick(m.topic, hdr);
        if (!r) { if (table.is_ignored(hdr)) ++st.ignored; else ++st.unknown; continue; }
        const std::string& band = r->band;

        // ONE sender counter over all messages: a gap = lost upstream; a smaller value = the sender restarted
        if (hdr.contains("seq") && hdr["seq"].is_number_integer()) {
            long long s = hdr["seq"].get<long long>();
            if (last_src && s > *last_src + 1) st.src_lost += s - *last_src - 1;
            last_src = s;
        }
        if (r->rate_sps && hdr.contains("rate_sps") && !hdr["rate_sps"].is_null() && hdr["rate_sps"].is_number() &&
            hdr["rate_sps"].get<double>() != *r->rate_sps)
            ++st.rate_mismatch;

        long long seq = ++st.seq[band];
        json out = {{"seq", seq},
                    {"t", hdr.contains("t") ? hdr["t"] : json(now_s())},
                    {"band", band},
                    {"module", r->module},
                    {"centre_hz", hdr.contains("centre_hz") ? hdr["centre_hz"] : json(nullptr)},
                    {"rate_sps", hdr.contains("rate_sps") ? hdr["rate_sps"] : json(nullptr)},
                    {"n", (long long)(zmq_msg_size(&m.payload) / 8)},
                    {"src_seq", hdr.contains("seq") ? hdr["seq"] : json(nullptr)}};
        std::string topic = "iq/" + band + "/", h = out.dump();
        if (!send_frame(pub, topic.data(), topic.size(), ZMQ_SNDMORE | ZMQ_DONTWAIT)) { ++st.out_dropped; continue; }
        send_frame(pub, h.data(), h.size(), ZMQ_SNDMORE);
        zmq_msg_send(&m.payload, pub, 0);              // moves the payload buffer: no copy of the IQ samples
        ++st.routed[band];
    }
    zmq_close(sub);
    zmq_close(pub);
    zmq_ctx_term(ctx);
    return 0;
}
