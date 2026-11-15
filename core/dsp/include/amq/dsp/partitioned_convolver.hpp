#pragma once
// Uniformly partitioned FIR convolution (area 05 R3; Gardner, "Efficient Convolution without
// Input-Output Delay", JAES 1995; Wefers, "Partitioned convolution algorithms for real-time
// auralization", 2015). Uniformly partitioned overlap-save with a frequency-domain delay line:
//   - the kernel is cut into P partitions of B samples, each transformed once (FFT size 2B);
//   - every B input samples: transform the last 2B inputs, push the spectrum into the delay
//     line, multiply-accumulate it against the P kernel spectra, inverse transform, and keep
//     the last B samples.
// Latency is exactly B samples (the input partition is collected before it is processed). Any
// host block size works; samples are buffered internally.
//
// Kernel swap (area 05 R8). submitKernel() runs on one writer thread (the kernel worker) while
// process() runs. The audio thread picks the new kernel up at the next partition boundary and
// crossfades linearly from the old kernel's output to the new one's over fadeSamples. The
// delay line holds input spectra only, so both kernels share it: during a fade the cost is one
// extra multiply-accumulate pass and one extra inverse FFT per partition.
//
// Two kernel slots: the active one and the other, which is either the writer's spare or the
// kernel fading out. A submit while a fade is still running is refused (returns false); the
// caller retries later (rate limiting is the scheduler's job, area 04 R13).
//
// prepare() and setKernel() allocate and run off the audio thread, never concurrently with
// process() or submitKernel(). process() and reset() are real-time safe. submitKernel() does
// not allocate.

#include "amq/dsp/fft.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace amq::dsp {

class PartitionedConvolver {
public:
    static constexpr int kDefaultPartition = 64;    // area 05 design
    static constexpr int kDefaultFadeSamples = 256;  // PROVISIONAL: 5.3 ms at 48 kHz, tuned by listening

    // partition: B, a valid RealFft half-size (2B must be valid: B >= 16, B = 2^a 3^b 5^c).
    // maxKernel: the longest kernel any call will get. fadeSamples: crossfade length of a
    // submitted kernel (0 switches at the partition boundary). Returns false for invalid input.
    bool prepare(int partition, int maxKernel, int fadeSamples = kDefaultFadeSamples);
    // Replaces the kernel at once and clears the delay line. Off the audio thread only.
    bool setKernel(const float* h, int length);
    // Writer thread: hands over a kernel for a crossfaded swap. False if not prepared, the
    // kernel is too long, or the previous swap is still pending or fading.
    bool submitKernel(const float* h, int length) noexcept;
    // True while a submitted kernel waits for the audio thread or is fading in.
    [[nodiscard]] bool swapInProgress() const noexcept { return handoff_.load(std::memory_order_acquire) != kFree; }

    void process(const float* in, float* out, size_t n) noexcept;
    // Clears the delay line and the output; a running fade completes at once.
    void reset() noexcept;

    [[nodiscard]] int latency() const noexcept { return b_; }
    [[nodiscard]] int partition() const noexcept { return b_; }
    [[nodiscard]] int partitions() const noexcept { return parts_[static_cast<size_t>(active_)]; }
    [[nodiscard]] int fadeSamples() const noexcept { return fadeLen_; }

private:
    enum : int { kFree = 0, kReady = 1, kBusy = 2 };

    void processPartition() noexcept;
    void transformKernel(RealFft& fft, FftBuffer& scratch, int slot, const float* h, int length) noexcept;
    void accumulate(int slot, FftBuffer& acc) noexcept;
    void finishFade() noexcept;

    int b_ = 0;          // partition size B
    int n_ = 0;          // FFT size 2B
    int maxParts_ = 0;
    int fadeLen_ = 0;
    std::unique_ptr<RealFft> fft_;        // audio thread
    std::unique_ptr<RealFft> writerFft_;  // kernel writer thread
    std::array<std::vector<FftBuffer>, 2> kernel_;  // per slot: P packed spectra
    std::array<int, 2> parts_{};                     // partitions per slot
    std::vector<FftBuffer> fdl_;     // input spectra, ring of maxParts_
    int fdlHead_ = 0;
    FftBuffer time_;                 // 2B: last B inputs then the new B
    FftBuffer acc_;                  // packed accumulator, then time-domain result
    FftBuffer accOld_;               // the same for the kernel fading out
    FftBuffer writerScratch_;        // 2B, writer thread
    FftBuffer outBlock_;             // B outputs of the last partition
    int fill_ = 0;                   // inputs collected in the current partition

    // Hand-off. Audio-thread state: active_, fading_, fadePos_. spare_ is written by the audio
    // thread before it releases kFree and read by the writer after it acquires kFree.
    int active_ = 0;
    bool fading_ = false;
    int fadePos_ = 0;
    int spare_ = 1;
    std::atomic<int> handoff_{kFree};
};

}  // namespace amq::dsp
