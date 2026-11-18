// Four bands of the dynamics dB chain in AVX2 lanes (dynamics_lanes.hpp). Compiled with AVX2
// enabled for this file only (core/dsp/CMakeLists.txt) and called only when the CPU supports it
// (cpuHasAvx2). The operations are the scalar kernel's, in the same order and without fused
// multiply-adds, so the results are the same bits (tests/test_dynamics.cpp).

#include "amq/dsp/dynamics_lanes.hpp"

#if defined(AMQ_DYN_AVX2_BUILD)
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace amq::dsp {

#if defined(AMQ_DYN_AVX2_BUILD)

namespace {

inline __m256d sel(__m256d m, __m256d a, __m256d b) { return _mm256_or_pd(_mm256_and_pd(m, a), _mm256_andnot_pd(m, b)); }
inline __m256d vabs(__m256d x) { return _mm256_andnot_pd(_mm256_set1_pd(-0.0), x); }
inline __m256d mask4(bool a, bool b, bool c, bool d) {
    return _mm256_castsi256_pd(_mm256_set_epi64x(d ? -1 : 0, c ? -1 : 0, b ? -1 : 0, a ? -1 : 0));
}
inline __m256d set4(double a, double b, double c, double d) { return _mm256_set_pd(d, c, b, a); }

// fastLog2, four lanes, the scalar form's operations.
inline __m256d log2pd(__m256d x) {
    const __m256i bits = _mm256_castpd_si256(x);
    const __m256i ex = _mm256_srli_epi64(bits, 52);
    const __m256i packed = _mm256_permutevar8x32_epi32(ex, _mm256_setr_epi32(0, 2, 4, 6, 0, 0, 0, 0));
    __m256d e = _mm256_sub_pd(_mm256_cvtepi32_pd(_mm256_castsi256_si128(packed)), _mm256_set1_pd(1023.0));
    const __m256i mbits = _mm256_or_si256(_mm256_and_si256(bits, _mm256_set1_epi64x(0x000fffffffffffffll)),
                                          _mm256_set1_epi64x(0x3ff0000000000000ll));
    __m256d m = _mm256_castsi256_pd(mbits);
    const __m256d big = _mm256_cmp_pd(m, _mm256_set1_pd(1.4142135623730951), _CMP_GT_OQ);
    m = sel(big, _mm256_mul_pd(m, _mm256_set1_pd(0.5)), m);
    e = sel(big, _mm256_add_pd(e, _mm256_set1_pd(1.0)), e);
    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d t = _mm256_div_pd(_mm256_sub_pd(m, one), _mm256_add_pd(m, one));
    const __m256d t2 = _mm256_mul_pd(t, t);
    __m256d p = _mm256_set1_pd(kLog2Series[10]);
    for (int k = 9; k >= 0; --k) p = _mm256_add_pd(_mm256_mul_pd(p, t2), _mm256_set1_pd(kLog2Series[k]));
    return _mm256_add_pd(e, _mm256_mul_pd(t, p));
}

inline __m256d smooth(__m256d t, __m256d& y, __m256d ca, __m256d cr) {
    const __m256d d = _mm256_sub_pd(t, y);
    const __m256d snap = _mm256_cmp_pd(vabs(d), _mm256_set1_pd(DbAttackRelease::kSnapDb), _CMP_LE_OQ);
    const __m256d c = sel(_mm256_cmp_pd(d, _mm256_setzero_pd(), _CMP_GT_OQ), ca, cr);
    y = sel(snap, t, _mm256_add_pd(y, _mm256_mul_pd(c, d)));
    return y;
}

}  // namespace

bool cpuHasAvx2() noexcept {
    static const bool has = [] {
#if defined(_MSC_VER)
        int r[4];
        __cpuid(r, 0);
        if (r[0] < 7) return false;
        __cpuidex(r, 1, 0);
        const bool osxsave = (r[2] & (1 << 27)) != 0, avx = (r[2] & (1 << 28)) != 0;
        if (!osxsave || !avx || (_xgetbv(0) & 6) != 6) return false;  // the OS saves the YMM state
        __cpuidex(r, 7, 0);
        return (r[1] & (1 << 5)) != 0;
#else
        unsigned a, b, c, d;
        if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 27)) || !(c & (1u << 28))) return false;
        unsigned lo, hi;
        __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        if ((lo & 6) != 6) return false;
        if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return false;
        return (b & (1u << 5)) != 0;
#endif
    }();
    return has;
}

