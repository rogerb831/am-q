#pragma once
// Am-Q core processor entry (area 01 R3-R5/R7/R15/R18, area 02 R1/R3/R5/R6, area 03 R10-R12).
//
// Framework-free. The plugin wrapper splits each host block at every parameter-event offset,
// writes the events into the ParameterTable, and calls process() once per piece.
//
// Per piece:
//   1. Ramps are retargeted from the table. A band whose targets changed is read at once, so
//      a change at offset k starts at sample k (area 01 R3).
//   2. Band parameters are otherwise read every 4 samples on an absolute-time grid, and each
//      band is redesigned at most every 32 samples, or at once on a large frequency jump or a
//      shape / slope change (R15). The grid is absolute, so output does not depend on how the
//      host cuts its blocks.
//   3. The 24 bands run in series as TPT SVF cascades with double state (R18): the L/R stage
//      (Stereo, Left, Right) in index order, then, while any Mid or Side band is audible,
//      encode, the M/S stage (Mid, Side) in index order, and decode (area 03 R1-R4). A band
//      runs on a channel only when used, enabled and its placement matches; turning it on or
//      off crossfades over 10 ms (area 02 R6). A band that is fully off is skipped, so it is
//      bit-transparent.
//   4. The output Level / Pan / Invert node (area 03 R10-R12).
//
// Design tiers: bands are designed on the audio thread with the fast three-point design while
// they move; once settled, the refined design (R12) comes from DesignWorker and the
// coefficients glide to it (refineStep, startGlide).
//
// Not yet (later tasks): dynamics,
// spectral bands, the 100 ms section-count crossfade (R17), near-zero bypass (R16), Auto Gain,
// bypass, and SIMD.
//
// Threading: prepare() and release() on the main thread while not processing; process(),
// snapToTable() and the read accessors on the audio thread. process() never allocates, locks
// or blocks (area 01 R19).

#include "amq/analysis/analyzer.hpp"
#include "amq/analysis/display_spectrum.hpp"
#include "amq/analysis/publisher.hpp"
#include "amq/dsp/latency.hpp"
#include "amq/dsp/mid_side.hpp"
#include "amq/dsp/realtime_design.hpp"
#include "amq/engine/design_worker.hpp"
#include "amq/engine/ramp.hpp"
#include "amq/params/parameter_table.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace amq::engine {

inline constexpr double kMinSampleRate = 44100.0;
inline constexpr double kMaxSampleRate = 384000.0;
inline constexpr uint32_t kMaxChannels = 2;
inline constexpr uint32_t kReadInterval = 4;       // samples between parameter reads (R15)
inline constexpr uint32_t kRedesignInterval = 32;  // minimum samples between redesigns (R15)
inline constexpr double kJumpOctaves = 0.5;        // PROVISIONAL: redesign at once beyond this
inline constexpr double kEnableFadeMs = 10.0;      // area 02 R6 initial value
inline constexpr double kRefineSwapDelayMs = 50.0; // PROVISIONAL: swap a refined design this long after the request
inline constexpr double kRefineGlideMs = 20.0;     // PROVISIONAL: glide from the fast to the refined design

// Where refined designs are computed (area 01 "Design tiers").
enum class DesignMode : uint8_t {
    Manual,  // requests wait for runDesignJobsForTests() (deterministic tests); the default
    Thread,  // a worker thread designs them (the plugin's real-time mode)
};

// Band fields that have audio-side ramps, in this order.
inline constexpr std::array<params::BandField, 5> kRampedBandFields = {
    params::BandField::Freq, params::BandField::Gain, params::BandField::Q, params::BandField::DynRange,
    params::BandField::Threshold};

struct EngineStats {
    uint64_t designs = 0;            // band redesigns since prepare
    uint64_t bilinearFallbacks = 0;  // designs that needed the stability fallback
    uint64_t unsupported = 0;        // designs that left the band bypassed
    uint64_t refineRequests = 0;     // refined designs requested from the worker
    uint64_t refinedSwaps = 0;       // refined designs swapped in
    uint64_t refineFailures = 0;     // refined designs that could not be used
};

