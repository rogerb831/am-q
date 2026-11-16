#pragma once
// EQ Match difference curve (area 20 R1-R10).
//
//   D(f) = reference dB - input dB per match-grid point, from the two captures' mean power (both use
//     the same window, FFT size and grid, so scaling constants cancel). Never tilted (R10).
//   Level alignment (R3, on by default): the BS.1770 loudness difference of the captures is subtracted;
//     the offset is reported either way.
//   Smoothing (R4): fractional-octave, 1/3, 1/6 (default), 1/12 or 1/24 octave, a plain mean of D in
//     dB over a window a fixed fraction of an octave wide, centred; at the edges the window shrinks
//     symmetrically instead of padding.
//   Confidence (R5): a point within 10 dB of either capture's floor (its loudest point minus 60 dB,
//     but never below -110 dBFS, PROVISIONAL) gets a weight ramping down to 0 over 10 dB; points above
//     the usable limit (the lower of 20 kHz and 0.45 fs) get 0 (area 16 R3).
//   Clamp (R6): +-36 dB, a little wider than the fit's gain range.
// The curve the fitter gets is the curve shown (R8): one struct, published as one snapshot.

#include "amq/analysis/match_capture.hpp"

#include <array>

namespace amq::analysis {

inline constexpr double kMatchClampDb = 36.0;
inline constexpr double kMatchFloorRangeDb = 60.0;
inline constexpr double kMatchAbsoluteFloorDb = -110.0;
inline constexpr double kMatchConfidenceRampDb = 10.0;

enum class MatchSmoothing : uint8_t { Third, Sixth, Twelfth, TwentyFourth };

struct MatchCurveSettings {
    bool levelAlign = true;
    MatchSmoothing smoothing = MatchSmoothing::Sixth;
};

struct MatchCurve {
    bool valid = false;
    double sampleRate = 48000.0;
    double levelOffsetDb = 0.0;  // reference loudness - input loudness (LU)
    bool aligned = true;
    std::array<double, kMatchGridPoints> inputDb{}, referenceDb{};
    std::array<double, kMatchGridPoints> target{};  // smoothed, aligned, clamped: what the fitter fits
    std::array<double, kMatchGridPoints> weight{};  // fit weight, 0 .. 1
};

[[nodiscard]] double smoothingOctaves(MatchSmoothing s) noexcept;

// Builds the curve from two captures (worker). False (curve.valid false) until both are ready.
bool computeMatchCurve(const MatchCapture& input, const MatchCapture& reference, const MatchCurveSettings& s,
                       MatchCurve& out) noexcept;
// The pieces, for tests and offline use.
void smoothFractionalOctave(const std::array<double, kMatchGridPoints>& in, double octaves,
                            std::array<double, kMatchGridPoints>& out) noexcept;

}  // namespace amq::analysis