void gainLaneQuadAvx2(GainLane* const L[4], double* const buf[4], uint32_t n) noexcept {
    GainLane &A = *L[0], &B = *L[1], &C = *L[2], &D = *L[3];
    auto v4 = [&](double GainLane::*f) { return set4(A.*f, B.*f, C.*f, D.*f); };
    const __m256d autoM = mask4(A.autoThr, B.autoThr, C.autoThr, D.autoThr);
    const __m256d threshold = v4(&GainLane::thresholdDb), offset = v4(&GainLane::offsetDb), tilt = v4(&GainLane::tiltDb);
    const __m256d knee = v4(&GainLane::kneeDb);
    const __m256d halfKnee = _mm256_mul_pd(_mm256_set1_pd(0.5), knee);
    const __m256d twoKnee = _mm256_mul_pd(_mm256_set1_pd(2.0), knee);
    const __m256d kneeOn = _mm256_cmp_pd(knee, _mm256_setzero_pd(), _CMP_GT_OQ);
    const __m256d capPos = set4(std::max(A.capDb, 0.0), std::max(B.capDb, 0.0), std::max(C.capDb, 0.0), std::max(D.capDb, 0.0));
    const __m256d inv = v4(&GainLane::inv), refSlow = v4(&GainLane::refSlow), refWarm = v4(&GainLane::refWarm);
    const __m256d fa = v4(&GainLane::fa), fr = v4(&GainLane::fr), sa = v4(&GainLane::sa), sr = v4(&GainLane::sr);
    const __m256d za = v4(&GainLane::za), zr = v4(&GainLane::zr);
    __m256d ref = v4(&GainLane::ref), fy = v4(&GainLane::fy), sy = v4(&GainLane::sy), zy = v4(&GainLane::zy);
    const __m256d zero = _mm256_setzero_pd(), oneD = _mm256_set1_pd(1.0);
    const __m256d inf = _mm256_set1_pd(std::numeric_limits<double>::infinity());
    const __m256d floorRef = _mm256_set1_pd(kRefFloorDb);
    GainLane* lanes[4] = {&A, &B, &C, &D};
    alignas(32) double out[4];
    for (uint32_t i = 0; i < n; ++i) {
        const __m256d env = set4(buf[0][i], buf[1][i], buf[2][i], buf[3][i]);
        __m256d level = sel(_mm256_cmp_pd(env, _mm256_set1_pd(3.1622776601683795e-8), _CMP_GT_OQ),
                            _mm256_mul_pd(_mm256_set1_pd(6.020599913279624), log2pd(env)), _mm256_set1_pd(kLevelFloorDb));
        level = sel(_mm256_cmp_pd(env, inf, _CMP_EQ_OQ), inf, level);
        const __m256d upd = _mm256_and_pd(autoM, _mm256_cmp_pd(_mm256_sub_pd(level, level), zero, _CMP_EQ_OQ));
        bool warm[4];
        for (int k = 0; k < 4; ++k) warm[k] = lanes[k]->refCount < lanes[k]->refWarmSamples;
        const __m256d c = sel(mask4(warm[0], warm[1], warm[2], warm[3]), refWarm, refSlow);
        __m256d next = sel(_mm256_cmp_pd(level, ref, _CMP_GT_OQ), level,
                           _mm256_add_pd(ref, _mm256_mul_pd(c, _mm256_sub_pd(level, ref))));
        next = sel(_mm256_cmp_pd(next, floorRef, _CMP_LT_OQ), floorRef, next);
        ref = sel(upd, next, ref);
        const int u = _mm256_movemask_pd(upd);
        for (int k = 0; k < 4; ++k)
            if ((u >> k & 1) && warm[k]) ++lanes[k]->refCount;
        const __m256d thr = _mm256_add_pd(sel(autoM, _mm256_sub_pd(ref, offset), threshold), tilt);
        const __m256d over = _mm256_sub_pd(level, thr);
        const __m256d inKnee = _mm256_and_pd(kneeOn, _mm256_cmp_pd(vabs(over), halfKnee, _CMP_LT_OQ));
        const __m256d uu = _mm256_add_pd(over, halfKnee);
        const __m256d g1 = _mm256_div_pd(_mm256_mul_pd(uu, uu), twoKnee);
        const __m256d g2 = sel(_mm256_cmp_pd(over, zero, _CMP_GT_OQ), over, zero);
        __m256d gr = sel(inKnee, g1, g2);
        gr = sel(_mm256_cmp_pd(capPos, gr, _CMP_LT_OQ), capPos, gr);
        const __m256d a = smooth(gr, fy, fa, fr);
        const __m256d b = smooth(gr, sy, sa, sr);
        const __m256d m = sel(_mm256_cmp_pd(a, b, _CMP_LT_OQ), b, a);
        const __m256d o = smooth(m, zy, za, zr);
        const __m256d wv = _mm256_mul_pd(o, inv);
        _mm256_store_pd(out, sel(_mm256_cmp_pd(oneD, wv, _CMP_LT_OQ), oneD, wv));
        for (int k = 0; k < 4; ++k) buf[k][i] = out[k];
    }
    alignas(32) double t[4];
    _mm256_store_pd(t, ref);
    for (int k = 0; k < 4; ++k) lanes[k]->ref = t[k];
    _mm256_store_pd(t, fy);
    for (int k = 0; k < 4; ++k) lanes[k]->fy = t[k];
    _mm256_store_pd(t, sy);
    for (int k = 0; k < 4; ++k) lanes[k]->sy = t[k];
    _mm256_store_pd(t, zy);
    for (int k = 0; k < 4; ++k) lanes[k]->zy = t[k];
}

