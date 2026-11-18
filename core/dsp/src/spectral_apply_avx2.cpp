// The spectral apply step in AVX2 (spectral_apply.hpp). Compiled with AVX2 for this file only
// (core/dsp/CMakeLists.txt) and called only when the CPU supports it. Intrinsics and raw pointers
// only, so no inline function shared with other files is compiled here with AVX2.

#include "amq/dsp/spectral_apply.hpp"

#if defined(AMQ_DYN_AVX2_BUILD)
#include <immintrin.h>
#endif

namespace amq::dsp {

#if defined(AMQ_DYN_AVX2_BUILD)

int spectralApplyAvx2(const SpectralApplyArgs& a, int k0, int k1) noexcept {
    const __m256d one = _mm256_set1_pd(1.0), zero = _mm256_setzero_pd(), inv = _mm256_set1_pd(a.inv);
    const __m128i lastV = _mm_set1_epi32(a.last), oneI = _mm_set1_epi32(1);
    int k = k0;
    for (; k + 3 <= k1; k += 4) {
        const __m128i ia = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a.band + k));
        const __m128i ib = _mm_min_epi32(_mm_add_epi32(ia, oneI), lastV);  // std::min(ia + 1, last)
        const __m256d ga = _mm256_i32gather_pd(a.grDb, ia, 8), gb = _mm256_i32gather_pd(a.grDb, ib, 8);
        const __m256d wa = _mm256_cvtps_pd(_mm_loadu_ps(a.weight + k));
        const __m256d frac = _mm256_mul_pd(_mm256_add_pd(_mm256_mul_pd(wa, ga), _mm256_mul_pd(_mm256_sub_pd(one, wa), gb)), inv);
        const __m256d on = _mm256_cmp_pd(frac, zero, _CMP_NLE_UQ);  // !(frac <= 0), NaN included
        if (_mm256_movemask_pd(on) == 0) continue;
        const __m256d capped = _mm256_blendv_pd(frac, one, _mm256_cmp_pd(one, frac, _CMP_LT_OQ));  // std::min(frac, 1.0)
        const __m256d db = _mm256_mul_pd(capped, _mm256_cvtps_pd(_mm_loadu_ps(a.relDb + k)));
        for (int j = 0; j < a.nc; ++j) {
            const __m256d c = _mm256_loadu_pd(a.comps[j] + k);
            _mm256_storeu_pd(a.comps[j] + k, _mm256_blendv_pd(c, _mm256_add_pd(c, db), on));
        }
    }
    return k;
}

#else

int spectralApplyAvx2(const SpectralApplyArgs&, int k0, int) noexcept { return k0; }

#endif

}  // namespace amq::dsp
