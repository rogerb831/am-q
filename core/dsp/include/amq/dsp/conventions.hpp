#pragma once
// Am-Q shared conventions (plan area 01, "Conventions"; R13, R20).
// One header defines dB <-> linear, the 90 %-step time definition and the
// log2-Hz frequency encoding. Every module uses these instead of its own copies.

#include <cmath>
#include <numbers>

namespace amq::dsp {

inline constexpr double kPi = std::numbers::pi;
inline constexpr double kTwoPi = 2.0 * std::numbers::pi;

// ---------------------------------------------------------------------------
// Gain law (R13): dB converts to linear as 10^(dB/20) everywhere.
// ---------------------------------------------------------------------------
inline constexpr double kMaxBandGainDb = 30.0;  // effective band gain clamp, R13

[[nodiscard]] inline double dbToLinear(double db) noexcept { return std::pow(10.0, db / 20.0); }

// Returns -inf for 0. Callers that need a floor use linearToDbFloored.
[[nodiscard]] inline double linearToDb(double linear) noexcept { return 20.0 * std::log10(linear); }

[[nodiscard]] inline double linearToDbFloored(double linear, double floorDb) noexcept {
    const double floorLin = dbToLinear(floorDb);
    return linearToDb(linear > floorLin ? linear : floorLin);
}

[[nodiscard]] inline double clampBandGainDb(double db) noexcept {
    return db > kMaxBandGainDb ? kMaxBandGainDb : (db < -kMaxBandGainDb ? -kMaxBandGainDb : db);
}

// ---------------------------------------------------------------------------
// Time definition (R20): every attack, release and smoothing time is the time to
// cover 90 % of a step.
//
// For a one-pole smoother y[n] = y[n-1] + c * (x - y[n-1]) the remaining error after
// N samples is (1 - c)^N. Setting it to 0.1 at N = t * fs gives
//     c = 1 - 0.1^(1 / (t * fs)) = -expm1(ln(0.1) / (t * fs)).
// ---------------------------------------------------------------------------
inline constexpr double kStepFraction = 0.9;  // fraction of a step covered in "time"

[[nodiscard]] inline double onePoleCoefficientFor90PercentTime(double seconds, double sampleRate) noexcept {
    const double samples = seconds * sampleRate;
    if (!(samples > 0.0)) return 1.0;  // zero time: jump immediately
    return -std::expm1(std::log(1.0 - kStepFraction) / samples);
}

// Inverse: the 90 % time (seconds) of a one-pole coefficient.
[[nodiscard]] inline double ninetyPercentTimeOfOnePole(double coefficient, double sampleRate) noexcept {
    return std::log(1.0 - kStepFraction) / std::log1p(-coefficient) / sampleRate;
}

// For a linear ramp, the 90 % time is 0.9 of the total ramp length.
[[nodiscard]] inline double linearRampLengthFor90PercentTime(double seconds) noexcept {
    return seconds / kStepFraction;
}

// ---------------------------------------------------------------------------
// Frequency encoding: frequencies are stored as log2(Hz) (plan "Encodings").
// ---------------------------------------------------------------------------
inline constexpr double kMinBandFrequencyHz = 10.0;     // R8
inline constexpr double kMaxBandFrequencyHz = 30000.0;  // R8
inline constexpr double kDefaultBandFrequencyHz = 1000.0;

[[nodiscard]] inline double hzToLog2(double hz) noexcept { return std::log2(hz); }
[[nodiscard]] inline double log2ToHz(double log2Hz) noexcept { return std::exp2(log2Hz); }

// Host 0..1 mapping is linear in log2 Hz between the R8 limits.
[[nodiscard]] inline double frequencyToNormalised(double hz) noexcept {
    const double lo = std::log2(kMinBandFrequencyHz), hi = std::log2(kMaxBandFrequencyHz);
    return (std::log2(hz) - lo) / (hi - lo);
}
[[nodiscard]] inline double normalisedToFrequency(double n) noexcept {
    const double lo = std::log2(kMinBandFrequencyHz), hi = std::log2(kMaxBandFrequencyHz);
    return std::exp2(lo + n * (hi - lo));
}

// Digital angular frequency in radians per sample.
[[nodiscard]] inline double hzToRadiansPerSample(double hz, double sampleRate) noexcept {
    return kTwoPi * hz / sampleRate;
}
[[nodiscard]] inline double log2HzToRadiansPerSample(double log2Hz, double sampleRate) noexcept {
    return hzToRadiansPerSample(log2ToHz(log2Hz), sampleRate);
}

}  // namespace amq::dsp
