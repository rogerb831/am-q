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
// PROVISIONAL guard, not the area 03 R15 regularisation (Phase 2): each digital response is
// floored relative to its own analog target before inversion, so no single ratio exceeds
// +24 dB, and every correction entry is capped at +24 dB. No Nyquist phase taper yet. Brickwall bands (unsupported prototypes) are left out of the
// correction (area 03 R16).
//
// Scheduling (area 04 R13). The audio thread posts a CorrectionRequest (a copy of the active
// bands' design inputs and cascades) at most every kKernelIntervalMs while bands change. The
// kernel worker builds the four kernels; the audio thread hands them to the convolvers together,
// at one partition boundary, and they crossfade (area 05 R8). Nothing on the audio thread
// allocates or locks: requests and results go through single-writer sequence slots.

#include "amq/dsp/natural_phase_kernel.hpp"
#include "amq/dsp/partitioned_convolver.hpp"
#include "amq/dsp/realtime_design.hpp"
#include "amq/engine/design_worker.hpp"
#include "amq/params/param_spec.hpp"

#include <array>
#include <atomic>
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

// Builds the kernels for a request. Uses the builder's FFT; one builder per thread.
void buildCorrectionKernels(const CorrectionRequest& r, dsp::NaturalPhaseKernelBuilder& builder,
                            CorrectionKernels& out) noexcept;

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
