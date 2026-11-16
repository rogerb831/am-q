#pragma once
// Solo audition filters (area 02 R14, R15). Soloing a band replaces the output with the band's
// region of the (latency-matched) input:
//
//   Bell, Notch, Band Pass, All Pass  band-pass at the band frequency, width from Q
//   Low Shelf, Low Cut                low-pass at the band frequency (what the band acts on below)
//   High Shelf, High Cut              high-pass at the band frequency
//   Tilt Shelf, Flat Tilt             no filter: the tilt acts on the whole spectrum
//
// All are one second-order section, unity gain in the passband (the band-pass peaks at exactly
// 0 dB at the band frequency). They are internal helper filters, so R15 asks for a bilinear
// design with a bandwidth pre-warp, done here exactly in the SVF form (svf.hpp):
//
//   - The centre is pre-warped: g = tan(pi f0 / fs), so the digital peak sits exactly at f0.
//   - The bandwidth is pre-warped: the -3 dB edges of the digital band-pass are f1 and f2 with
//     tan(pi f1/fs) * tan(pi f2/fs) = g^2 and k = (tan(pi f2/fs) - tan(pi f1/fs)) / g. We pick
//     f1, f2 so that f2 / f1 = 2^N, where N is the octave bandwidth of an analog second-order
//     band-pass with the audition Q: N = (2 / ln 2) asinh(1 / (2 Q)). So the audition keeps
//     the analog width in octaves at every frequency, up to near Nyquist.
//
// The width law (audition Q from the band's Q) is PROVISIONAL, to be tuned by listening
// (area 02 task); the constants below are our own starting points, not copied values.

#include "amq/dsp/analog_prototype.hpp"
#include "amq/dsp/svf.hpp"

#include <algorithm>
#include <cmath>

namespace amq::dsp {

enum class AuditionKind : uint8_t { BandPass, LowPass, HighPass, Pass };

inline constexpr double kAuditionWidth = 1.0;  // PROVISIONAL: audition Q = band Q * this
inline constexpr double kAuditionQMin = 0.3;   // PROVISIONAL: widest audition (about 3.2 octaves)
inline constexpr double kAuditionQMax = 8.0;   // PROVISIONAL: narrowest; keeps tight notches audible
inline constexpr double kAuditionCutQ = 0.7071067811865476;  // low-pass / high-pass: Butterworth

[[nodiscard]] inline AuditionKind auditionKind(Shape s) noexcept {
    switch (s) {
        case Shape::LowShelf:
        case Shape::LowCut: return AuditionKind::LowPass;
        case Shape::HighShelf:
        case Shape::HighCut: return AuditionKind::HighPass;
        case Shape::TiltShelf:
        case Shape::FlatTilt: return AuditionKind::Pass;
        default: return AuditionKind::BandPass;
    }
}

// Shapes whose boost is part of the audition level (R14: a boosted Bell includes its boost).
[[nodiscard]] inline bool auditionIncludesBoost(Shape s) noexcept {
    return s == Shape::Bell || s == Shape::LowShelf || s == Shape::HighShelf;
}

[[nodiscard]] inline double auditionQ(double bandQ) noexcept {
    return std::clamp(bandQ * kAuditionWidth, kAuditionQMin, kAuditionQMax);
}

// Octave bandwidth of an analog second-order band-pass with quality factor q.
[[nodiscard]] inline double bandPassOctaves(double q) noexcept {
    return 2.0 / std::log(2.0) * std::asinh(1.0 / (2.0 * q));
}

// The SVF section of an audition filter. hz is clamped into (0, 0.49 fs).
[[nodiscard]] inline SvfParams auditionDesign(AuditionKind kind, double hz, double q, double fs) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    hz = std::clamp(hz, 1.0, 0.49 * fs);
    SvfParams p;
    p.g = std::tan(kPi * hz / fs);
    switch (kind) {
        case AuditionKind::Pass: p = SvfParams{}; p.m0 = 1.0, p.m1 = 0.0, p.m2 = 0.0; return p;
        case AuditionKind::LowPass: p.k = 1.0 / kAuditionCutQ, p.m0 = 0.0, p.m1 = 0.0, p.m2 = 1.0; return p;
        case AuditionKind::HighPass: p.k = 1.0 / kAuditionCutQ, p.m0 = 1.0, p.m1 = -p.k, p.m2 = -1.0; return p;
        case AuditionKind::BandPass: break;
    }
    // Bandwidth pre-warp: solve tan(pi f1/fs) * tan(pi r f1/fs) = g^2 for f1 (increasing in f1).
    const double r = std::exp2(bandPassOctaves(q));
    const double g2 = p.g * p.g;
    const double top = std::min(hz, 0.4999 * fs / r);  // keep f2 = r f1 below Nyquist
    auto h = [&](double f1) { return std::tan(kPi * f1 / fs) * std::tan(kPi * r * f1 / fs) - g2; };
    if (h(top) >= 0.0) {
        double lo = 0.0, hi = top;
        for (int i = 0; i < 60; ++i) {
            const double mid = 0.5 * (lo + hi);
            (h(mid) < 0.0 ? lo : hi) = mid;
        }
        const double f1 = 0.5 * (lo + hi);
        p.k = (std::tan(kPi * r * f1 / fs) - std::tan(kPi * f1 / fs)) / p.g;
    } else {
        p.k = 1.0 / q;  // the upper edge would pass Nyquist: plain bilinear width
    }
    // Band output v1 peaks at 1/k at the centre: m1 = k gives a 0 dB peak.
    p.m0 = 0.0, p.m1 = p.k, p.m2 = 0.0;
    return p;
}

}  // namespace amq::dsp
