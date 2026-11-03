// Mode-S / ADS-B demodulator: C++ port of airmaritime/modes_demod.py (same algorithm, same constants).
// Input: complex IQ at fs (an even number of samples per microsecond). Magnitude is integrated over each half-bit,
// a preamble test (pulses at 0, 1, 3.5, 4.5 us, quiet in between) finds candidates relative to the measured noise
// floor (median) -- no absolute threshold -- then 112 PPM bits are sliced at a few sub-bit timing offsets and only
// DF17/18 frames whose CRC-24 remainder is 0 are reported.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <complex>
#include <functional>
#include <map>
#include <vector>

namespace airdec {

struct Frame { std::vector<uint8_t> bits; double timestamp; double snr_db; };

class ModesDemodCore {
public:
    static constexpr int NBITS = 112;
    static constexpr uint32_t POLY = 0xFFF409;

    ModesDemodCore(double fs, double start_ts, double snr_db = 6.0) : fs_(fs), start_ts_(start_ts) {
        double sps = fs / 1e6;
        h_ = (int)sps / 2;                                               // samples per half-bit (0.5 us)
        frame_len_ = 16 * h_ + 2 * h_ * NBITS + 4 * h_;
        min_ratio_ = std::pow(10.0, snr_db / 10.0);
        delay_ = (size_t)(frame_len_ + 32 * h_);                          // look-behind carried between calls
        tail_.assign(delay_, 0.0f);
    }

    // One work() call: n_in new samples. Frames whose start is among the samples being "written" in this call are reported.
    void process(const std::complex<float>* x, size_t n_in, const std::function<void(const Frame&)>& emit) {
        std::vector<float> m(tail_.size() + n_in);
        std::copy(tail_.begin(), tail_.end(), m.begin());
        for (size_t i = 0; i < n_in; ++i) m[tail_.size() + i] = x[i].real() * x[i].real() + x[i].imag() * x[i].imag();
        scan(m, written_, n_in, emit);
        tail_.assign(m.begin() + n_in, m.end());
        written_ += n_in;
    }

    static uint32_t crc24(const uint8_t* bits) {
        uint32_t reg = 0;
        for (int i = 0; i < NBITS; ++i) {
            uint32_t top = ((reg >> 23) & 1) ^ bits[i];
            reg = ((reg << 1) & 0xFFFFFF) ^ (top * POLY);
        }
        return reg;
    }

private:
    void scan(const std::vector<float>& m, long long base_abs, size_t n_out, const std::function<void(const Frame&)>& emit) {
        const int h = h_;
        const size_t N = m.size();
        if (N < (size_t)h) return;
        std::vector<double> c(N + 1, 0.0);                                // float64 cumulative sum, like numpy
        for (size_t i = 0; i < N; ++i) c[i + 1] = c[i] + (double)m[i];
        const size_t ns = N + 1 - h;
        std::vector<float> s(ns);
        for (size_t i = 0; i < ns; ++i) s[i] = (float)(c[i + h] - c[i]);
        // noise = median(s) + 1e-20 (numpy: mean of the two middle values for an even count)
        std::vector<float> tmp(s);
        size_t mid = ns / 2;
        std::nth_element(tmp.begin(), tmp.begin() + mid, tmp.end());
        float upper = tmp[mid], med = upper;
        if (ns % 2 == 0) {
            float lower = *std::max_element(tmp.begin(), tmp.begin() + mid);
            med = (lower + upper) / 2.0f;
        }
        double noise = (double)med + 1e-20;
        long long last = (long long)ns - (16 * h + 2 * h * NBITS + 2 * h);
        if (last <= 0) return;
        const float thr = (float)(min_ratio_ * noise);                    // numpy compares float32 data with a weak python scalar
        std::vector<long long> cand;
        std::vector<float> hi_all(last);
        for (long long t = 0; t < last; ++t) {
            float hi = std::min(std::min(s[t], s[t + 2 * h]), std::min(s[t + 7 * h], s[t + 9 * h]));
            float lo = std::max(std::max(std::max(s[t + 4 * h], s[t + 5 * h]), std::max(s[t + 6 * h], s[t + 11 * h])),
                                std::max(s[t + 12 * h], s[t + 13 * h]));
            hi_all[t] = hi;
            if (hi > thr && hi > 1.5f * lo) cand.push_back(t);
        }
        if (cand.empty()) return;
        // strongest start in each cluster of adjacent candidates (first maximum wins, like argmax)
        std::vector<long long> starts;
        size_t a = 0;
        for (size_t i = 1; i <= cand.size(); ++i) {
            if (i == cand.size() || cand[i] - cand[i - 1] > 2 * h) {
                size_t best = a;
                for (size_t j = a + 1; j < i; ++j) if (hi_all[cand[j]] > hi_all[cand[best]]) best = j;
                starts.push_back(cand[best]);
                a = i;
            }
        }
        const long long klast = (long long)(NBITS - 1) * 2 * h;
        std::map<long long, std::pair<std::vector<uint8_t>, long long>> best;           // key -> (bits, t0), ordered by key
        for (int off = -(h / 2); off <= h / 2; ++off) {
            for (long long st0 : starts) {
                long long st = st0 + off;
                if (!(st >= 0 && st + 16 * h + klast + h < (long long)ns)) continue;
                uint8_t bits[NBITS];
                for (int k = 0; k < NBITS; ++k) {
                    float first = s[st + 16 * h + (long long)k * 2 * h], second = s[st + 17 * h + (long long)k * 2 * h];
                    bits[k] = first > second ? 1 : 0;
                }
                int df = bits[0] * 16 + bits[1] * 8 + bits[2] * 4 + bits[3] * 2 + bits[4];
                if (!(df == 17 || df == 18)) continue;
                if (crc24(bits) != 0) continue;
                long long key = st - off;
                if (!best.count(key)) best[key] = {std::vector<uint8_t>(bits, bits + NBITS), st};
            }
        }
        for (auto& [key, v] : best) {
            long long t0 = v.second;
            if (t0 >= (long long)n_out) continue;                          // not written yet: found again on a later call
            long long abs_t = base_abs + t0;
            if (abs_t - last_frame_abs_ < 16 * h + 2 * h * NBITS) continue;   // same frame seen again via the tail overlap
            last_frame_abs_ = abs_t;
            double snr = 10 * std::log10(std::max((double)s[t0], 1e-20) / noise);
            emit(Frame{v.first, start_ts_ + (double)abs_t / fs_, snr});
        }
    }

    double fs_, start_ts_, min_ratio_;
    int h_;
    int frame_len_;
    size_t delay_;
    std::vector<float> tail_;
    long long written_ = 0;
    long long last_frame_abs_ = -1000000000000LL;
};

}  // namespace airdec
