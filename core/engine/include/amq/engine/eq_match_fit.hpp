#pragma once
// EQ Match fitter (area 16 R2-R10, R16-R18): greedy matching pursuit (forward stagewise fitting)
// with least squares in dB, on the match grid (analysis/match_capture.hpp).
//
// Each round evaluates every candidate family, with no entry thresholds (R5, R6):
//   - Low Cut and High Cut at every slope Am-Q offers (Brickwall included), starting where the
//     residual's edge deficit begins;
//   - Low Shelf and High Shelf, starting at the mean residual over the outer octave;
//   - a Bell at each of the K largest residual peaks (K = 12, PROVISIONAL), scored by residual area
//     over +-1/3 octave so broad deviations beat narrow spikes.
// Each candidate is refined (R7) by coordinate descent: kSweeps sweeps of golden-section line
// searches over log frequency, then gain (gain shapes), then log Q, every value clamped to Am-Q's
// ranges (R9: 10 Hz - 30 kHz, +-30 dB, Q 0.025 - 40; shelves and cuts non-resonant, Q 0.3 - 1.0,
// PROVISIONAL). The search uses the analog prototypes (fast); the candidate kept is scored, and its
// response subtracted from the residual, with the exact digital response of the design the audio
// path runs (R4: designBandRealtime). The round keeps the candidate that reduces the weighted mean
// squared dB error most (R3). Fitting stops at 24 bands, when no candidate reduces the error, or when
// the relative improvement falls below the tolerance (R8). Bands come back sorted by the error they
// removed when accepted, strongest first (R10; responses add in dB, so the order is free). Deterministic (R17), finite and in range for any input (R18).
// Progress and cancel (R16) are checked between candidates.

#include "amq/analysis/match_capture.hpp"
#include "amq/dsp/analog_prototype.hpp"

#include <array>
#include <atomic>
#include <vector>

namespace amq::engine {

using MatchArray = std::array<double, analysis::kMatchGridPoints>;

struct FitSettings {
    double sampleRate = 48000.0;
    int maxBands = 24;
    double tolerance = 1e-3;  // PROVISIONAL: stop when a round improves the error by less than this fraction
    int peaks = 12;           // PROVISIONAL
    int sweeps = 3;           // PROVISIONAL
};

struct FittedBand {
    dsp::Shape shape = dsp::Shape::Bell;
    dsp::Slope slope = dsp::Slope::Db12;
    double hz = 1000.0, gainDb = 0.0, q = 1.0;
    double errorReduction = 0.0;  // weighted mean squared dB error removed when it was accepted
};

struct FitResult {
    std::vector<FittedBand> bands;
    double initialError = 0.0, finalError = 0.0;
    bool cancelled = false;
};

[[nodiscard]] FitResult fitMatch(const MatchArray& target, const MatchArray& weight, const FitSettings& s,
                                 std::atomic<float>* progress = nullptr, const std::atomic<bool>* cancel = nullptr);

// The exact digital response of a band in dB on the match grid (the design the audio path runs).
void bandResponseDb(const FittedBand& b, double sampleRate, MatchArray& out);
// Weighted mean squared dB error.
[[nodiscard]] double weightedError(const MatchArray& residual, const MatchArray& weight) noexcept;

}  // namespace amq::engine
