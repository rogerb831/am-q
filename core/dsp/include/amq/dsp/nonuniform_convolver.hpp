#pragma once
// Non-uniformly partitioned FIR convolution for long Linear Phase kernels (area 05 R4; Gardner,
// "Efficient Convolution without Input-Output Delay", JAES 1995; Wefers 2015, ch. 5).
//
// A uniform convolver with a 64-sample partition would need 2048 partitions for a 131072-tap
// kernel. Here the kernel is cut into segments whose partitions grow by 4 per step, each run by a
// uniform PartitionedConvolver (partitioned_convolver.hpp):
//
//   head      partition P0       taps [0, 4 P0)
//   then      partition 4 P0     the next 4 blocks, then 16 P0 for the next 4, ... up to Pmax,
//   last      partition Pmax     the rest of the kernel.
//
// Segment i starts at tap S_i with partition P_i and has latency P_i, so its input is delayed by
// D_i = S_i + P0 - P_i (>= 0 because S_i >= P_i for every later segment); the sum is the full
// convolution with the head's latency P0. Cost per sample is about 4 multiply-accumulates per
// small segment plus (L - S_last) / Pmax for the last; the large FFTs land on the sample that
// completes a Pmax block (a CPU peak every Pmax samples).
//
// Kernel swap: submitKernel() (writer thread) hands every segment its slice; each segment switches
// at its own next partition boundary (no fade), so the response is a mix of old and new slices
// until the largest segment has switched. The engine never listens to a convolver while it swaps:
// it keeps a standby convolver, fed the same input, and crossfades to it once swapInProgress() is
// false (engine/linear_phase.hpp).
//
// prepare() and setKernel() allocate (main thread, not while processing). process() and reset()
// are real-time safe. submitKernel() does not allocate.

#include "amq/dsp/partitioned_convolver.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace amq::dsp {

class NonUniformConvolver {
public:
    struct Segment {
        int start = 0, length = 0, partition = 0, delay = 0;
    };
    // The segment plan for a kernel of maxKernel taps.
    [[nodiscard]] static std::vector<Segment> plan(int headPartition, int maxPartition, int maxKernel);

    bool prepare(int headPartition, int maxPartition, int maxKernel);
    bool setKernel(const float* h, int length);
    bool submitKernel(const float* h, int length) noexcept;
    [[nodiscard]] bool swapInProgress() const noexcept;
    void process(const float* in, float* out, size_t n) noexcept;
    void reset() noexcept;

    [[nodiscard]] int latency() const noexcept { return head_; }
    [[nodiscard]] int maxKernel() const noexcept { return maxKernel_; }
    [[nodiscard]] const std::vector<Segment>& segments() const noexcept { return plan_; }

private:
    static constexpr size_t kChunk = 256;
    struct Seg {
        Segment s;
        PartitionedConvolver conv;
        std::vector<float> line;  // delay ring of s.delay samples
        size_t pos = 0;
    };
    std::vector<Segment> plan_;
    std::vector<std::unique_ptr<Seg>> seg_;  // PartitionedConvolver is not movable
    std::vector<float> in_, out_;  // kChunk scratch
    int head_ = 0, maxKernel_ = 0;
};

}  // namespace amq::dsp
