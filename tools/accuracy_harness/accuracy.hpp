#pragma once
// Accuracy harness (plan area 01 acceptance test "Filter magnitude accuracy"; R12).
// Sweeps shapes x slopes x frequencies x gains x Q x sample rates, designs each band
// digitally and compares the digital magnitude with the analog prototype on a dense
// log grid up to limitFraction * Nyquist.

#include "amq/dsp/analog_prototype.hpp"
#include "amq/dsp/digital_design.hpp"

#include <string>
#include <utility>
#include <vector>

namespace amq::accuracy {

struct SweepConfig {
    std::vector<dsp::Shape> shapes;
    std::vector<dsp::Slope> slopes;
    std::vector<double> frequenciesHz;
    std::vector<double> gainsDb;   // used for shapes with gain; others use 0 dB only
    std::vector<double> qs;        // used for shapes that use Q; others use Q = 1 only
    std::vector<double> sampleRates;
    dsp::DesignMethod method = dsp::DesignMethod::MagnitudeMatched;

    double limitFraction = 0.9;  // compare below limitFraction * Nyquist (R12)
    double targetDb = 0.1;       // pass threshold (R12)
    double floorDb = -60.0;      // both magnitudes are floored here before comparing
    double audibleFloorDb = -40.0;  // secondary metric: only levels above this count
    int gridPoints = 384;        // log-spaced points from 10 Hz to the limit
    int threads = 0;             // 0 = hardware concurrency
    // MatchedRefined only: 0 = default (correction section only when R12 is missed),
    // 1 = order N only (never add a section), 2 = always try one correction section.
    int extraMode = 0;
    double tierDb = -36.0;       // level tier boundary for the split metric below
};

// The full Phase 0 grid. 44.1, 48, 96 and 192 kHz.
[[nodiscard]] SweepConfig defaultFullSweep();

struct CaseResult {
    dsp::BandSpec spec;
    double sampleRate = 0.0;
    double maxErrorDb = 0.0;  // max |dB(digital) - dB(analog)| below the limit
    double worstHz = 0.0;     // where it happened
    double maxErrorAudibleDb = 0.0;  // same, with both magnitudes floored at audibleFloorDb
    double maxErrorUpperTierDb = 0.0;  // only points where the analog level >= tierDb
    double maxErrorLowerTierDb = 0.0;  // points with analog level in [floorDb, tierDb) (floored)
    double worstAnalogDb = 0.0;        // analog level at worstHz
    double r12TolDb = 0.1;             // revised R12 tolerance for this band (NaN = TBD)
    int protoSections = 0, digitalSections = 0;  // digitalSections > protoSections: correction used
    double allPassPhaseErrorDeg = -1.0;  // All Pass only, at the centre (if in band)
    int clampedSections = 0;
    bool evaluated = false;   // false when the centre/grid leaves nothing to compare
};

// Evaluates one band at one sample rate.
[[nodiscard]] CaseResult evaluateCase(const dsp::BandSpec& spec, double sampleRate,
                                      const SweepConfig& cfg);

struct SweepResult {
    SweepConfig config;
    std::vector<CaseResult> cases;
    double seconds = 0.0;
};

[[nodiscard]] SweepResult runSweep(const SweepConfig& cfg);

// Markdown report: summary per shape, per sample rate, per region (centre vs Nyquist,
// Q band), the worst cases, and All Pass phase.
struct NamedResult {
    std::string name;
    const SweepResult* result;
};
// `primary` gets the full breakdown; `comparisons` get a per-shape summary each.
[[nodiscard]] std::string formatReport(const SweepResult& primary, const std::vector<NamedResult>& comparisons);

// CSV of every case (one row per case).
[[nodiscard]] std::string formatCsv(const SweepResult& result);

}  // namespace amq::accuracy
