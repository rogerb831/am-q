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
// prepare() and setKernel() allocate and run off the audio thread. process() and reset() are
// real-time safe. The crossfaded kernel swap (area 05 R8) is built on top in Phase 2.

#include "amq/dsp/fft.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace amq::dsp {

class PartitionedConvolver {
public:
    static constexpr int kDefaultPartition = 64;  // area 05 design

    // partition: B, a valid RealFft half-size (2B must be valid: B >= 16, B = 2^a 3^b 5^c).
    // maxKernel: the longest kernel setKernel() will get. Returns false for invalid input.
    bool prepare(int partition, int maxKernel);
    // Off the audio thread, while process() is not running. Clears the delay line.
    bool setKernel(const float* h, int length);

    void process(const float* in, float* out, size_t n) noexcept;
    void reset() noexcept;

    [[nodiscard]] int latency() const noexcept { return b_; }
    [[nodiscard]] int partition() const noexcept { return b_; }
    [[nodiscard]] int partitions() const noexcept { return p_; }

private:
    void processPartition() noexcept;

    int b_ = 0;          // partition size B
    int n_ = 0;          // FFT size 2B
    int maxParts_ = 0;
    int p_ = 0;          // partitions of the current kernel
    std::unique_ptr<RealFft> fft_;
    std::vector<FftBuffer> kernel_;  // P packed spectra
    std::vector<FftBuffer> fdl_;     // input spectra, ring of maxParts_
    int fdlHead_ = 0;
    FftBuffer time_;                 // 2B: last B inputs then the new B
    FftBuffer acc_;                  // packed accumulator, then time-domain result
    FftBuffer outBlock_;             // B outputs of the last partition
    int fill_ = 0;                   // inputs collected in the current partition
};

}  // namespace amq::dsp
