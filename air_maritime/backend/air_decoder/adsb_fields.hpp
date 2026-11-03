// ADS-B (Mode-S extended squitter) message fields: what the bridge needs from gr-adsb's decoder, computed from the raw
// 112 bits per ICAO Doc 9871 / DO-260B. Differences from gr-adsb (deliberate):
//   * heading is the COMPASS heading in degrees from true north, 0..360 (gr-adsb reports the mathematical angle
//     atan2(v_ns, v_ew): -92.88 for a true heading of 182.88; the Python bridge's `% 360` turned that into 267.12)
//   * altitude is reported from every airborne-position message (gr-adsb only after its own CPR pair check passed)
//   * latitude / longitude are not decoded here: the bridge decodes validated positions from the raw frames
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace airdec {

struct AdsbFields {
    int df = 0;
    std::string icao;                       // 6 hex digits, lower case (as gr-adsb)
    std::optional<std::string> callsign;    // 8 characters, trailing spaces kept (as gr-adsb)
    std::optional<long> altitude_ft;
    std::optional<double> speed_kt, heading_deg;
    std::optional<long> vertical_rate_fpm;
    bool any() const { return callsign || altitude_ft || speed_kt || heading_deg || vertical_rate_fpm; }
};

inline long bits_val(const std::vector<uint8_t>& b, size_t a, size_t n) { long v = 0; for (size_t i = a; i < a + n; ++i) v = (v << 1) | b[i]; return v; }

inline AdsbFields decode_adsb(const std::vector<uint8_t>& b) {
    AdsbFields f;
    if (b.size() < 112) return f;
    f.df = (int)bits_val(b, 0, 5);
    char ic[8];
    std::snprintf(ic, sizeof ic, "%06lx", bits_val(b, 8, 24));
    f.icao = ic;
    int tc = (int)bits_val(b, 32, 5);
    if (tc >= 1 && tc <= 4) {                                          // identification: 8 x 6-bit characters
        static const char* CS = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";
        std::string cs;
        for (int i = 0; i < 8; ++i) cs += CS[bits_val(b, 40 + 6 * i, 6)];
        f.callsign = cs;
    } else if (tc >= 9 && tc <= 18) {                                  // airborne position: barometric altitude
        if (b[47]) {                                                    // Q bit: 25 ft steps
            long n = (bits_val(b, 40, 7) << 4) | bits_val(b, 48, 4);
            f.altitude_ft = n * 25 - 1000;
        }
    } else if (tc == 19) {                                             // airborne velocity
        int st = (int)bits_val(b, 37, 3);
        long vr_raw = bits_val(b, 69, 9);
        if (vr_raw != 0) f.vertical_rate_fpm = (b[68] ? -1 : 1) * (vr_raw - 1) * 64;
        if (st == 1 || st == 2) {
            long ew = bits_val(b, 46, 10), ns = bits_val(b, 57, 10);
            if (ew != 0 && ns != 0) {
                double k = st == 2 ? 4.0 : 1.0;
                double vew = (b[45] ? -1 : 1) * (ew - 1) * k, vns = (b[56] ? -1 : 1) * (ns - 1) * k;
                f.speed_kt = std::sqrt(vew * vew + vns * vns);
                double hd = std::atan2(vew, vns) * 180.0 / M_PI;
                f.heading_deg = hd < 0 ? hd + 360.0 : hd;
            }
        } else if (st == 3 || st == 4) {                               // airspeed + magnetic heading
            if (b[45]) f.heading_deg = bits_val(b, 46, 10) * 360.0 / 1024.0;
            long as = bits_val(b, 57, 10);
            if (as != 0) f.speed_kt = (double)(as - 1) * (st == 4 ? 4.0 : 1.0);
        }
    }
    return f;
}

}  // namespace airdec
