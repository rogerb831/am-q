#pragma once
// Natural Phase correction stage (area 03 R14-R16, R18; area 04 R9, R10, R13; area 05 R1-R3, R8).
//
// Audio passes the band IIR filters first (L/R stage, then the M/S stage), then this stage: one
// combined 2x2 correction FIR per channel pair (area 02 "Natural Phase", area 03 "One combined
// Natural Phase correction"). Per kernel bin the correction is
//
//     C = T * H^-1,   H = Dec * diag(MSd) * Enc * diag(LRd),   T = the same with analog targets,
//
// where LRd / MSd are the per-channel products of the digital responses of the active bands of
// each stage (the exact cascades the audio runs, R18 / area 04 R10) and LRa / MSa the products of
// their analog targets at the true bin frequencies (area 04 R9, area 05 R6). Without Mid or Side
// bands T and H are diagonal and C is the plain per-channel ratio. The four kernels (LL, LR, RL,
// RR) run in four partitioned convolvers: yL = LL*xL + LR*xR, yR = RL*xL + RR*xR. Mono uses LL.
//
// Latency is the kernel centre plus the partition: 256 + 64 = 320 samples (area 05 R2), the
// value of dsp::latencySamples(NaturalPhase, ...).
//
// Regularisation (area 03 R15, chosen by measurement; see the area 03 task note):
//   1. Removable-singularity limit: where a band's digital response is below -120 dB on a bin
//      (a zero on the bin: target and digital response are both rounding noise), the
//      correction is the mean of the correction just either side, the limit of the ratio.
//   2. Capped ratio: each digital response is floored relative to its own analog target before
//      inversion, and every correction entry is capped at +24 dB (PROVISIONAL).
//   3. Nyquist phase taper: above 0.97 Nyquist the unwrapped phase fades to 0 with a raised
//      cosine, so the real Nyquist bin is reached without a jump (PROVISIONAL position). Brickwall bands (unsupported prototypes) are left out of the
// correction (area 03 R16).
//
// Scheduling (area 04 R13). The audio thread posts a CorrectionRequest (a copy of the active
// bands' design inputs and cascades) at most every kKernelIntervalMs while bands change. The
// kernel worker builds the four kernels; the audio thread hands them to the convolvers together,
// at one partition boundary, and they crossfade (area 05 R8). Nothing on the audio thread
// allocates or locks: requests and results go through single-writer sequence slots.

#include "amq/dsp/analog_prototype.hpp"
#include "amq/dsp/natural_phase_kernel.hpp"
#include "amq/dsp/partitioned_convolver.hpp"
#include "amq/dsp/realtime_design.hpp"
#include "amq/engine/design_worker.hpp"
#include "amq/params/param_spec.hpp"

#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace amq::engine {

inline constexpr double kKernelIntervalMs = 10.0;    // PROVISIONAL: minimum time between kernel requests
inline constexpr double kCorrectionCapDb = 24.0;     // PROVISIONAL guard (see above)

struct CorrectionBand {
    bool active = false;   // audible on at least one channel (enabled, designed, wet target 1)
    uint8_t stage = 0;     // 0 = L/R stage, 1 = M/S stage
    uint8_t mask = 0;      // channels in the stage's domain (bit 0 = L or M, bit 1 = R or S)
    DesignInputs in;       // analog target inputs: the inputs of the running design
    dsp::SvfCascade cascade;  // the digital sections the audio runs (the glide target)
};

struct CorrectionRequest {
    uint64_t id = 0;        // 0 = none
    double sampleRate = 48000.0;
    uint32_t channels = 2;  // 1 = mono: only the LL kernel is used
    std::array<CorrectionBand, params::kBandCount> bands{};
};

// The four kernels of one correction, row-major: LL, LR, RL, RR.
struct CorrectionKernels {
    static constexpr int kTaps = dsp::NaturalPhaseKernelBuilder::kTaps;
    uint64_t id = 0;
    std::array<std::array<float, kTaps>, 4> taps{};
};

// Regularisation choices (area 03 R15). The defaults are what the engine runs; the other
// settings exist for the evaluation report in tests/engine/test_natural_phase.cpp.
struct CorrectionOptions {
    bool singularityLimit = true;  // take the limit where a band's zero sits on a bin
    double taperStart = 0.97;      // Nyquist phase taper from this fraction of Nyquist; >= 1: off (PROVISIONAL, by measurement)
};
// A digital band response below this (-120 dB) on a bin is rounding noise: take the limit.
inline constexpr double kSingularResponse = 1e-6;

