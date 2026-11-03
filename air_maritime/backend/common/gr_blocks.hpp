// GNU Radio blocks shared by the C++ decoders.
#pragma once
#include <gnuradio/io_signature.h>
#include <gnuradio/sync_block.h>
#include <zmq.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace sc {
inline double now_s_() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// ---------------------------------------------------------------- ZeroMQ IQ source (port of iq_zmq_source.py)
class iq_zmq_source : public gr::sync_block {
public:
    using sptr = std::shared_ptr<iq_zmq_source>;
    static sptr make(const std::string& endpoint, const std::string& topic, int rate_sps, int hwm) {
        return gnuradio::make_block_sptr<iq_zmq_source>(endpoint, topic, rate_sps, hwm);
    }
    iq_zmq_source(const std::string& endpoint, const std::string& topic, int rate_sps, int hwm)
        : gr::sync_block("iq_zmq_source", gr::io_signature::make(0, 0, 0), gr::io_signature::make(1, 1, sizeof(gr_complex))),
          topic_(topic), rate_(rate_sps), t_report_(now_s_()) {
        ctx_ = zmq_ctx_new();
        sock_ = zmq_socket(ctx_, ZMQ_SUB);
        int linger = 0, to = 200;                                   // never block forever: the scheduler must be able to stop us
        zmq_setsockopt(sock_, ZMQ_RCVHWM, &hwm, sizeof hwm);
        zmq_setsockopt(sock_, ZMQ_LINGER, &linger, sizeof linger);
        zmq_setsockopt(sock_, ZMQ_RCVTIMEO, &to, sizeof to);
        std::string sub = topic + "/";                              // prefix match: "iq/nxdn" alone would also receive "iq/nxdn48"
        zmq_setsockopt(sock_, ZMQ_SUBSCRIBE, sub.data(), sub.size());
        zmq_connect(sock_, endpoint.c_str());
    }
    ~iq_zmq_source() override { zmq_close(sock_); zmq_ctx_term(ctx_); }

    int work(int noutput_items, gr_vector_const_void_star&, gr_vector_void_star& output_items) override {
        gr_complex* out = (gr_complex*)output_items[0];
        report();
        if (pos_ >= pending_.size()) {
            pending_.clear();
            pos_ = 0;
            std::vector<std::string> fr;
            for (;;) {
                zmq_msg_t m;
                zmq_msg_init(&m);
                if (zmq_msg_recv(&m, sock_, 0) < 0) { zmq_msg_close(&m); return 0; }
                fr.emplace_back((const char*)zmq_msg_data(&m), zmq_msg_size(&m));
                int more = zmq_msg_more(&m);
                zmq_msg_close(&m);
                if (!more) break;
            }
            if (fr.size() != 3 || fr[2].size() % 8) { ++bad_; return 0; }
            // header: only "seq" and "rate_sps" are used (loss is counted from gaps in seq, never replaced by invented samples)
            long long seq = jnum(fr[1], "seq", -1), rate = jnum(fr[1], "rate_sps", -1);
            if (last_seq_ >= 0 && seq >= 0 && seq != last_seq_ + 1 && seq > last_seq_) lost_ += seq - last_seq_ - 1;
            if (seq >= 0) last_seq_ = seq;
            if (rate >= 0 && rate != rate_) ++rate_mismatch_;
            ++msgs_;
            pending_.resize(fr[2].size() / 8);
            std::memcpy(pending_.data(), fr[2].data(), fr[2].size());
        }
        size_t n = std::min<size_t>(noutput_items, pending_.size() - pos_);
        std::memcpy(out, pending_.data() + pos_, n * 8);
        pos_ += n;
        out_ += n;
        return (int)n;
    }
private:
    static long long jnum(const std::string& js, const char* key, long long dflt) {   // tiny extractor for a flat numeric JSON field
        std::string k = std::string("\"") + key + "\":";
        size_t p = js.find(k);
        if (p == std::string::npos) return dflt;
        return std::atoll(js.c_str() + p + k.size());
    }
    void report() {
        double now = now_s_();
        if (now - t_report_ >= 10.0) {
            std::printf("[iq_zmq_source %s] msgs=%lld lost_msgs=%lld bad=%lld rate_mismatch=%lld out=%.0f kSps backlog=%zu last_seq=%lld\n",
                        topic_.c_str(), msgs_, lost_, bad_, rate_mismatch_, out_ / (now - t_report_) / 1e3, pending_.size() - pos_, last_seq_);
            std::fflush(stdout);
            out_ = 0;
            t_report_ = now;
        }
    }
    void* ctx_;
    void* sock_;
    std::string topic_;
    int rate_;
    std::vector<gr_complex> pending_;
    size_t pos_ = 0;
    long long last_seq_ = -1, msgs_ = 0, lost_ = 0, bad_ = 0, rate_mismatch_ = 0, out_ = 0;
    double t_report_;
};


}  // namespace sc
