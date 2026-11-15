// RealFft on PFFFT (area 01 R22). The only file in core/dsp that touches third-party code.
#include "amq/dsp/fft.hpp"

#include <pffft/pffft.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace amq::dsp {

// ---------------------------------------------------------------------------
// FftBuffer
// ---------------------------------------------------------------------------

FftBuffer::FftBuffer(size_t n) : n_(n) {
    if (n == 0) return;
    p_ = static_cast<float*>(pffft_aligned_malloc(n * sizeof(float)));
    if (!p_) throw std::bad_alloc();
    clear();
}

FftBuffer::~FftBuffer() {
    if (p_) pffft_aligned_free(p_);
}

FftBuffer::FftBuffer(FftBuffer&& o) noexcept : p_(std::exchange(o.p_, nullptr)), n_(std::exchange(o.n_, 0)) {}

FftBuffer& FftBuffer::operator=(FftBuffer&& o) noexcept {
    if (this != &o) {
        if (p_) pffft_aligned_free(p_);
        p_ = std::exchange(o.p_, nullptr);
        n_ = std::exchange(o.n_, 0);
    }
    return *this;
}

void FftBuffer::clear() noexcept {
    if (p_) std::memset(p_, 0, n_ * sizeof(float));
}

// ---------------------------------------------------------------------------
// RealFft
// ---------------------------------------------------------------------------

struct RealFft::Impl {
    PFFFT_Setup* setup = nullptr;
    FftBuffer work, a, b;
    ~Impl() {
        if (setup) pffft_destroy_setup(setup);
    }
};

bool RealFft::validSize(int n) noexcept { return n > 0 && pffft_is_valid_size(n, PFFFT_REAL) != 0; }

RealFft::RealFft(int n) : impl_(std::make_unique<Impl>()), n_(n) {
    if (!validSize(n)) throw std::invalid_argument("RealFft: size must be 32 * 2^a * 3^b * 5^c");
    impl_->setup = pffft_new_setup(n, PFFFT_REAL);
    if (!impl_->setup) throw std::invalid_argument("RealFft: backend rejected the size");
    const auto sz = static_cast<size_t>(n);
    impl_->work = FftBuffer(sz);
    impl_->a = FftBuffer(sz);
    impl_->b = FftBuffer(sz);
}

RealFft::~RealFft() = default;

void RealFft::forward(const float* in, std::complex<float>* out) noexcept {
    float* x = impl_->a.data();
    float* y = impl_->b.data();
    std::memcpy(x, in, static_cast<size_t>(n_) * sizeof(float));
    pffft_transform_ordered(impl_->setup, x, y, impl_->work.data(), PFFFT_FORWARD);
    // Ordered real layout: y[0] = DC, y[1] = Nyquist, then (re, im) pairs for bins 1..n/2-1.
    const int half = n_ / 2;
    out[0] = {y[0], 0.0f};
    out[half] = {y[1], 0.0f};
    for (int k = 1; k < half; ++k) out[k] = {y[2 * k], y[2 * k + 1]};
}

void RealFft::inverse(const std::complex<float>* in, float* out) noexcept {
    float* x = impl_->a.data();
    float* y = impl_->b.data();
    const int half = n_ / 2;
    x[0] = in[0].real();
    x[1] = in[half].real();
    for (int k = 1; k < half; ++k) {
        x[2 * k] = in[k].real();
        x[2 * k + 1] = in[k].imag();
    }
    pffft_transform_ordered(impl_->setup, x, y, impl_->work.data(), PFFFT_BACKWARD);
    const float scale = 1.0f / static_cast<float>(n_);
    for (int i = 0; i < n_; ++i) out[i] = y[i] * scale;
}

void RealFft::forwardPacked(const float* in, float* packed) noexcept {
    pffft_transform(impl_->setup, in, packed, impl_->work.data(), PFFFT_FORWARD);
}

void RealFft::inversePacked(const float* packed, float* out) noexcept {
    pffft_transform(impl_->setup, packed, out, impl_->work.data(), PFFFT_BACKWARD);
}

void RealFft::multiplyAccumulatePacked(const float* a, const float* b, float* acc, float scale) const noexcept {
    pffft_zconvolve_accumulate(impl_->setup, a, b, acc, scale);
}

}  // namespace amq::dsp
