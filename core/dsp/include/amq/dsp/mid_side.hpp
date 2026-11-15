#pragma once
// Mid/Side matrix (area 03 R1, R2). Textbook form without sqrt(2) scaling, so a Mid-only
// signal keeps its level:
//     encode  M = (L + R) / 2,  S = (L - R) / 2
//     decode  L = M + S,        R = M - S
// In double precision the round trip is exact to rounding (far below -140 dBFS). Works in place.

#include <cstdint>

namespace amq::dsp {

enum class MidSideMode : uint8_t { Pass, Encode, Decode };

inline void midSide(MidSideMode mode, double* a, double* b, uint32_t n) noexcept {
    switch (mode) {
        case MidSideMode::Pass: return;
        case MidSideMode::Encode:
            for (uint32_t i = 0; i < n; ++i) {
                const double l = a[i], r = b[i];
                a[i] = 0.5 * (l + r);
                b[i] = 0.5 * (l - r);
            }
            return;
        case MidSideMode::Decode:
            for (uint32_t i = 0; i < n; ++i) {
                const double m = a[i], s = b[i];
                a[i] = m + s;
                b[i] = m - s;
            }
            return;
    }
}

}  // namespace amq::dsp
