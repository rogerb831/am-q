#pragma once
// The dB-domain half of the dynamic EQ detector, vectorised across bands (area 06; plan item
// "SIMD across bands for dynamics"). Each band's side-chain envelope is computed per band (its
// filters differ); everything after it is the same arithmetic for every band:
//
//   level = detectorLevelDb(env)
//   thr   = (auto ? referenceEnvelope(level) - min(|range|, 30) : threshold) + tilt
//   gr    = gainComputerDb(level - thr, knee, cap)
//   out   = final(max(fast(gr), slow(gr)))          (BandDynamics)
//   w     = min(out / cap, 1)
//
// GainLane holds one band's state for a block. gainLaneScalar runs one band, gainLanePair runs two
// in SSE2 lanes; both perform the same operations in the same order, so they give the same bits
// (tests/test_dynamics.cpp checks this, and that both match the object chain in dynamics.hpp).

#include "amq/dsp/dynamics.hpp"

#include <cstdint>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define AMQ_DYN_SSE2 1
#endif

namespace amq::dsp {

struct GainLane {
    bool autoThr = true;
    double thresholdDb = -20.0, offsetDb = 0.0;  // offset = min(|range|, 30): the auto threshold gap
    // Added to the threshold after it is formed: 0 for a dynamic band (x + 0 leaves x, so the bits
    // do not change), Spectral Tilt's offset for a spectral analysis band (area 07).
    double tiltDb = 0.0;
    double kneeDb = 6.0, capDb = 0.0, inv = 0.0;  // cap = |range|; inv = 1 / cap
    double ref = kRefFloorDb, refSlow = 0.0, refWarm = 0.0;
    uint64_t refCount = 0, refWarmSamples = 0;
    double fy = 0.0, fa = 1.0, fr = 1.0;  // fast stage: value, attack and release coefficients
    double sy = 0.0, sa = 1.0, sr = 1.0;  // slow stage
    double zy = 0.0, za = 1.0, zr = 1.0;  // final stage

