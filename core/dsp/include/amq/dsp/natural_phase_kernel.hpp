#pragma once
// Natural Phase kernel builder (area 05 R1, R6, R7): frequency-sampling FIR design.
//
//   1. Take the 257-bin half-spectrum correction C[k] at the FFT bin centres w_k = 2 pi k / 512
//      (R6: no pre-warp; the caller evaluates its targets at the true bin frequencies
//      k * fs / 512). DC and Nyquist are made real by taking the real part.
//   2. Add the analytic delay of 256 taps: C[k] * exp(-j pi k) = C[k] * (-1)^k.
//   3. Inverse FFT to 512 real taps. The correction's impulse response is now centred at
//      tap 256, so no explicit circular shift is needed after step 2.
//   4. Apply the window, centred on tap 256 (w[256] = 1, symmetric: w[256 - m] = w[256 + m]).
//
// The kernel's delay is exactly kCentre = 256 taps; the convolver adds its partition, so
// Natural Phase latency = kCentre + partition (R2), 320 at the default 64-sample partition.
//
// The correction itself (target / digital response, regularisation, Nyquist phase taper) is
// built by the caller (area 03 R14, R15). Construction allocates; build() does not, so it may
// run on a worker thread repeatedly (R8). One instance per thread.

#include "amq/dsp/fft.hpp"

#include <array>
#include <complex>

namespace amq::dsp {

class NaturalPhaseKernelBuilder {
public:
    static constexpr int kTaps = 512;              // R1, at every sample rate
    static constexpr int kBins = kTaps / 2 + 1;    // 257
    static constexpr int kCentre = kTaps / 2;      // analytic delay in taps (R2)

    // R7 candidates. The default is chosen by measurement (area 05 window evaluation).
    enum class Window { Rectangular, Hann, BlackmanHarris, Kaiser };
    static constexpr Window kDefaultWindow = Window::Hann;  // PROVISIONAL until the evaluation closes
    static constexpr double kDefaultKaiserBeta = 6.0;       // PROVISIONAL

    explicit NaturalPhaseKernelBuilder(Window w = kDefaultWindow, double kaiserBeta = kDefaultKaiserBeta);

    // Frequency of bin k in Hz (R6).
    [[nodiscard]] static double binHz(int k, double sampleRate) noexcept {
        return static_cast<double>(k) * sampleRate / static_cast<double>(kTaps);
    }

    // correction: kBins values. taps: kTaps outputs.
    void build(const std::complex<double>* correction, float* taps) noexcept;
    // The identity correction (all bands flat): a unit tap at kCentre, exactly.
    static void identity(float* taps) noexcept;

    [[nodiscard]] Window window() const noexcept { return window_; }
    [[nodiscard]] const std::array<float, kTaps>& windowTable() const noexcept { return win_; }

private:
    Window window_;
    RealFft fft_;
    std::array<std::complex<float>, kBins> spec_{};
    std::array<float, kTaps> win_{};
};

}  // namespace amq::dsp
