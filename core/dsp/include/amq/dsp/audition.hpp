#pragma once
// Solo audition filters (area 02 R14, R15). Soloing a band replaces the output with the band's
// region of the (latency-matched) input:
//
//   Bell, Notch, Band Pass, All Pass  band-pass at the band frequency, width from Q
//   Low Shelf, Low Cut                low-pass at the band frequency (what the band acts on below)
//   High Shelf, High Cut              high-pass at the band frequency
//   Tilt Shelf, Flat Tilt             no filter: the tilt acts on the whole spectrum
//
// They are internal helper filters, so they use the BilinearBandwidth design mode
// (helper_filters.hpp): one SVF section, unity gain in the passband, the band-pass peaking at
// exactly 0 dB at the band frequency with the analog octave width.
//
// The width law (audition Q from the band's Q) is PROVISIONAL, to be tuned by listening
// (area 02 task); the constants below are our own starting points, not copied values.

#include "amq/dsp/helper_filters.hpp"

#include <algorithm>

namespace amq::dsp {

enum class AuditionKind : uint8_t { BandPass, LowPass, HighPass, Pass };

inline constexpr double kAuditionWidth = 1.0;  // PROVISIONAL: audition Q = band Q * this
inline constexpr double kAuditionQMin = 0.3;   // PROVISIONAL: widest audition (about 3.2 octaves)
inline constexpr double kAuditionQMax = 8.0;   // PROVISIONAL: narrowest; keeps tight notches audible

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

// The SVF section of an audition filter. hz is clamped into (0, 0.49 fs).
[[nodiscard]] inline SvfParams auditionDesign(AuditionKind kind, double hz, double q, double fs) noexcept {
    switch (kind) {
        case AuditionKind::Pass: return SvfParams{1.0, 2.0, 1.0, 0.0, 0.0};
        case AuditionKind::LowPass: return helperLowPass(hz, fs);
        case AuditionKind::HighPass: return helperHighPass(hz, fs);
        case AuditionKind::BandPass: break;
    }
    return helperBandPass(hz, q, fs);
}

}  // namespace amq::dsp
