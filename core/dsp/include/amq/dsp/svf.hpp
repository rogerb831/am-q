#pragma once
// TPT / ZDF state-variable filter kernel (area 01 R18; Zavalishin, "The Art of VA Filter
// Design"; Simper, "Solving the continuous SVF equations using trapezoidal integration").
//
// Any strictly stable digital biquad is realised exactly by a trapezoidal SVF plus an output
// mix. With sigma = (z - 1) / (z + 1), the trapezoidal SVF has
//     band v1 = g*sigma / D,   low v2 = g^2 / D,   D = sigma^2 + k*g*sigma + g^2,
// and the output m0*x + m1*v1 + m2*v2. Substituting z = (1 + sigma) / (1 - sigma) into the
// biquad and matching D and the numerator gives g, k, m0, m1 and m2 (svfFromBiquad). The
// designed transfer function is kept exactly; the SVF brings well-behaved coefficient
// modulation (any g > 0, k > 0 is stable, so interpolating g and k between designs is safe)
// and good low-frequency precision. State is double.

#include "amq/dsp/digital_design.hpp"

#include <cmath>
#include <complex>
#include <cstdint>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define AMQ_SVF_SSE2 1
#endif

namespace amq::dsp {

// Interpolatable parameters of one SVF section.
struct SvfParams {
    double g = 1.0, k = 2.0;              // pre-warped cutoff and damping (g > 0, k > 0)
    double m0 = 1.0, m1 = 0.0, m2 = 0.0;  // output mix
};

// Per-sample coefficients derived from SvfParams.
struct SvfCoeffs {
    double a1 = 1.0, a2 = 0.0, a3 = 0.0;  // 1/(1+g(g+k)), g*a1, g*a2
    double m0 = 1.0, m1 = 0.0, m2 = 0.0;
};

struct SvfState {
    double ic1 = 0.0, ic2 = 0.0;
};

[[nodiscard]] inline SvfCoeffs svfCoeffs(const SvfParams& p) noexcept {
    SvfCoeffs c;
    c.a1 = 1.0 / (1.0 + p.g * (p.g + p.k));
    c.a2 = p.g * c.a1;
    c.a3 = p.g * c.a2;
    c.m0 = p.m0;
    c.m1 = p.m1;
    c.m2 = p.m2;
    return c;
}

// Removes a pole/zero pair cancelling at z = -1 (bilinear writes first-order sections this
// way). Returns the reduced biquad; unchanged if there is no such pair.
[[nodiscard]] inline Biquad reduceNyquistPair(const Biquad& b) noexcept {
    const double d2 = 1.0 - b.a1 + b.a2;
    const double n2 = b.b0 - b.b1 + b.b2;
    const double scale = std::abs(b.b0) + std::abs(b.b1) + std::abs(b.b2) + 1e-300;
    if (std::abs(d2) > 1e-12 || std::abs(n2) > 1e-9 * scale) return b;
    // (1 + z^-1)(1 + p z^-1) => a1 = 1 + p, a2 = p;  (1 + z^-1)(b0 + q z^-1) => b2 = q.
    Biquad r;
    r.b0 = b.b0;
    r.b1 = b.b2;
    r.b2 = 0.0;
    r.a1 = b.a2;
    r.a2 = 0.0;
    return r;
}

// Returns false (out unchanged) if the biquad has no strictly stable SVF form: a pole on or
// outside the unit circle, or at z = -1 without a matching zero.
[[nodiscard]] inline bool svfFromBiquad(const Biquad& in, SvfParams& out) noexcept {
    const Biquad b = reduceNyquistPair(in);
    const double d2 = 1.0 - b.a1 + b.a2;  // sigma^2 coefficient of the denominator
    const double d1 = 2.0 - 2.0 * b.a2;   // sigma^1
    const double d0 = 1.0 + b.a1 + b.a2;  // sigma^0
    if (!(d2 > 0.0) || !(d1 > 0.0) || !(d0 > 0.0)) return false;
    const double c2 = (b.b0 - b.b1 + b.b2) / d2;
    const double c1 = (2.0 * b.b0 - 2.0 * b.b2) / d2;
    const double c0 = (b.b0 + b.b1 + b.b2) / d2;
    SvfParams p;
    p.g = std::sqrt(d0 / d2);
    p.k = (d1 / d2) / p.g;
    p.m0 = c2;
    p.m1 = (c1 - c2 * p.k * p.g) / p.g;
    p.m2 = c0 / (p.g * p.g) - c2;
    if (!std::isfinite(p.g) || !std::isfinite(p.k) || !std::isfinite(p.m0) || !std::isfinite(p.m1) ||
        !std::isfinite(p.m2) || !(p.g > 0.0) || !(p.k > 0.0))
        return false;
    out = p;
    return true;
}

// Exact transfer function of one SVF section at z (area 04 R10): with sigma = (z-1)/(z+1),
// H = m0 + m1 * g*sigma / D + m2 * g^2 / D, D = sigma^2 + k*g*sigma + g^2 (see the header note).
[[nodiscard]] inline std::complex<double> svfResponse(const SvfParams& p, std::complex<double> z) noexcept {
    const std::complex<double> sigma = (z - 1.0) / (z + 1.0);
    const std::complex<double> d = sigma * sigma + p.k * p.g * sigma + p.g * p.g;
    return p.m0 + (p.m1 * p.g * sigma + p.m2 * p.g * p.g) / d;
}

[[nodiscard]] inline double svfTick(const SvfCoeffs& c, SvfState& s, double v0) noexcept {
    const double v3 = v0 - s.ic2;
    const double v1 = c.a1 * s.ic1 + c.a2 * v3;
    const double v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
    s.ic1 = 2.0 * v1 - s.ic1;
    s.ic2 = 2.0 * v2 - s.ic2;
    return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
}

// Envelope of a band-pass section (helper band-pass: m0 = m2 = 0, m1 = k). The SVF's band output
// v1 and low output v2 are in quadrature (v2 / v1 = g / sigma, purely imaginary on the unit
// circle) and equal in size at the centre frequency, so k * sqrt(v1^2 + v2^2) is the amplitude of
// a sine at the centre with no ripple, and nearly so close to it: a detector with no lag.
[[nodiscard]] inline double svfBandEnvelope(const SvfCoeffs& c, SvfState& s, double v0) noexcept {
    const double v3 = v0 - s.ic2;
    const double v1 = c.a1 * s.ic1 + c.a2 * v3;
    const double v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
    s.ic1 = 2.0 * v1 - s.ic1;
    s.ic2 = 2.0 * v2 - s.ic2;
    return c.m1 * std::sqrt(v1 * v1 + v2 * v2);
}

// svfBandEnvelope on two channels with the same coefficients (linked stereo side chain), one block:
// l and r hold the inputs and receive the envelopes. SSE2 keeps the scalar operation order, and its
// square root is correctly rounded like std::sqrt, so the results are bit-identical.
inline void svfBandEnvelopeStereo(const SvfCoeffs& c, SvfState& sl, SvfState& sr, double* l, double* r,
                                  uint32_t n) noexcept {
#if defined(AMQ_SVF_SSE2)
    const __m128d a1 = _mm_set1_pd(c.a1), a2 = _mm_set1_pd(c.a2), a3 = _mm_set1_pd(c.a3);
    const __m128d m1 = _mm_set1_pd(c.m1), two = _mm_set1_pd(2.0);
    __m128d ic1 = _mm_set_pd(sr.ic1, sl.ic1), ic2 = _mm_set_pd(sr.ic2, sl.ic2);
    for (uint32_t i = 0; i < n; ++i) {
        const __m128d v0 = _mm_set_pd(r[i], l[i]);
        const __m128d v3 = _mm_sub_pd(v0, ic2);
        const __m128d v1 = _mm_add_pd(_mm_mul_pd(a1, ic1), _mm_mul_pd(a2, v3));
        const __m128d v2 = _mm_add_pd(_mm_add_pd(ic2, _mm_mul_pd(a2, ic1)), _mm_mul_pd(a3, v3));
        ic1 = _mm_sub_pd(_mm_mul_pd(two, v1), ic1);
        ic2 = _mm_sub_pd(_mm_mul_pd(two, v2), ic2);
        const __m128d y = _mm_mul_pd(m1, _mm_sqrt_pd(_mm_add_pd(_mm_mul_pd(v1, v1), _mm_mul_pd(v2, v2))));
        _mm_storel_pd(l + i, y);
        _mm_storeh_pd(r + i, y);
    }
    double t[2];
    _mm_storeu_pd(t, ic1);
    sl.ic1 = t[0], sr.ic1 = t[1];
    _mm_storeu_pd(t, ic2);
    sl.ic2 = t[0], sr.ic2 = t[1];
#else
    for (uint32_t i = 0; i < n; ++i) l[i] = svfBandEnvelope(c, sl, l[i]), r[i] = svfBandEnvelope(c, sr, r[i]);
#endif
}

// Block runs of one section in place (R18 kernel variants). Results are bit-identical to calling
// svfTick per sample: a section's recurrence does not depend on the order sections are run in, and
// the SIMD form keeps the scalar operation order.
inline void svfRun(const SvfCoeffs& c, SvfState& s, double* x, uint32_t n) noexcept {
    for (uint32_t i = 0; i < n; ++i) x[i] = svfTick(c, s, x[i]);
}

// One sample of linked stereo (same coefficients on both channels), bit-identical to two svfTick.
inline void svfTickStereo(const SvfCoeffs& c, SvfState& sl, SvfState& sr, double& l, double& r) noexcept {
#if defined(AMQ_SVF_SSE2)
    const __m128d v0 = _mm_set_pd(r, l);
    const __m128d ic1 = _mm_set_pd(sr.ic1, sl.ic1), ic2 = _mm_set_pd(sr.ic2, sl.ic2);
    const __m128d a2 = _mm_set1_pd(c.a2);
    const __m128d v3 = _mm_sub_pd(v0, ic2);
    const __m128d v1 = _mm_add_pd(_mm_mul_pd(_mm_set1_pd(c.a1), ic1), _mm_mul_pd(a2, v3));
    const __m128d v2 = _mm_add_pd(_mm_add_pd(ic2, _mm_mul_pd(a2, ic1)), _mm_mul_pd(_mm_set1_pd(c.a3), v3));
    const __m128d two = _mm_set1_pd(2.0);
    const __m128d n1 = _mm_sub_pd(_mm_mul_pd(two, v1), ic1), n2 = _mm_sub_pd(_mm_mul_pd(two, v2), ic2);
    const __m128d y = _mm_add_pd(_mm_add_pd(_mm_mul_pd(_mm_set1_pd(c.m0), v0), _mm_mul_pd(_mm_set1_pd(c.m1), v1)),
                                 _mm_mul_pd(_mm_set1_pd(c.m2), v2));
    double t[2];
    _mm_storeu_pd(t, n1);
    sl.ic1 = t[0], sr.ic1 = t[1];
    _mm_storeu_pd(t, n2);
    sl.ic2 = t[0], sr.ic2 = t[1];
    _mm_storeu_pd(t, y);
    l = t[0], r = t[1];
#else
    l = svfTick(c, sl, l);
    r = svfTick(c, sr, r);
#endif
}

// Linked stereo: both channels share the coefficients; SSE2 runs them in one register.
inline void svfRunStereo(const SvfCoeffs& c, SvfState& sl, SvfState& sr, double* l, double* r, uint32_t n) noexcept {
#if defined(AMQ_SVF_SSE2)
    const __m128d a1 = _mm_set1_pd(c.a1), a2 = _mm_set1_pd(c.a2), a3 = _mm_set1_pd(c.a3);
    const __m128d m0 = _mm_set1_pd(c.m0), m1 = _mm_set1_pd(c.m1), m2 = _mm_set1_pd(c.m2);
    const __m128d two = _mm_set1_pd(2.0);
    __m128d ic1 = _mm_set_pd(sr.ic1, sl.ic1), ic2 = _mm_set_pd(sr.ic2, sl.ic2);
    for (uint32_t i = 0; i < n; ++i) {
        const __m128d v0 = _mm_set_pd(r[i], l[i]);
        const __m128d v3 = _mm_sub_pd(v0, ic2);
        const __m128d v1 = _mm_add_pd(_mm_mul_pd(a1, ic1), _mm_mul_pd(a2, v3));
        const __m128d v2 = _mm_add_pd(_mm_add_pd(ic2, _mm_mul_pd(a2, ic1)), _mm_mul_pd(a3, v3));
        ic1 = _mm_sub_pd(_mm_mul_pd(two, v1), ic1);
        ic2 = _mm_sub_pd(_mm_mul_pd(two, v2), ic2);
        const __m128d y = _mm_add_pd(_mm_add_pd(_mm_mul_pd(m0, v0), _mm_mul_pd(m1, v1)), _mm_mul_pd(m2, v2));
        _mm_storel_pd(l + i, y);
        _mm_storeh_pd(r + i, y);
    }
    double t[2];
    _mm_storeu_pd(t, ic1);
    sl.ic1 = t[0], sr.ic1 = t[1];
    _mm_storeu_pd(t, ic2);
    sl.ic2 = t[0], sr.ic2 = t[1];
#else
    svfRun(c, sl, l, n);
    svfRun(c, sr, r, n);
#endif
}

}  // namespace amq::dsp
