#pragma once
// Linear Phase stage (area 02 R8, R9, R11; area 04 R11; area 05 R4, R5, R8).
//
// Every static band's exact analog magnitude (area 04 R11: a zero-phase magnitude object per
// band), evaluated at the kernel's bin frequencies, is multiplied per channel and combined into a
// 2x2 kernel set like the Natural Phase correction (area 03): with l, r the L/R-stage products and
// m, s the M/S-stage products,
//     LL = (m + s) l / 2,  LR = (m - s) r / 2,  RL = (m - s) l / 2,  RR = (m + s) r / 2.
// Each kernel is zero-phase, delayed by N / 2 to be causal, inverse-transformed and tapered with a
// Tukey window (outer 12.5 % per side, PROVISIONAL), so it is exactly symmetric: linear phase.
//
// N is the resolution's FIR length times the rate factor (area 02 R11), and the head partition is
// 64 times the rate factor, so the latency is N / 2 + partition (dsp/latency.hpp). The kernels run
// in non-uniformly partitioned convolvers (dsp/nonuniform_convolver.hpp).
//
// Kernel changes (area 05 R8): two complete convolver sets are always fed the same input. The
// worker builds a new kernel set and submits it to the standby set; once every segment there has
// switched, the audio thread crossfades the output from the active set to the standby set over
// kLinearPhaseFadeMs and the two swap roles. Nothing on the audio thread allocates or locks.
// Requests are rate-limited by the engine like Natural Phase (area 04 R13); a kernel takes effect
// after its build, the largest partition and the crossfade.

#include "amq/dsp/fft.hpp"
#include "amq/dsp/nonuniform_convolver.hpp"
#include "amq/engine/natural_phase.hpp"

#include <array>
#include <atomic>
#include <complex>
#include <memory>
#include <thread>
#include <vector>

namespace amq::engine {

inline constexpr double kLinearPhaseFadeMs = 20.0;      // PROVISIONAL: output crossfade between kernel sets
inline constexpr double kLinearPhaseTukeyAlpha = 0.25;  // PROVISIONAL: tapered fraction of the kernel
inline constexpr int kLinearPhaseMaxPartitionBase = 4096;  // largest segment partition at 44.1 / 48 kHz

class LinearPhaseStage {
public:
    LinearPhaseStage() = default;
    ~LinearPhaseStage() { stopWorker(); }
    LinearPhaseStage(const LinearPhaseStage&) = delete;
    LinearPhaseStage& operator=(const LinearPhaseStage&) = delete;

    // Main thread, not processing: sizes everything for this rate and resolution and loads the
    // kernels for `initial` synchronously.
    bool prepare(uint32_t maxFrames, double sampleRate, int resolution, const CorrectionRequest& initial);
    void startWorker();
    void stopWorker();

    void post(const CorrectionRequest& r) noexcept;  // audio thread
    void buildNow(const CorrectionRequest& r) noexcept;  // audio thread, offline rendering only
    void process(double* const* w, uint32_t channels, uint32_t n) noexcept;  // audio thread
    int runPending();  // worker (or the test thread in Manual mode)

    [[nodiscard]] int taps() const noexcept { return taps_; }
    [[nodiscard]] int latency() const noexcept { return taps_ / 2 + head_; }
    [[nodiscard]] uint64_t swaps() const noexcept { return swaps_; }
    [[nodiscard]] uint64_t appliedId() const noexcept { return appliedId_; }
    // Per-thread kernel building scratch (the worker and, offline, the audio thread).
    struct Builder {
        std::unique_ptr<dsp::RealFft> fft;
        std::vector<std::complex<float>> spec;
        std::vector<double> mag;  // 4 * (taps/2 + 1): l, r, m, s
        std::array<std::vector<float>, 4> taps;
        uint64_t pending = 0;     // built, not yet submitted
    };
    // Builds the kernels of a request into b.taps (four arrays of taps() floats).
    void build(const CorrectionRequest& r, Builder& b) const;
    // The test thread's view of the worker's last build.
    [[nodiscard]] const std::array<std::vector<float>, 4>& lastBuilt() const noexcept { return worker_.taps; }

private:
    enum : int { kIdle = 0, kSubmitted = 1, kSwitching = 2 };
    bool trySubmit(Builder& b) noexcept;  // hand b's kernels to the standby set
    bool allocate(Builder& b);

    int taps_ = 0, head_ = 0;
    double sampleRate_ = 48000.0;
    uint32_t maxFrames_ = 0;
    std::array<std::array<dsp::NonUniformConvolver, 4>, 2> set_;
    std::vector<float> buf_;  // 2 inputs + 8 outputs, maxFrames each
    std::vector<float> window_;
    Builder worker_, audio_;
    std::unique_ptr<CorrectionRequest> workerRequest_;

    SeqSlot<CorrectionRequest> request_;
    uint64_t builtId_ = 0;        // worker
    std::atomic<int> active_{0};  // set the listener hears (audio writes, only while not idle)
    std::atomic<int> phase_{kIdle};
    std::atomic<uint64_t> standbyId_{0};
    uint64_t appliedId_ = 0;      // audio
    uint32_t fadeLen_ = 0, fadePos_ = 0;
    uint64_t swaps_ = 0;

    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace amq::engine