// Per-band response caches (area 04 R12). For each band, the analog target and the digital
// response at every kernel bin, with the prototype. An entry is valid while the band's design
// inputs and its exact cascade are unchanged (compared by value, so it cannot go stale), so a
// one-band edit recomputes only that band. One cache per building thread; about 200 KB, so
// allocate it off the audio thread.
struct BandResponseCache {
    bool valid = false;
    DesignInputs in;
    dsp::SvfCascade cascade;
    dsp::AnalogPrototype proto;
    std::array<std::complex<double>, dsp::NaturalPhaseKernelBuilder::kBins> target{}, digital{};
};
struct CorrectionCache {
    std::array<BandResponseCache, params::kBandCount> bands{};
    uint64_t recomputed = 0;  // band entries (re)computed since construction
};

// Builds the kernels for a request, with the thread's builder and cache. No allocation.
void buildCorrectionKernels(const CorrectionRequest& r, dsp::NaturalPhaseKernelBuilder& builder,
                            CorrectionCache& cache, CorrectionKernels& out, const CorrectionOptions& opt = {}) noexcept;
// The same with a fresh cache (allocates: tests and reports only).
void buildCorrectionKernels(const CorrectionRequest& r, dsp::NaturalPhaseKernelBuilder& builder,
                            CorrectionKernels& out, const CorrectionOptions& opt = {});

class NaturalPhaseStage {
public:
    NaturalPhaseStage() = default;
    ~NaturalPhaseStage() { stopWorker(); }
    NaturalPhaseStage(const NaturalPhaseStage&) = delete;
    NaturalPhaseStage& operator=(const NaturalPhaseStage&) = delete;

    // Main thread, not processing. Allocates the convolvers and the float buffers, and builds
    // the first kernels from `initial` synchronously (no crossfade).
    bool prepare(uint32_t maxFrames, const CorrectionRequest& initial);
    void startWorker();
    void stopWorker();

    // Audio thread: hand a request to the worker (replaces any request not yet built).
    void post(const CorrectionRequest& r) noexcept;
    // Audio thread, offline rendering only: build the kernels now (no worker, no waiting).
    void buildNow(const CorrectionRequest& r) noexcept;
    // Audio thread: runs the correction in place on the work buffers (double), n <= maxFrames.
    // Picks up a finished kernel set first, when the convolvers can take it.
    void process(double* const* w, uint32_t channels, uint32_t n) noexcept;

    // Worker thread, or the test thread in Manual mode: builds the latest request if new.
    int runPending();

    [[nodiscard]] uint64_t swaps() const noexcept { return swaps_; }
    // Band responses the worker's cache has (re)computed since prepare (tests, Manual mode).
    [[nodiscard]] uint64_t cacheRecomputes() const noexcept { return workerCache_ ? workerCache_->recomputed : 0; }
    [[nodiscard]] uint64_t appliedId() const noexcept { return appliedId_; }
    [[nodiscard]] int latency() const noexcept {
        return dsp::NaturalPhaseKernelBuilder::kCentre + dsp::PartitionedConvolver::kDefaultPartition;
    }

private:
    void submitAll(const CorrectionKernels& k) noexcept;

    std::array<dsp::PartitionedConvolver, 4> conv_;
    std::unique_ptr<dsp::NaturalPhaseKernelBuilder> workerBuilder_;  // worker / test thread
    std::unique_ptr<dsp::NaturalPhaseKernelBuilder> audioBuilder_;   // offline renders
    std::unique_ptr<CorrectionRequest> workerRequest_;               // worker scratch
    std::unique_ptr<CorrectionKernels> workerKernels_;               // worker scratch
    std::unique_ptr<CorrectionKernels> audioKernels_;                // audio-thread scratch
    std::unique_ptr<CorrectionCache> workerCache_;                   // worker / test thread
    std::unique_ptr<CorrectionCache> audioCache_;                    // offline renders
    std::vector<float> buf_;  // 6 * maxFrames: xL, xR, then the four convolver outputs
    uint32_t maxFrames_ = 0;

    SeqSlot<CorrectionRequest> request_;
    SeqSlot<CorrectionKernels> result_;
    std::atomic<uint64_t> readyId_{0};
    uint64_t builtId_ = 0;     // worker: last request built
    uint64_t appliedId_ = 0;   // audio: last kernel set handed to the convolvers
    bool pendingOffline_ = false;
    uint64_t swaps_ = 0;

    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace amq::engine