class Engine {
public:
    explicit Engine(const params::ParameterTable& table) noexcept;
    ~Engine() {
        analyzer_.stop();
        worker_.stop();
    }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Main thread, before prepare(). Thread mode starts the design worker and the analyzer
    // worker in prepare() and stops them in release(); Manual mode leaves both to the tests.
    void setDesignMode(DesignMode m) noexcept { mode_ = m; }
    // Offline rendering (CLAP render mode): refined designs run at once on the audio thread,
    // which may then allocate and take milliseconds, so renders are exact and repeatable.
    // Any thread.
    void setOfflineRender(bool offline) noexcept { offline_.store(offline, std::memory_order_relaxed); }
    // Manual mode: run outstanding refined-design requests now (test thread).
    int runDesignJobsForTests() { return worker_.runPending(); }
    // Manual mode: run the analyzer over the audio tapped so far (test thread).
    uint64_t runAnalyzerForTests() noexcept { return analyzer_.runPending(); }

    // Allocates the work buffers (main thread). Returns false, and the engine outputs silence,
    // for an unsupported sample rate. Ramps and filters are seeded from the current table
    // values, never from defaults (area 09 R14).
    bool prepare(double sampleRate, uint32_t maxFrames) noexcept;
    void release() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return prepared_; }
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    // The latency of the processing that actually runs (area 02 R9, R12). Only Zero Latency
    // with no spectral band is implemented in Phase 1, so this is 0 whatever global.mode says;
    // Phase 2 passes the active mode, resolution and spectral flag (area 21 R12).
    [[nodiscard]] uint32_t latencySamples() const noexcept {
        return prepared_ ? dsp::latencySamples(dsp::ProcessingMode::ZeroLatency, 0, sampleRate_, false) : 0u;
    }

    // Jump every ramp to the table value: state and preset restores apply without ramps
    // (area 09 R15). Audio thread, at a piece boundary.
    void snapToTable() noexcept;

    // One piece of a host block, frames <= maxFrames. in == out (in place) is allowed. A mono
    // input feeds every output; output channels beyond kMaxChannels are silenced.
    // sc / numSc: the external side-chain bus for this piece (area 01 R2), or null / 0 while the
    // host feeds none. It never reaches the main path; the dynamics (area 06) and the analyzer
    // (area 19) read it. For now it is metered.
    void process(const float* const* in, uint32_t numIn, float* const* out, uint32_t numOut,
                 uint32_t frames, const float* const* sc = nullptr, uint32_t numSc = 0) noexcept;

    // Value of a band field at the last parameter read (for tests and the editor).
    [[nodiscard]] double bandValue(int band, params::BandField field) const noexcept;
    [[nodiscard]] double gainScale() const noexcept { return gainScale_.current(); }
    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }
    // Current section count and tier of a band (tests).
    [[nodiscard]] size_t bandSectionCount(int band) const noexcept { return bands_[static_cast<size_t>(band)].coeffs.size(); }
    [[nodiscard]] bool bandRefined(int band) const noexcept { return bands_[static_cast<size_t>(band)].refined; }

    // Meters and gain-change snapshots (areas 11, 12, 14). The engine publishes on the audio
    // thread; the editor pulls on the GUI thread through the pull* functions only.
    [[nodiscard]] analysis::SnapshotPublisher& snapshots() noexcept { return publisher_; }
    // The analyzer engine (area 19). The audio thread taps pre-EQ input, post-EQ output and the
    // external side chain; the analyzer worker fills the spectrum snapshot. Its lifetime follows
    // prepare() / release(), never the editor.
    [[nodiscard]] const analysis::Analyzer& analyzer() const noexcept { return analyzer_; }

    // Test hooks: read and redesign intervals in samples. Production uses 4 and 32; 1 and 1
    // gives the per-sample "offline ideal" for zipper measurements.
    void setRedesignIntervalForTests(uint32_t samples) noexcept { redesignInterval_ = samples ? samples : 1; }
    void setReadIntervalForTests(uint32_t samples) noexcept { readInterval_ = samples ? samples : 1; }

