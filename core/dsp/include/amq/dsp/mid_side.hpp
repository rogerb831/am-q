#pragma once
// Mid/Side matrix (area 03 R1, R2). Textbook form without sqrt(2) scaling, so a Mid-only
// signal keeps its level:
//     encode  M = (L + R) / 2,  S = (L - R) / 2
//     decode  L = M + S,        R = M - S
// In double precision the round trip is exact to rounding (far below -140 dBFS). Works in place.

#include <cstdint>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define AMQ_MS_SSE2 1
#endif

namespace amq::dsp {

enum class MidSideMode : uint8_t { Pass, Encode, Decode };

inline void midSide(MidSideMode mode, double* a, double* b, uint32_t n) noexcept {
    switch (mode) {
        case MidSideMode::Pass: return;
        case MidSideMode::Encode: {
            uint32_t i = 0;
#if defined(AMQ_MS_SSE2)  // two samples per step; the same operations, so bit-identical (R2)
            const __m128d half = _mm_set1_pd(0.5);
            for (; i + 2 <= n; i += 2) {
                const __m128d l = _mm_loadu_pd(a + i), r = _mm_loadu_pd(b + i);
                _mm_storeu_pd(a + i, _mm_mul_pd(half, _mm_add_pd(l, r)));
                _mm_storeu_pd(b + i, _mm_mul_pd(half, _mm_sub_pd(l, r)));
            }
#endif
            for (; i < n; ++i) {
                const double l = a[i], r = b[i];
                a[i] = 0.5 * (l + r);
                b[i] = 0.5 * (l - r);
            }
            return;
        }
        case MidSideMode::Decode: {
            uint32_t i = 0;
#if defined(AMQ_MS_SSE2)
            for (; i + 2 <= n; i += 2) {
                const __m128d m = _mm_loadu_pd(a + i), sd = _mm_loadu_pd(b + i);
                _mm_storeu_pd(a + i, _mm_add_pd(m, sd));
                _mm_storeu_pd(b + i, _mm_sub_pd(m, sd));
            }
#endif
            for (; i < n; ++i) {
                const double m = a[i], s = b[i];
                a[i] = m + s;
                b[i] = m - s;
            }
            return;
        }
    }
}

}  // namespace amq::dsp
