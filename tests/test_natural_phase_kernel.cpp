// Natural Phase kernel builder (area 05 R1, R2, R6, R7).
#include "amq/dsp/fft.hpp"
#include "amq/dsp/natural_phase_kernel.hpp"
#include "amq/dsp/partitioned_convolver.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <complex>
#include <random>
#include <vector>

using namespace amq::dsp;
using K = NaturalPhaseKernelBuilder;

namespace {

constexpr double kPi = 3.14159265358979323846;

const K::Window kWindows[] = {K::Window::Rectangular, K::Window::Hann, K::Window::BlackmanHarris, K::Window::Kaiser};

// Kernel DTFT at w (rad/sample).
std::complex<double> dtft(const std::vector<float>& h, double w) {
    std::complex<double> s = 0.0;
    for (size_t n = 0; n < h.size(); ++n) s += static_cast<double>(h[n]) * std::polar(1.0, -w * static_cast<double>(n));
    return s;
}

}  // namespace

TEST_CASE("Kernel constants: 512 taps, 257 bins, delay 256, bins at k fs / 512 (R1, R2, R6)") {
    CHECK(K::kTaps == 512);
    CHECK(K::kBins == 257);
    CHECK(K::kCentre == 256);
    CHECK(K::binHz(1, 48000.0) == doctest::Approx(93.75));
    CHECK(K::binHz(256, 44100.0) == doctest::Approx(22050.0));
}

TEST_CASE("Windows peak at the centre tap and are symmetric about it (R7)") {
    for (auto w : kWindows) {
        K b(w);
        const auto& t = b.windowTable();
        CHECK(t[K::kCentre] == 1.0f);
        for (int m = 1; m < K::kCentre; ++m) {
            CHECK(t[static_cast<size_t>(K::kCentre - m)] == doctest::Approx(t[static_cast<size_t>(K::kCentre + m)]).epsilon(1e-6));
            CHECK(t[static_cast<size_t>(K::kCentre - m)] <= t[static_cast<size_t>(K::kCentre - m + 1)] + 1e-7f);
        }
    }
}

TEST_CASE("Flat correction gives a unit tap at 256 (kernel symmetry baseline)") {
    for (auto w : kWindows) {
        K b(w);
        std::vector<std::complex<double>> c(K::kBins, 1.0);
        std::vector<float> h(K::kTaps);
        b.build(c.data(), h.data());
        double off = 0.0;
        for (int n = 0; n < K::kTaps; ++n)
            if (n != K::kCentre) off = std::max(off, static_cast<double>(std::abs(h[static_cast<size_t>(n)])));
        CHECK(h[K::kCentre] == doctest::Approx(1.0f).epsilon(1e-6));
        CHECK(off < 1e-7);
    }
    std::vector<float> id(K::kTaps, 7.0f);
    K::identity(id.data());
    for (int n = 0; n < K::kTaps; ++n) CHECK(id[static_cast<size_t>(n)] == (n == K::kCentre ? 1.0f : 0.0f));
}

TEST_CASE("Rectangular window: the kernel hits the correction exactly at every bin centre (R6)") {
    // Frequency sampling: with no window, the 512-point DFT of the kernel is C[k] * (-1)^k.
    std::vector<std::complex<double>> c(K::kBins);
    for (int k = 0; k < K::kBins; ++k) {
        const double x = static_cast<double>(k) / (K::kBins - 1);
        c[static_cast<size_t>(k)] = std::polar(1.0 + 0.5 * std::sin(3.0 * x), 0.8 * std::sin(kPi * x));
    }
    K b(K::Window::Rectangular);
    std::vector<float> h(K::kTaps);
    b.build(c.data(), h.data());
    double err = 0.0;
    for (int k = 1; k < K::kBins - 1; ++k) {
        const auto got = dtft(h, 2.0 * kPi * k / K::kTaps) * std::polar(1.0, kPi * k);  // remove the 256-tap delay
        err = std::max(err, std::abs(got - c[static_cast<size_t>(k)]));
    }
    CHECK(err < 1e-5);
    // DC and Nyquist are taken as real (area 03 R15).
    CHECK(dtft(h, 0.0).real() == doctest::Approx(c[0].real()).epsilon(1e-5));
    CHECK(std::abs(dtft(h, kPi)) == doctest::Approx(std::abs(c.back().real())).epsilon(1e-5));
}

TEST_CASE("Natural Phase latency: kernel delay plus one partition = 320 samples (R2)") {
    K b;
    std::vector<std::complex<double>> c(K::kBins, 1.0);
    std::vector<float> h(K::kTaps);
    b.build(c.data(), h.data());
    PartitionedConvolver conv;
    REQUIRE(conv.prepare(PartitionedConvolver::kDefaultPartition, K::kTaps));
    REQUIRE(conv.setKernel(h.data(), K::kTaps));
    const int latency = K::kCentre + conv.latency();
    CHECK(latency == 320);
    std::vector<float> x(2048, 0.0f), y(2048);
    x[100] = 1.0f;
    conv.process(x.data(), y.data(), x.size());
    size_t at = 0;
    for (size_t i = 0; i < y.size(); ++i)
        if (std::abs(y[i]) > std::abs(y[at])) at = i;
    CHECK(static_cast<int>(at) == 100 + latency);
}

TEST_CASE("All bands flat: Natural Phase output equals the input delayed (kernel symmetry baseline)") {
    // Full-scale noise through the built flat kernel and the convolver, against the delayed input.
    PartitionedConvolver conv;
    REQUIRE(conv.prepare(PartitionedConvolver::kDefaultPartition, K::kTaps));
    std::vector<float> h(K::kTaps);
    K b;
    std::vector<std::complex<double>> c(K::kBins, 1.0);
    b.build(c.data(), h.data());
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<float> x(48000), y(x.size());
    for (auto& v : x) v = u(rng);
    auto nullDb = [&](const float* kernel) {
        REQUIRE(conv.setKernel(kernel, K::kTaps));
        conv.process(x.data(), y.data(), x.size());
        double err = 0.0;
        for (size_t i = 320; i < x.size(); ++i) err = std::max(err, static_cast<double>(std::abs(y[i] - x[i - 320])));
        return 20.0 * std::log10(std::max(err, 1e-30));
    };
    const double built = nullDb(h.data());
    std::vector<float> id(K::kTaps);
    K::identity(id.data());
    const double identity = nullDb(id.data());
    MESSAGE("flat kernel null through the float convolver: built " << built << " dBFS, identity " << identity << " dBFS");
    CHECK(built < -120.0);
    CHECK(identity < -120.0);
}
