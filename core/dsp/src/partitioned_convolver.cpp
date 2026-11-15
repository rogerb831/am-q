#include "amq/dsp/partitioned_convolver.hpp"

#include <algorithm>
#include <cstring>

namespace amq::dsp {

bool PartitionedConvolver::prepare(int partition, int maxKernel, int fadeSamples) {
    if (partition < 16 || maxKernel < 1 || fadeSamples < 0 || !RealFft::validSize(2 * partition)) return false;
    b_ = partition;
    n_ = 2 * partition;
    maxParts_ = (maxKernel + b_ - 1) / b_;
    fadeLen_ = fadeSamples;
    fft_ = std::make_unique<RealFft>(n_);
    writerFft_ = std::make_unique<RealFft>(n_);
    const auto sz = static_cast<size_t>(n_);
    for (auto& k : kernel_) {
        k.clear();
        for (int i = 0; i < maxParts_; ++i) k.emplace_back(sz);
    }
    fdl_.clear();
    for (int i = 0; i < maxParts_; ++i) fdl_.emplace_back(sz);
    time_ = FftBuffer(sz);
    acc_ = FftBuffer(sz);
    accOld_ = FftBuffer(sz);
    writerScratch_ = FftBuffer(sz);
    outBlock_ = FftBuffer(static_cast<size_t>(b_));
    parts_ = {0, 0};
    active_ = 0;
    spare_ = 1;
    fading_ = false;
    handoff_.store(kFree, std::memory_order_release);
    reset();
    return true;
}

void PartitionedConvolver::transformKernel(RealFft& fft, FftBuffer& scratch, int slot, const float* h,
                                           int length) noexcept {
    // Each partition is zero-padded to 2B; overlap-save keeps the last B outputs, which are the
    // linear convolution of the last 2B inputs with a B-tap partition.
    const int parts = (length + b_ - 1) / b_;
    float* t = scratch.data();
    for (int p = 0; p < parts; ++p) {
        std::memset(t, 0, static_cast<size_t>(n_) * sizeof(float));
        const int count = std::min(b_, length - p * b_);
        std::memcpy(t, h + static_cast<size_t>(p) * b_, static_cast<size_t>(count) * sizeof(float));
        fft.forwardPacked(t, kernel_[static_cast<size_t>(slot)][static_cast<size_t>(p)].data());
    }
    parts_[static_cast<size_t>(slot)] = parts;
}

bool PartitionedConvolver::setKernel(const float* h, int length) {
    if (!fft_ || length < 0 || (length > 0 && !h)) return false;
    if ((length + b_ - 1) / b_ > maxParts_) return false;
    fading_ = false;
    transformKernel(*fft_, acc_, active_, h, length);
    spare_ = 1 - active_;
    handoff_.store(kFree, std::memory_order_release);
    reset();
    return true;
}

bool PartitionedConvolver::submitKernel(const float* h, int length) noexcept {
    if (!writerFft_ || length < 0 || (length > 0 && !h)) return false;
    if ((length + b_ - 1) / b_ > maxParts_) return false;
    if (handoff_.load(std::memory_order_acquire) != kFree) return false;
    transformKernel(*writerFft_, writerScratch_, spare_, h, length);
    handoff_.store(kReady, std::memory_order_release);
    return true;
}

void PartitionedConvolver::finishFade() noexcept {
    fading_ = false;
    spare_ = 1 - active_;
    handoff_.store(kFree, std::memory_order_release);
}

void PartitionedConvolver::reset() noexcept {
    for (auto& f : fdl_) f.clear();
    time_.clear();
    acc_.clear();
    accOld_.clear();
    outBlock_.clear();
    fdlHead_ = 0;
    fill_ = 0;
    if (fading_) finishFade();
}

void PartitionedConvolver::accumulate(int slot, FftBuffer& acc) noexcept {
    // y = sum_p X[now - p] * H[p], scaled for the unnormalised inverse.
    acc.clear();
    const float scale = 1.0f / static_cast<float>(n_);
    const auto& k = kernel_[static_cast<size_t>(slot)];
    const int parts = parts_[static_cast<size_t>(slot)];
    for (int p = 0; p < parts; ++p) {
        const int ring = (fdlHead_ + p) % maxParts_;
        fft_->multiplyAccumulatePacked(fdl_[static_cast<size_t>(ring)].data(), k[static_cast<size_t>(p)].data(),
                                       acc.data(), scale);
    }
    fft_->inversePacked(acc.data(), acc.data());
}

void PartitionedConvolver::processPartition() noexcept {
    // Pick up a submitted kernel at the partition boundary.
    if (!fading_ && handoff_.load(std::memory_order_acquire) == kReady) {
        active_ = spare_;
        handoff_.store(kBusy, std::memory_order_relaxed);
        fading_ = true;
        fadePos_ = 0;
    }
    // Spectrum of the last 2B inputs into the delay line.
    fdlHead_ = fdlHead_ == 0 ? maxParts_ - 1 : fdlHead_ - 1;
    fft_->forwardPacked(time_.data(), fdl_[static_cast<size_t>(fdlHead_)].data());

    accumulate(active_, acc_);
    const float* now = acc_.data() + b_;
    float* out = outBlock_.data();
    if (fading_ && fadePos_ < fadeLen_) {
        accumulate(1 - active_, accOld_);
        const float* old = accOld_.data() + b_;
        const float inv = 1.0f / static_cast<float>(fadeLen_);
        for (int i = 0; i < b_; ++i) {
            const float g = std::min(1.0f, static_cast<float>(fadePos_ + i + 1) * inv);
            out[i] = old[i] + g * (now[i] - old[i]);
        }
        fadePos_ += b_;
    } else {
        std::memcpy(out, now, static_cast<size_t>(b_) * sizeof(float));
    }
    if (fading_ && fadePos_ >= fadeLen_) finishFade();
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