// Lanes [begin, begin + 4k) of a GainLaneSet, one step: gainLaneQuadAvx2's operations with the
// state read from and written to the arrays.
size_t gainLanesStepAvx2(GainLaneSet& s, const double* env, size_t count) noexcept {
    const __m256d zero = _mm256_setzero_pd();
    const __m256d inf = _mm256_set1_pd(std::numeric_limits<double>::infinity());
    const __m256d floorRef = _mm256_set1_pd(kRefFloorDb);
    size_t i = 0;
    for (; i + 3 < count; i += 4) {
        auto ld = [&](const std::vector<double>& v) { return _mm256_loadu_pd(v.data() + i); };
        const __m256d autoM = mask4(s.autoThr[i] != 0, s.autoThr[i + 1] != 0, s.autoThr[i + 2] != 0, s.autoThr[i + 3] != 0);
        const __m256d threshold = ld(s.thresholdDb), offset = ld(s.offsetDb), tilt = ld(s.tiltDb), knee = ld(s.kneeDb);
        const __m256d halfKnee = _mm256_mul_pd(_mm256_set1_pd(0.5), knee);
        const __m256d twoKnee = _mm256_mul_pd(_mm256_set1_pd(2.0), knee);
        const __m256d kneeOn = _mm256_cmp_pd(knee, zero, _CMP_GT_OQ);
        const __m256d cap = ld(s.capDb);
        const __m256d capPos = sel(_mm256_cmp_pd(cap, zero, _CMP_GT_OQ), cap, zero);  // std::max(cap, 0.0)
        __m256d ref = ld(s.ref), fy = ld(s.fy), sy = ld(s.sy), zy = ld(s.zy);
        const __m256d e = _mm256_loadu_pd(env + i);
        __m256d level = sel(_mm256_cmp_pd(e, _mm256_set1_pd(3.1622776601683795e-8), _CMP_GT_OQ),
                            _mm256_mul_pd(_mm256_set1_pd(6.020599913279624), log2pd(e)), _mm256_set1_pd(kLevelFloorDb));
        level = sel(_mm256_cmp_pd(e, inf, _CMP_EQ_OQ), inf, level);
        const __m256d upd = _mm256_and_pd(autoM, _mm256_cmp_pd(_mm256_sub_pd(level, level), zero, _CMP_EQ_OQ));
        bool warm[4];
        for (int k = 0; k < 4; ++k) warm[k] = s.refCount[i + k] < s.refWarmSamples[i + k];
        const __m256d c = sel(mask4(warm[0], warm[1], warm[2], warm[3]), ld(s.refWarm), ld(s.refSlow));
        __m256d next = sel(_mm256_cmp_pd(level, ref, _CMP_GT_OQ), level, _mm256_add_pd(ref, _mm256_mul_pd(c, _mm256_sub_pd(level, ref))));
        next = sel(_mm256_cmp_pd(next, floorRef, _CMP_LT_OQ), floorRef, next);
        ref = sel(upd, next, ref);
        const int u = _mm256_movemask_pd(upd);
        for (int k = 0; k < 4; ++k)
            if ((u >> k & 1) && warm[k]) ++s.refCount[i + k];
        const __m256d thr = _mm256_add_pd(sel(autoM, _mm256_sub_pd(ref, offset), threshold), tilt);
        const __m256d over = _mm256_sub_pd(level, thr);
        const __m256d inKnee = _mm256_and_pd(kneeOn, _mm256_cmp_pd(vabs(over), halfKnee, _CMP_LT_OQ));
        const __m256d uu = _mm256_add_pd(over, halfKnee);
        const __m256d g1 = _mm256_div_pd(_mm256_mul_pd(uu, uu), twoKnee);
        const __m256d g2 = sel(_mm256_cmp_pd(over, zero, _CMP_GT_OQ), over, zero);
        __m256d gr = sel(inKnee, g1, g2);
        gr = sel(_mm256_cmp_pd(capPos, gr, _CMP_LT_OQ), capPos, gr);
        const __m256d a = smooth(gr, fy, ld(s.fa), ld(s.fr));
        const __m256d b = smooth(gr, sy, ld(s.sa), ld(s.sr));
        const __m256d m = sel(_mm256_cmp_pd(a, b, _CMP_LT_OQ), b, a);
        smooth(m, zy, ld(s.za), ld(s.zr));
        _mm256_storeu_pd(s.ref.data() + i, ref);
        _mm256_storeu_pd(s.fy.data() + i, fy);
        _mm256_storeu_pd(s.sy.data() + i, sy);
        _mm256_storeu_pd(s.zy.data() + i, zy);
    }
    return i;
}

#else

size_t gainLanesStepAvx2(GainLaneSet&, const double*, size_t) noexcept { return 0; }
bool cpuHasAvx2() noexcept { return false; }
void gainLaneQuadAvx2(GainLane* const L[4], double* const buf[4], uint32_t n) noexcept {
    for (int k = 0; k < 4; ++k) gainLaneScalar(*L[k], buf[k], n);
}

#endif

}  // namespace amq::dsp
