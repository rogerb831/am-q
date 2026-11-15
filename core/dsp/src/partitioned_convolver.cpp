#include "amq/dsp/partitioned_convolver.hpp"

#include <algorithm>
#include <cstring>

namespace amq::dsp {

bool PartitionedConvolver::prepare(int partition, int maxKernel) {
    if (partition < 16 || maxKernel < 1 || !RealFft::validSize(2 * partition)) return false;
    b_ = partition;
    n_ = 2 * partition;
    maxParts_ = (maxKernel + b_ - 1) / b_;
    fft_ = std::make_unique<RealFft>(n_);
    const auto sz = static_cast<size_t>(n_);
    kernel_.clear();
    fdl_.clear();
    for (int i = 0; i < maxParts_; ++i) {
        kernel_.emplace_back(sz);
        fdl_.emplace_back(sz);
    }
    time_ = FftBuffer(sz);
    acc_ = FftBuffer(sz);
    outBlock_ = FftBuffer(static_cast<size_t>(b_));
    p_ = 0;
    reset();
    return true;
}

bool PartitionedConvolver::setKernel(const float* h, int length) {
    if (!fft_ || length < 0 || (length > 0 && !h)) return false;
    const int parts = (length + b_ - 1) / b_;
    if (parts > maxParts_) return false;
    // Each partition is zero-padded to 2B; overlap-save keeps the last B outputs, which are the
    // linear convolution of the last 2B inputs with a B-tap partition.
    float* t = acc_.data();
    for (int p = 0; p < parts; ++p) {
        std::memset(t, 0, static_cast<size_t>(n_) * sizeof(float));
        const int count = std::min(b_, length - p * b_);
        std::memcpy(t, h + static_cast<size_t>(p) * b_, static_cast<size_t>(count) * sizeof(float));
        fft_->forwardPacked(t, kernel_[static_cast<size_t>(p)].data());
    }
    p_ = parts;
    reset();
    return true;
}

void PartitionedConvolver::reset() noexcept {
    for (auto& f : fdl_) f.clear();
    time_.clear();
    acc_.clear();
    outBlock_.clear();
    fdlHead_ = 0;
    fill_ = 0;
}

void PartitionedConvolver::processPartition() noexcept {
    // Spectrum of the last 2B inputs into the delay line.
    fdlHead_ = fdlHead_ == 0 ? maxParts_ - 1 : fdlHead_ - 1;
    fft_->forwardPacked(time_.data(), fdl_[static_cast<size_t>(fdlHead_)].data());
    // y = sum_p X[now - p] * H[p], scaled for the unnormalised inverse.
    acc_.clear();
    const float scale = 1.0f / static_cast<float>(n_);
    for (int p = 0; p < p_; ++p) {
        const int slot = (fdlHead_ + p) % maxParts_;
        fft_->multiplyAccumulatePacked(fdl_[static_cast<size_t>(slot)].data(), kernel_[static_cast<size_t>(p)].data(),
                                       acc_.data(), scale);
    }
    fft_->inversePacked(acc_.data(), acc_.data());
    std::memcpy(outBlock_.data(), acc_.data() + b_, static_cast<size_t>(b_) * sizeof(float));
    // Slide: the new half becomes the old half.
    std::memcpy(time_.data(), time_.data() + b_, static_cast<size_t>(b_) * sizeof(float));
}

void PartitionedConvolver::process(const float* in, float* out, size_t n) noexcept {
    if (!fft_) {
        std::fill(out, out + n, 0.0f);
        return;
    }
    float* newHalf = time_.data() + b_;
    for (size_t i = 0; i < n; ++i) {
        // Output the previous partition's result sample by sample: latency B.
        out[i] = outBlock_.data()[fill_];
        newHalf[fill_] = in[i];
        if (++fill_ == b_) {
            fill_ = 0;
            processPartition();
        }
    }
}

}  // namespace amq::dsp
