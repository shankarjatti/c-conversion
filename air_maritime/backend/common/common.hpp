// Helpers shared by the C++ backend programs: time/env, thread-safe ZeroMQ publisher (same 3-frame envelope as
// gui/zmq_iface.py), PMT -> JSON.
#pragma once
#include <zmq.h>

#include <nlohmann/json.hpp>
#include <pmt/pmt.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

namespace sc {
using json = nlohmann::json;

inline double now_s() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
inline std::string env(const char* k, const char* d) { const char* v = std::getenv(k); return v ? v : d; }
inline double round_to(double x, int dec) { double m = std::pow(10.0, dec); return std::round(x * m) / m; }

// Envelope: frame 0 topic, frame 1 JSON header {seq (per topic, from 1), t, ...}, frame 2 payload (raw bytes or empty).
class Pub {
public:
    Pub(void* ctx, const std::string& addr) {
        sock_ = zmq_socket(ctx, ZMQ_PUB);
        int hwm = 20000, linger = 0;
        zmq_setsockopt(sock_, ZMQ_SNDHWM, &hwm, sizeof hwm);
        zmq_setsockopt(sock_, ZMQ_LINGER, &linger, sizeof linger);
        if (zmq_bind(sock_, addr.c_str()) != 0) { std::fprintf(stderr, "bind %s: %s\n", addr.c_str(), zmq_strerror(zmq_errno())); std::exit(1); }
    }
    // Never blocks: a subscriber that cannot keep up loses messages (visible as a seq gap).
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
    void close() { zmq_close(sock_); }
    long long dropped = 0;
private:
    void* sock_;
    std::mutex mu_;
    std::map<std::string, long long> seq_;
};

inline json pmt_to_json(const pmt::pmt_t& p) {
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
}  // namespace sc