    void load(const ReferenceEnvelope& r, BandDynamics& d) noexcept {
        ref = r.value(), refSlow = r.slowCoefficient(), refWarm = r.warmCoefficient();
        refCount = r.count(), refWarmSamples = r.warmSamples();
        fy = d.fast().value(), fa = d.fast().attackCoefficient(), fr = d.fast().releaseCoefficient();
        sy = d.slow().value(), sa = d.slow().attackCoefficient(), sr = d.slow().releaseCoefficient();
        zy = d.finalStage().value(), za = d.finalStage().attackCoefficient(), zr = d.finalStage().releaseCoefficient();
    }
    void store(ReferenceEnvelope& r, BandDynamics& d) const noexcept {
        r.setState(ref, refCount);
        d.fast().setValue(fy);
        d.slow().setValue(sy);
        d.finalStage().setValue(zy);
    }
};

namespace detail {
inline double smooth(double t, double& y, double ca, double cr) noexcept {  // DbAttackRelease::process, finite t
    const double d = t - y;
    if (std::abs(d) <= DbAttackRelease::kSnapDb) y = t;
    else y += (d > 0.0 ? ca : cr) * d;
    return y;
}
}  // namespace detail

// One band: buf holds the side-chain envelope on entry and the gain-reduction weight on return.
inline void gainLaneScalar(GainLane& L, double* buf, uint32_t n) noexcept {
    const double capPos = std::max(L.capDb, 0.0);
    for (uint32_t i = 0; i < n; ++i) {
        const double level = detectorLevelDb(buf[i]);
        double thr = L.thresholdDb;
        if (L.autoThr) {
            if (std::isfinite(level)) {  // ReferenceEnvelope::process
                if (level > L.ref) L.ref = level;
                else L.ref += (L.refCount < L.refWarmSamples ? L.refWarm : L.refSlow) * (level - L.ref);
                if (L.ref < kRefFloorDb) L.ref = kRefFloorDb;
                if (L.refCount < L.refWarmSamples) ++L.refCount;
            }
            thr = L.ref - L.offsetDb;
        }
        thr += L.tiltDb;
        const double over = level - thr;
        double gr;  // gainComputerDb
        if (L.kneeDb > 0.0 && std::abs(over) < 0.5 * L.kneeDb) {
            const double u = over + 0.5 * L.kneeDb;
            gr = u * u / (2.0 * L.kneeDb);
        } else {
            gr = over > 0.0 ? over : 0.0;
        }
        gr = std::min(gr, capPos);
        const double a = detail::smooth(gr, L.fy, L.fa, L.fr);
        const double b = detail::smooth(gr, L.sy, L.sa, L.sr);
        const double out = detail::smooth(std::max(a, b), L.zy, L.za, L.zr);
        buf[i] = std::min(out * L.inv, 1.0);
    }
}

#if defined(AMQ_DYN_SSE2)
namespace detail {
inline __m128d sel(__m128d m, __m128d a, __m128d b) noexcept { return _mm_or_pd(_mm_and_pd(m, a), _mm_andnot_pd(m, b)); }
inline __m128d vabs(__m128d x) noexcept { return _mm_andnot_pd(_mm_set1_pd(-0.0), x); }
// fastLog2 in two lanes, the same operations as the scalar form.
inline __m128d log2pd(__m128d x) noexcept {
    const __m128i bits = _mm_castpd_si128(x);
    const __m128i ex = _mm_srli_epi64(bits, 52);  // sign bit is 0 for positive x
    const __m128i ex32 = _mm_shuffle_epi32(ex, _MM_SHUFFLE(3, 3, 2, 0));
    __m128d e = _mm_sub_pd(_mm_cvtepi32_pd(ex32), _mm_set1_pd(1023.0));
    const __m128i mbits = _mm_or_si128(_mm_and_si128(bits, _mm_set1_epi64x(0x000fffffffffffffll)),
                                       _mm_set1_epi64x(0x3ff0000000000000ll));
    __m128d m = _mm_castsi128_pd(mbits);
    const __m128d big = _mm_cmpgt_pd(m, _mm_set1_pd(1.4142135623730951));
    m = sel(big, _mm_mul_pd(m, _mm_set1_pd(0.5)), m);
    e = sel(big, _mm_add_pd(e, _mm_set1_pd(1.0)), e);
    const __m128d one = _mm_set1_pd(1.0);
    const __m128d t = _mm_div_pd(_mm_sub_pd(m, one), _mm_add_pd(m, one));
    const __m128d t2 = _mm_mul_pd(t, t);
    __m128d p = _mm_set1_pd(kLog2Series[10]);
    for (int k = 9; k >= 0; --k) p = _mm_add_pd(_mm_mul_pd(p, t2), _mm_set1_pd(kLog2Series[k]));
    return _mm_add_pd(e, _mm_mul_pd(t, p));
}
inline __m128d smoothpd(__m128d t, __m128d& y, __m128d ca, __m128d cr) noexcept {
    const __m128d d = _mm_sub_pd(t, y);
    const __m128d snap = _mm_cmple_pd(vabs(d), _mm_set1_pd(DbAttackRelease::kSnapDb));
    const __m128d c = sel(_mm_cmpgt_pd(d, _mm_setzero_pd()), ca, cr);
    y = sel(snap, t, _mm_add_pd(y, _mm_mul_pd(c, d)));
    return y;
}
}  // namespace detail
#endif

// Two bands in SSE2 lanes; falls back to two scalar runs without SSE2.
inline void gainLanePair(GainLane& A, GainLane& B, double* bufA, double* bufB, uint32_t n) noexcept {
#if defined(AMQ_DYN_SSE2)
    using namespace detail;
    auto pd = [](double a, double b) { return _mm_set_pd(b, a); };  // lane 0 = A
    auto bmask = [](bool a, bool b) {
        return _mm_castsi128_pd(_mm_set_epi64x(b ? -1 : 0, a ? -1 : 0));
    };
    const __m128d autoM = bmask(A.autoThr, B.autoThr);
    const __m128d threshold = pd(A.thresholdDb, B.thresholdDb), offset = pd(A.offsetDb, B.offsetDb);
    const __m128d tilt = pd(A.tiltDb, B.tiltDb);
    const __m128d knee = pd(A.kneeDb, B.kneeDb), halfKnee = _mm_mul_pd(_mm_set1_pd(0.5), knee);
    const __m128d twoKnee = _mm_mul_pd(_mm_set1_pd(2.0), knee);
    const __m128d kneeOn = _mm_cmpgt_pd(knee, _mm_setzero_pd());
    const __m128d capPos = pd(std::max(A.capDb, 0.0), std::max(B.capDb, 0.0)), inv = pd(A.inv, B.inv);
    const __m128d refSlow = pd(A.refSlow, B.refSlow), refWarm = pd(A.refWarm, B.refWarm);
    const __m128d fa = pd(A.fa, B.fa), fr = pd(A.fr, B.fr), sa = pd(A.sa, B.sa), sr = pd(A.sr, B.sr);
    const __m128d za = pd(A.za, B.za), zr = pd(A.zr, B.zr);
    const __m128d floorLevel = _mm_set1_pd(kLevelFloorDb), floorRef = _mm_set1_pd(kRefFloorDb);
    const __m128d minEnv = _mm_set1_pd(3.1622776601683795e-8), dbPerOct = _mm_set1_pd(6.020599913279624);
    const __m128d inf = _mm_set1_pd(std::numeric_limits<double>::infinity());
    const __m128d zero = _mm_setzero_pd(), oneD = _mm_set1_pd(1.0);
    __m128d ref = pd(A.ref, B.ref), fy = pd(A.fy, B.fy), sy = pd(A.sy, B.sy), zy = pd(A.zy, B.zy);
    for (uint32_t i = 0; i < n; ++i) {
        const __m128d env = pd(bufA[i], bufB[i]);
        __m128d level = sel(_mm_cmpgt_pd(env, minEnv), _mm_mul_pd(dbPerOct, log2pd(env)), floorLevel);
        level = sel(_mm_cmpeq_pd(env, inf), inf, level);
        // Reference envelope, in auto lanes with a finite level.
        const __m128d finite = _mm_cmpeq_pd(_mm_sub_pd(level, level), zero);
        const __m128d upd = _mm_and_pd(autoM, finite);
        const __m128d warm = bmask(A.refCount < A.refWarmSamples, B.refCount < B.refWarmSamples);
        const __m128d c = sel(warm, refWarm, refSlow);
        __m128d next = sel(_mm_cmpgt_pd(level, ref), level, _mm_add_pd(ref, _mm_mul_pd(c, _mm_sub_pd(level, ref))));
        next = sel(_mm_cmplt_pd(next, floorRef), floorRef, next);
        ref = sel(upd, next, ref);
        const int u = _mm_movemask_pd(upd);
        if ((u & 1) && A.refCount < A.refWarmSamples) ++A.refCount;
        if ((u & 2) && B.refCount < B.refWarmSamples) ++B.refCount;
        const __m128d thr = _mm_add_pd(sel(autoM, _mm_sub_pd(ref, offset), threshold), tilt);
        // Gain computer.
        const __m128d over = _mm_sub_pd(level, thr);
        const __m128d inKnee = _mm_and_pd(kneeOn, _mm_cmplt_pd(vabs(over), halfKnee));
        const __m128d uu = _mm_add_pd(over, halfKnee);
        const __m128d g1 = _mm_div_pd(_mm_mul_pd(uu, uu), twoKnee);
        const __m128d g2 = sel(_mm_cmpgt_pd(over, zero), over, zero);
        __m128d gr = sel(inKnee, g1, g2);
        gr = sel(_mm_cmplt_pd(capPos, gr), capPos, gr);  // std::min(gr, cap)
        // Dual release and the final stage.
        const __m128d a = smoothpd(gr, fy, fa, fr);
        const __m128d b = smoothpd(gr, sy, sa, sr);
        const __m128d m = sel(_mm_cmplt_pd(a, b), b, a);  // std::max(a, b)
        const __m128d out = smoothpd(m, zy, za, zr);
        const __m128d wv = _mm_mul_pd(out, inv);
        const __m128d w = sel(_mm_cmplt_pd(oneD, wv), oneD, wv);  // std::min(w, 1)
        _mm_storel_pd(bufA + i, w);
        _mm_storeh_pd(bufB + i, w);
    }
    double tmp[2];
    _mm_storeu_pd(tmp, ref), A.ref = tmp[0], B.ref = tmp[1];
    _mm_storeu_pd(tmp, fy), A.fy = tmp[0], B.fy = tmp[1];
    _mm_storeu_pd(tmp, sy), A.sy = tmp[0], B.sy = tmp[1];
    _mm_storeu_pd(tmp, zy), A.zy = tmp[0], B.zy = tmp[1];
#else
    gainLaneScalar(A, bufA, n);
    gainLaneScalar(B, bufB, n);
#endif
}

// Four bands in AVX2 lanes (core/dsp/src/dynamics_avx2.cpp, built with AVX2 for that file only).
// Many lanes, one step each: the spectral stage's analysis bands run the same chain once per
// frame (area 07). The state is a structure of arrays, so a step loads whole vectors instead of
// gathering fields from separate lanes. Lane i of a step gives the same bits as gainLaneScalar on
// lane(i) with one sample (tests/test_dynamics.cpp).
struct GainLaneSet {
    std::vector<double> thresholdDb, offsetDb, tiltDb, kneeDb, capDb, inv;
    std::vector<double> ref, refSlow, refWarm, fy, fa, fr, sy, sa, sr, zy, za, zr;
    std::vector<uint64_t> refCount, refWarmSamples;
    std::vector<uint8_t> autoThr;

