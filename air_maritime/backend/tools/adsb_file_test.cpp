// Run the C++ Mode-S demodulator + field decoder over a complex64 file, in chunks like the flowgraph's work() calls.
//   adsb_file_test <file.cf32> [rate=6000000] [chunk=100000] [seconds=0 (all)]
// Prints one line per CRC-valid frame: <abs time> <hex frame> <snr dB>, then the decoded fields.
#include "../air_decoder/adsb_fields.hpp"
#include "../air_decoder/modes_demod.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: adsb_file_test file.cf32 [rate] [chunk] [seconds]\n"); return 1; }
    double rate = argc > 2 ? std::atof(argv[2]) : 6e6;
    size_t chunk = argc > 3 ? (size_t)std::atol(argv[3]) : 100000;
    double secs = argc > 4 ? std::atof(argv[4]) : 0;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror("open"); return 1; }
    airdec::ModesDemodCore core(rate, 0.0);
    std::vector<std::complex<float>> buf(chunk);
    size_t total = 0, limit = secs > 0 ? (size_t)(secs * rate) : (size_t)-1;
    long frames = 0;
    while (total < limit) {
        size_t n = std::fread(buf.data(), 8, std::min(chunk, limit - total), f);
        if (n == 0) break;
        core.process(buf.data(), n, [&](const airdec::Frame& fr) {
            ++frames;
            std::string hex;
            for (size_t i = 0; i < fr.bits.size(); i += 4) { int v = fr.bits[i] * 8 + fr.bits[i + 1] * 4 + fr.bits[i + 2] * 2 + fr.bits[i + 3]; hex += "0123456789ABCDEF"[v]; }
            auto d = airdec::decode_adsb(fr.bits);
            std::printf("%.7f %s %.2f | %s", fr.timestamp, hex.c_str(), fr.snr_db, d.icao.c_str());
            if (d.callsign) std::printf(" cs=%s", d.callsign->c_str());
            if (d.altitude_ft) std::printf(" alt=%ld", *d.altitude_ft);
            if (d.speed_kt) std::printf(" spd=%.3f hdg=%.3f", *d.speed_kt, *d.heading_deg);
            if (d.vertical_rate_fpm) std::printf(" vr=%ld", *d.vertical_rate_fpm);
            std::printf("\n");
        });
        total += n;
    }
    std::fprintf(stderr, "%ld frames in %.1f s of IQ\n", frames, (double)total / rate);
    return 0;
}