private:
    struct Field {
        params::ParamIndex index = 0;
        RampPolicy policy;
        LinearRamp ramp;
    };
    struct Band {
        // Coefficients glide between designs: each redesign targets the ramp values one
        // redesign interval ahead, and g, k and the mix are interpolated per sample towards
        // it, so the coefficient path follows the ramp instead of stepping (R15).
        dsp::SvfCascade cur;    // parameters now
        dsp::SvfCascade step;   // per-sample increment while gliding
        dsp::SvfCascade goal;   // parameters at the end of the glide
        dsp::FixedVector<dsp::SvfCoeffs, dsp::kMaxDigitalSections> coeffs;
        uint32_t glideLeft = 0;
        std::array<std::array<dsp::SvfState, dsp::kMaxDigitalSections>, kMaxChannels> state{};
        LinearRamp wet;           // 0 = off, 1 = on (enable crossfade)
        bool designed = false;
        bool runnable = false;    // last design succeeded
        bool event = false;       // targets changed at this piece start
        uint8_t mask = 0;         // channels the band runs on (in its stage's domain)
        uint8_t stage = 0;        // 0 = L/R stage, 1 = M/S stage (area 03 R4)
        double dFreq = 0.0, dGain = 0.0, dQ = 0.0;  // inputs of the current design
        int dShape = -1, dSlope = -1;
        uint64_t lastDesignAt = 0;
        bool refined = false;     // current coefficients come from the refined design
        bool refineDone = false;  // refined (or failed) for the current design inputs
        uint64_t pendingId = 0;   // outstanding worker request
        uint64_t requestedAt = 0;
        std::array<double, 5> seen{-1, -1, -1, -1, -1};  // used, enabled, placement, shape, slope
    };

    static int rampSlot(params::BandField f) noexcept;
    Field& field(int band, int slot) noexcept { return fields_[static_cast<size_t>(band) * kRampedBandFields.size() + static_cast<size_t>(slot)]; }
    const Field& field(int band, int slot) const noexcept { return fields_[static_cast<size_t>(band) * kRampedBandFields.size() + static_cast<size_t>(slot)]; }
    void advanceBandRampsTo(uint64_t sample) noexcept;
    void retargetAll() noexcept;
    void retarget(Field& f, double target) noexcept;
    void readBands(uint64_t sample, bool pieceStart, uint32_t numOut, bool snapWet) noexcept;
    void runStage(uint8_t stage, double* const* w, uint32_t outCh, uint32_t off, uint32_t n) noexcept;
    void designBand(int b, uint64_t sample, double freqLog2, double gainDb, double qPos, int shape, int slope,
                    bool hardSwitch) noexcept;
    void resetBandStates() noexcept;
    void startGlide(Band& band, const dsp::SvfCascade& next, uint32_t samples, bool byProximity) noexcept;
    static void dropIdentitySections(Band& band) noexcept;
    void refineStep(int b, uint64_t sample) noexcept;

    const params::ParameterTable& table_;
    analysis::SnapshotPublisher publisher_;
    analysis::DisplaySpectrum display_;  // analyzer consumer; writes publisher_'s spectra
    analysis::Analyzer analyzer_;
    bool analyzerSubscribed_ = false;
    double sampleRate_ = 48000.0;
    bool prepared_ = false;
    uint32_t maxFrames_ = 0;
    uint32_t redesignInterval_ = kRedesignInterval;
    uint32_t readInterval_ = kReadInterval;
    uint64_t pos_ = 0;         // absolute sample index of the next piece
    uint64_t rampClock_ = 0;   // band ramps hold the value of sample rampClock_ - 1
    bool pendingSnap_ = false; // state restored: next read designs every band and snaps wet
    EngineStats stats_;
    DesignMode mode_ = DesignMode::Manual;
    std::atomic<bool> offline_{false};
    DesignWorker worker_;
    uint64_t nextRequestId_ = 0;
    uint64_t swapDelaySamples_ = 2400;
    uint32_t refineGlideSamples_ = 960;

    std::array<Field, params::kBandCount * kRampedBandFields.size()> fields_{};
    std::array<Band, params::kBandCount> bands_{};
    std::vector<double> work_;  // kMaxChannels * maxFrames, allocated in prepare()

    LinearRamp gainScale_;
    RampPolicy gainScalePolicy_;
    LinearRamp levelGain_;   // linear gain; time from the dB distance
    double levelTargetDb_ = 0.0;
    RampPolicy levelPolicy_;
    LinearRamp pan_;         // -100 .. +100
    RampPolicy panPolicy_;
    LinearRamp polarity_;    // +1 / -1, always ramped
    RampPolicy polarityPolicy_;
    RampPolicy wetPolicy_;
};

}  // namespace amq::engine