    void resize(size_t n) {
        for (auto* v : {&thresholdDb, &offsetDb, &tiltDb, &kneeDb, &capDb, &inv, &ref, &refSlow, &refWarm, &fy, &fa, &fr, &sy,
                        &sa, &sr, &zy, &za, &zr})
            v->assign(n, 0.0);
        refCount.assign(n, 0), refWarmSamples.assign(n, 0), autoThr.assign(n, 0);
    }
    [[nodiscard]] GainLane lane(size_t i) const noexcept {
        GainLane L;
        L.autoThr = autoThr[i] != 0, L.thresholdDb = thresholdDb[i], L.offsetDb = offsetDb[i], L.tiltDb = tiltDb[i];
        L.kneeDb = kneeDb[i], L.capDb = capDb[i], L.inv = inv[i];
        L.ref = ref[i], L.refSlow = refSlow[i], L.refWarm = refWarm[i], L.refCount = refCount[i], L.refWarmSamples = refWarmSamples[i];
        L.fy = fy[i], L.fa = fa[i], L.fr = fr[i], L.sy = sy[i], L.sa = sa[i], L.sr = sr[i], L.zy = zy[i], L.za = za[i], L.zr = zr[i];
        return L;
    }
    void setLane(size_t i, const GainLane& L) noexcept {
        autoThr[i] = L.autoThr ? 1 : 0, thresholdDb[i] = L.thresholdDb, offsetDb[i] = L.offsetDb, tiltDb[i] = L.tiltDb;
        kneeDb[i] = L.kneeDb, capDb[i] = L.capDb, inv[i] = L.inv;
        ref[i] = L.ref, refSlow[i] = L.refSlow, refWarm[i] = L.refWarm, refCount[i] = L.refCount, refWarmSamples[i] = L.refWarmSamples;
        fy[i] = L.fy, fa[i] = L.fa, fr[i] = L.fr, sy[i] = L.sy, sa[i] = L.sa, sr[i] = L.sr, zy[i] = L.zy, za[i] = L.za, zr[i] = L.zr;
    }
    // The state and timing coefficients of lane i from / to the object chain it stands for.
    void load(size_t i, const ReferenceEnvelope& r, BandDynamics& d) noexcept {
        GainLane L = lane(i);
        L.load(r, d);
        setLane(i, L);
    }
    void store(size_t i, ReferenceEnvelope& r, BandDynamics& d) const noexcept { lane(i).store(r, d); }
};

// One step of lanes [0, count): env[i] is lane i's side-chain envelope. Afterwards zy[i] holds its
// gain reduction in dB (the final stage's value). AVX2 four lanes at a time where the CPU has it.
void gainLanesStep(GainLaneSet& s, const double* env, size_t count) noexcept;
// The AVX2 part of a step (call only when cpuHasAvx2() is true): lanes [0, 4k), returning 4k.
size_t gainLanesStepAvx2(GainLaneSet& s, const double* env, size_t count) noexcept;

// Call only when cpuHasAvx2() is true; same bits as gainLaneScalar.
[[nodiscard]] bool cpuHasAvx2() noexcept;
void gainLaneQuadAvx2(GainLane* const L[4], double* const buf[4], uint32_t n) noexcept;

}  // namespace amq::dsp
