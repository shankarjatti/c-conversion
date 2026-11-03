#include "audio.h"

#include <QMutexLocker>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <pulse/error.h>
#include <pulse/simple.h>

static pa_simple* open_stream(int rate, const char* name, QString* err) {
    pa_sample_spec ss{PA_SAMPLE_S16LE, (uint32_t)rate, 1};
    pa_buffer_attr attr{(uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1};
    attr.tlength = rate / 5 * 2;                                  // ~200 ms of buffering
    int e = 0;
    pa_simple* s = pa_simple_new(nullptr, "SecureCOM console", PA_STREAM_PLAYBACK, nullptr, name, &ss, nullptr, &attr, &e);
    if (!s && err) *err = QString::fromUtf8(pa_strerror(e));
    return s;
}

void AudioMixer::setListen(bool on) {
    on_ = on;
    if (!on) { QMutexLocker l(&mu_); ch_.clear(); }
}

void AudioMixer::push(const QString& band, int rate, const QByteArray& pcm) {
    if (!on_) return;
    QMutexLocker l(&mu_);
    Chan& c = ch_[band];
    c.rate = rate;
    const int16_t* s = (const int16_t*)pcm.constData();
    int n = pcm.size() / 2;
    for (int i = 0; i < n; ++i) c.q.push_back(s[i] / 32768.0f);
    while ((int)c.q.size() > rate) c.q.pop_front();                // never more than 1 s queued: drop the oldest, stay live
}

void AudioMixer::run() {
    const int OUT = 48000, CH = OUT / 50;                          // 20 ms blocks
    pa_simple* s = nullptr;
    std::vector<int16_t> block(CH);
    while (!quit_) {
        if (!on_) {
            if (s) { pa_simple_free(s); s = nullptr; }
            msleep(50);
            continue;
        }
        if (!s) {
            s = open_stream(OUT, "decoded audio", &err_);
            if (!s) { emit failed(err_); on_ = false; continue; }
        }
        std::vector<float> acc(CH, 0.0f);
        {
            QMutexLocker l(&mu_);
            for (auto it = ch_.begin(); it != ch_.end(); ++it) {   // each band resampled (linear) to 48 kHz and summed
                Chan& c = it.value();
                double step = (double)c.rate / OUT;
                for (int i = 0; i < CH; ++i) {
                    size_t i0 = (size_t)c.phase;
                    if (i0 + 1 >= c.q.size()) break;
                    double fr = c.phase - i0;
                    acc[i] += (float)(c.q[i0] * (1 - fr) + c.q[i0 + 1] * fr);
                    c.phase += step;
                }
                size_t drop = std::min((size_t)c.phase, c.q.size());
                c.q.erase(c.q.begin(), c.q.begin() + drop);
                c.phase -= drop;
            }
        }
        for (int i = 0; i < CH; ++i) block[i] = (int16_t)std::max(-32768.0f, std::min(32767.0f, acc[i] * 32767.0f));
        int e = 0;
        if (pa_simple_write(s, block.data(), CH * 2, &e) < 0) { err_ = QString::fromUtf8(pa_strerror(e)); emit failed(err_); pa_simple_free(s); s = nullptr; on_ = false; }
    }
    if (s) pa_simple_free(s);
}

bool WavPlayer::parse() {
    const char* d = wav_.constData();
    qint64 n = wav_.size();
    if (n < 44 || std::memcmp(d, "RIFF", 4) || std::memcmp(d + 8, "WAVE", 4)) return false;
    qint64 pos = 12;
    int bits = 0, ch = 0;
    while (pos + 8 <= n) {
        uint32_t sz;
        std::memcpy(&sz, d + pos + 4, 4);
        if (!std::memcmp(d + pos, "fmt ", 4) && pos + 24 <= n) {
            uint16_t fmt, c, b;
            uint32_t r;
            std::memcpy(&fmt, d + pos + 8, 2); std::memcpy(&c, d + pos + 10, 2); std::memcpy(&r, d + pos + 12, 4); std::memcpy(&b, d + pos + 22, 2);
            if (fmt != 1) return false;
            rate_ = (int)r; ch = c; bits = b;
        } else if (!std::memcmp(d + pos, "data", 4)) {
            dataOff_ = pos + 8;
            qint64 avail = n - dataOff_;
            frames_ = std::min<qint64>(sz, avail) / 2;
            return rate_ > 0 && bits == 16 && ch == 1;
        }
        pos += 8 + sz + (sz & 1);
    }
    return false;
}

void WavPlayer::run() {
    QString err;
    pa_simple* s = open_stream(rate_, "recording", &err);
    if (!s) { emit finished2(err); return; }
    const int16_t* data = (const int16_t*)(wav_.constData() + dataOff_);
    qint64 pos = 0;
    const int CH = std::max(1, rate_ / 50);
    int tick = 0;
    while (!quit_ && pos < frames_) {
        if (paused_) { msleep(40); continue; }
        int n = (int)std::min<qint64>(CH, frames_ - pos);
        int e = 0;
        if (pa_simple_write(s, data + pos, n * 2, &e) < 0) { err = QString::fromUtf8(pa_strerror(e)); break; }
        pos += n;
        if (++tick % 5 == 0) emit position((double)pos / rate_);
    }
    if (!quit_ && err.isEmpty()) pa_simple_drain(s, nullptr);
    pa_simple_free(s);
    emit finished2(err);
}
