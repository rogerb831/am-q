#pragma once
// Auto Gain weighting and make-up gain (area 03 R7-R9; area 05 R9-R13).
//
// Weighting (R10; counsel review marked complete 2026-11-18; PROVISIONAL until the programme-corpus tuning):
// on the shared 512-point log grid (log_grid.hpp), 20 Hz to 20 kHz, each point stands for an equal
// slice of log frequency, which carries equal power for a pink programme spectrum. Its weight is
// that pink share times the K-weighting power |K(f)|^2 (loudness.hpp: its RLB high-pass ignores the
// sub-bass), normalised to sum to 1; the grid's 20 kHz end ignores the extreme top. Measured
// (tests/engine/test_auto_gain.cpp): over 50 random EQ curves on pink noise, BS.1770 loudness after
// Auto Gain moves by at most 0.97 LU. An extra roll-off from 16 kHz was tried and dropped: it put
// pink noise 2.3 LU off on curves that shape the top octave.
//
// Make-up gain (R9): per channel, the weighted mean power of the combined band response,
// m = sum_i w_i |H(f_i)|^2, gives gain_dB = -10 log10(m). Channels combine as a weighted geometric
// mean, i.e. a weighted mean in dB, with the speaker loudness weights (speakers.hpp; LFE 0). Clamped
// to +-30 dB. All bands flat gives exactly 0 dB.
//
// The table is built once (autoGainWeights()); the engine touches it at construction so it never
// builds on the audio thread (R13), and the GUI reads the same table.

#include "amq/dsp/log_grid.hpp"
#include "amq/dsp/loudness.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace amq::dsp {

inline constexpr double kAutoGainClampDb = 30.0;

[[nodiscard]] inline const std::array<double, kLogGridPoints>& autoGainWeights() noexcept {
    static const std::array<double, kLogGridPoints> w = [] {
        std::array<double, kLogGridPoints> t{};
        double sum = 0.0;
        for (int i = 0; i < kLogGridPoints; ++i) {
            t[static_cast<size_t>(i)] = kWeightingPower(logGrid()[static_cast<size_t>(i)]);
            sum += t[static_cast<size_t>(i)];
        }
        for (auto& v : t) v /= sum;
        return t;
    }();
    return w;
}

// Make-up gain in dB for one channel's response power |H(f_i)|^2 on the log grid.
[[nodiscard]] inline double autoGainChannelDb(const double* powerOnGrid) noexcept {
    const auto& w = autoGainWeights();
    double m = 0.0;
    for (int i = 0; i < kLogGridPoints; ++i) m += w[static_cast<size_t>(i)] * powerOnGrid[i];
    if (!(m > 0.0)) return kAutoGainClampDb;
    return std::clamp(-10.0 * std::log10(m), -kAutoGainClampDb, kAutoGainClampDb);
}

}  // namespace amq::dsp
