#include "amq/dsp/natural_phase_kernel.hpp"

#include <algorithm>
#include <cmath>

namespace amq::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Modified Bessel function of the first kind, order 0 (power series; converges for all x).
double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 200; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

// Window value at x = (n - centre) / centre, x in [-1, 1). Periodic forms, peak 1 at x = 0.
double windowAt(NaturalPhaseKernelBuilder::Window w, double x, double beta) {
    using W = NaturalPhaseKernelBuilder::Window;
    switch (w) {
        case W::Rectangular:
            return 1.0;
        case W::Hann:
            return 0.5 + 0.5 * std::cos(kPi * x);
        case W::BlackmanHarris:  // 4-term, -92 dB sidelobes (Harris 1978)
            return 0.35875 + 0.48829 * std::cos(kPi * x) + 0.14128 * std::cos(2.0 * kPi * x) +
                   0.01168 * std::cos(3.0 * kPi * x);
        case W::Kaiser:
            return besselI0(beta * std::sqrt(std::max(0.0, 1.0 - x * x))) / besselI0(beta);
    }
    return 1.0;
}

}  // namespace

NaturalPhaseKernelBuilder::NaturalPhaseKernelBuilder(Window w, double kaiserBeta) : window_(w), fft_(kTaps) {
    for (int n = 0; n < kTaps; ++n) {
        const double x = static_cast<double>(n - kCentre) / static_cast<double>(kCentre);
        win_[static_cast<size_t>(n)] = static_cast<float>(windowAt(w, x, kaiserBeta));
    }
    win_[kCentre] = 1.0f;
}

void NaturalPhaseKernelBuilder::build(const std::complex<double>* correction, float* taps) noexcept {
    for (int k = 0; k < kBins; ++k) {
        std::complex<double> c = correction[k];
        if (k == 0 || k == kBins - 1) c = {c.real(), 0.0};  // DC and Nyquist must be real (area 03 R15)
        if (k & 1) c = -c;                                    // delay by kCentre = N/2 taps
        spec_[static_cast<size_t>(k)] = {static_cast<float>(c.real()), static_cast<float>(c.imag())};
    }
    fft_.inverse(spec_.data(), taps);
    for (int n = 0; n < kTaps; ++n) taps[n] *= win_[static_cast<size_t>(n)];
}

void NaturalPhaseKernelBuilder::identity(float* taps) noexcept {
    std::fill(taps, taps + kTaps, 0.0f);
    taps[kCentre] = 1.0f;
}

}  // namespace amq::dsp
