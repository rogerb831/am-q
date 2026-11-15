#pragma once
// Am-Q's FFT interface (area 01 R22). The backend is PFFFT (BSD-style, THIRD_PARTY.md), hidden
// behind this header so it can be replaced without touching callers.
//
// RealFft transforms n real samples. Valid sizes are n = 32 * 2^a * 3^b * 5^c with SIMD
// (32, 64, 96, 128, 160, 192, ...; validSize() asks the backend). Two domains:
//   - Ordered: n/2 + 1 complex bins, bin k at k * fs / n, DC and Nyquist with zero imaginary
//     part. forward() is unnormalised; inverse() scales by 1/n, so inverse(forward(x)) == x.
//   - Packed: n floats in the backend's internal order, for convolution. Only
//     multiplyAccumulatePacked() may combine packed spectra. inversePacked() is unnormalised.
//     Packed buffers must come from FftBuffer (SIMD alignment).
//
// Construction allocates (never on the audio thread). After that every call is real-time
// safe. One instance is used by one thread at a time (it owns scratch buffers).

#include <complex>
#include <cstddef>
#include <memory>

namespace amq::dsp {

// SIMD-aligned float buffer for packed spectra and convolver state. Allocates on construction.
class FftBuffer {
public:
    FftBuffer() = default;
    explicit FftBuffer(size_t n);
    ~FftBuffer();
    FftBuffer(FftBuffer&& o) noexcept;
    FftBuffer& operator=(FftBuffer&& o) noexcept;
    FftBuffer(const FftBuffer&) = delete;
    FftBuffer& operator=(const FftBuffer&) = delete;

    [[nodiscard]] float* data() noexcept { return p_; }
    [[nodiscard]] const float* data() const noexcept { return p_; }
    [[nodiscard]] size_t size() const noexcept { return n_; }
    void clear() noexcept;

private:
    float* p_ = nullptr;
    size_t n_ = 0;
};

class RealFft {
public:
    [[nodiscard]] static bool validSize(int n) noexcept;

    explicit RealFft(int n);  // throws std::invalid_argument for an invalid size
    ~RealFft();
    RealFft(const RealFft&) = delete;
    RealFft& operator=(const RealFft&) = delete;

    [[nodiscard]] int size() const noexcept { return n_; }

    // Ordered domain. Any pointers (copied through aligned scratch).
    void forward(const float* in, std::complex<float>* out) noexcept;
    void inverse(const std::complex<float>* in, float* out) noexcept;

    // Packed domain. Aligned pointers (FftBuffer::data()), n floats each; in and out may alias.
    void forwardPacked(const float* in, float* packed) noexcept;
    void inversePacked(const float* packed, float* out) noexcept;
    // acc += a * b * scale, bin by bin. Any of the three may alias.
    void multiplyAccumulatePacked(const float* a, const float* b, float* acc, float scale) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int n_ = 0;
};

}  // namespace amq::dsp
