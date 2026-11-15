// FFT interface and partitioned convolver (area 01 R22, area 05 R2/R3).
#include "amq/dsp/fft.hpp"
#include "amq/dsp/partitioned_convolver.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <new>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

// Allocation counter for the real-time paths.
namespace {
thread_local bool gCountAllocs = false;
std::atomic<long> gAllocs{0};
}  // namespace
void* operator new(std::size_t n) {
    if (gCountAllocs) ++gAllocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace amq::dsp;

namespace {

std::vector<float> noise(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<float> v(n);
    for (auto& x : v) x = u(rng);
    return v;
}

std::vector<std::complex<double>> naiveDft(const std::vector<float>& x) {
    const size_t n = x.size();
    std::vector<std::complex<double>> X(n / 2 + 1);
    for (size_t k = 0; k <= n / 2; ++k) {
        std::complex<double> s = 0.0;
        for (size_t t = 0; t < n; ++t) s += static_cast<double>(x[t]) * std::polar(1.0, -2.0 * 3.141592653589793 * static_cast<double>(k * t % n) / static_cast<double>(n));
        X[k] = s;
    }
    return X;
}

}  // namespace

TEST_CASE("Valid FFT sizes follow the backend's rule") {
    for (int n : {32, 64, 96, 128, 160, 480, 512, 1024, 4096, 8192}) CHECK(RealFft::validSize(n));
    for (int n : {0, 1, 16, 24, 48, 100, 31, 7 * 64}) CHECK_FALSE(RealFft::validSize(n));
    CHECK_THROWS_AS(RealFft(100), std::invalid_argument);
}

TEST_CASE("Forward transform matches a direct DFT; inverse restores the input") {
    for (int n : {32, 96, 128, 480, 1024, 4096}) {
        RealFft f(n);
        const auto x = noise(static_cast<size_t>(n), static_cast<unsigned>(n));
        std::vector<std::complex<float>> X(static_cast<size_t>(n / 2 + 1));
        f.forward(x.data(), X.data());
        const auto ref = naiveDft(x);
        double err = 0.0, mag = 0.0;
        for (size_t k = 0; k < X.size(); ++k) {
            err = std::max(err, std::abs(std::complex<double>(X[k]) - ref[k]));
            mag = std::max(mag, std::abs(ref[k]));
        }
        CHECK(err / mag < 2e-6);
        CHECK(X[0].imag() == 0.0f);
        CHECK(X.back().imag() == 0.0f);
        std::vector<float> y(static_cast<size_t>(n));
        f.inverse(X.data(), y.data());
        double rt = 0.0;
        for (int i = 0; i < n; ++i) rt = std::max(rt, static_cast<double>(std::abs(y[static_cast<size_t>(i)] - x[static_cast<size_t>(i)])));
        CHECK(rt < 1e-5);
    }
}

TEST_CASE("Packed multiply-accumulate gives circular convolution") {
    const int n = 128;
    RealFft f(n);
    const auto a = noise(n, 1), b = noise(n, 2);
    FftBuffer ta(n), tb(n), A(n), B(n), acc(n);
    std::copy(a.begin(), a.end(), ta.data());
    std::copy(b.begin(), b.end(), tb.data());
    f.forwardPacked(ta.data(), A.data());
    f.forwardPacked(tb.data(), B.data());
    f.multiplyAccumulatePacked(A.data(), B.data(), acc.data(), 1.0f / n);
    f.inversePacked(acc.data(), acc.data());
    double err = 0.0;
    for (int i = 0; i < n; ++i) {
        double s = 0.0;
        for (int j = 0; j < n; ++j) s += static_cast<double>(a[static_cast<size_t>(j)]) * b[static_cast<size_t>((i - j + n) % n)];
        err = std::max(err, std::abs(s - acc.data()[i]));
    }
    CHECK(err < 1e-4);
}

TEST_CASE("Partitioned convolver equals direct convolution, delayed by one partition (area 05 R2/R3)") {
    std::mt19937 rng(5);
    std::uniform_int_distribution<int> blk(1, 700);
    for (int len : {1, 63, 64, 65, 512, 1000}) {
        PartitionedConvolver c;
        REQUIRE(c.prepare(PartitionedConvolver::kDefaultPartition, 1024));
        const auto h = noise(static_cast<size_t>(len), static_cast<unsigned>(len + 10));
        REQUIRE(c.setKernel(h.data(), len));
        CHECK(c.latency() == 64);
        const size_t total = 6000;
        const auto x = noise(total, 99);
        std::vector<float> y(total);
        for (size_t pos = 0; pos < total;) {  // random host block sizes
            const size_t n = std::min(total - pos, static_cast<size_t>(blk(rng)));
            c.process(x.data() + pos, y.data() + pos, n);
            pos += n;
        }
        double err = 0.0, peak = 0.0;
        for (size_t i = 0; i < total; ++i) {
            double ref = 0.0;
            if (i >= 64) {
                const size_t t = i - 64;
                for (int k = 0; k < len && static_cast<size_t>(k) <= t; ++k) ref += static_cast<double>(h[static_cast<size_t>(k)]) * x[t - static_cast<size_t>(k)];
            }
            err = std::max(err, std::abs(ref - y[i]));
            peak = std::max(peak, std::abs(ref));
        }
        MESSAGE("kernel " << len << " taps: max error " << err << " (peak " << peak << ")");
        CHECK(err / peak < 1e-5);
    }
}

TEST_CASE("Convolver impulse response: kernel appears exactly one partition late") {
    PartitionedConvolver c;
    REQUIRE(c.prepare(64, 512));
    std::vector<float> h(512, 0.0f);
    h[256] = 1.0f;  // the Natural Phase kernel's centre tap (area 05 R2): total 256 + 64
    REQUIRE(c.setKernel(h.data(), 512));
    std::vector<float> x(1024, 0.0f), y(1024);
    x[0] = 1.0f;
    c.process(x.data(), y.data(), x.size());
    size_t at = 0;
    for (size_t i = 0; i < y.size(); ++i)
        if (std::abs(y[i]) > std::abs(y[at])) at = i;
    CHECK(at == 320);
    CHECK(y[at] == doctest::Approx(1.0f).epsilon(1e-5));
    double rest = 0.0;
    for (size_t i = 0; i < y.size(); ++i)
        if (i != at) rest = std::max(rest, static_cast<double>(std::abs(y[i])));
    CHECK(rest < 1e-6);
}

TEST_CASE("Convolver process is real-time safe and reset clears the tail") {
    PartitionedConvolver c;
    REQUIRE(c.prepare(64, 512));
    const auto h = noise(512, 3);
    REQUIRE(c.setKernel(h.data(), 512));
    const auto x = noise(48000, 4);
    std::vector<float> y(x.size());
    gAllocs = 0;
    gCountAllocs = true;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t pos = 0; pos < x.size(); pos += 480) c.process(x.data() + pos, y.data() + pos, 480);
    const auto t1 = std::chrono::steady_clock::now();
    gCountAllocs = false;
    CHECK(gAllocs.load() == 0);
    MESSAGE("512-tap kernel, 1 s at 48 kHz, one channel: "
            << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms ("
            << std::chrono::duration<double>(t1 - t0).count() * 100.0 << " % of one core)");
    c.reset();
    std::vector<float> z(2000, 0.0f), out(2000);
    c.process(z.data(), out.data(), z.size());
    double tail = 0.0;
    for (float v : out) tail = std::max(tail, static_cast<double>(std::abs(v)));
    CHECK(tail == 0.0);
    // Invalid setup is refused, not crashed on.
    PartitionedConvolver bad;
    CHECK_FALSE(bad.prepare(50, 512));  // 100 is not a valid FFT size
    CHECK_FALSE(c.setKernel(h.data(), 4096));  // longer than prepared
}

TEST_CASE("Kernel swap crossfades linearly from the old kernel to the new one (area 05 R8)") {
    // Old kernel: gain a at tap 0. New kernel: gain b at tap 0. y[n] = k(n) * x[n - 64].
    const float a = 1.0f, bGain = -0.5f;
    PartitionedConvolver c;
    REQUIRE(c.prepare(64, 512, 256));
    CHECK(c.fadeSamples() == 256);
    REQUIRE(c.setKernel(&a, 1));
    const auto x = noise(4096, 11);
    std::vector<float> y(x.size());
    const size_t submitAt = 1024;  // a partition boundary
    c.process(x.data(), y.data(), submitAt);
    gAllocs = 0;
    gCountAllocs = true;
    CHECK(c.submitKernel(&bGain, 1));
    CHECK(c.swapInProgress());
    CHECK_FALSE(c.submitKernel(&a, 1));  // refused while pending
    for (size_t pos = submitAt; pos < x.size(); pos += 37) c.process(x.data() + pos, y.data() + pos, std::min<size_t>(37, x.size() - pos));
    gCountAllocs = false;
    CHECK(gAllocs.load() == 0);
    CHECK_FALSE(c.swapInProgress());
    double err = 0.0;
    for (size_t n = 64; n < x.size(); ++n) {
        const size_t start = submitAt + 64;  // first output of the partition computed after the submit
        double g = n < start ? 0.0 : std::min(1.0, static_cast<double>(n - start + 1) / 256.0);
        const double k = a + g * (bGain - a);
        err = std::max(err, std::abs(k * x[n - 64] - y[n]));
    }
    CHECK(err < 1e-5);
    // The writer may submit again once the fade is done.
    CHECK(c.submitKernel(&a, 1));
}

TEST_CASE("Kernel swap with no fade switches at the partition boundary") {
    const float a = 1.0f, bGain = 2.0f;
    PartitionedConvolver c;
    REQUIRE(c.prepare(64, 64, 0));
    REQUIRE(c.setKernel(&a, 1));
    std::vector<float> x(512, 1.0f), y(512);
    c.process(x.data(), y.data(), 128);
    REQUIRE(c.submitKernel(&bGain, 1));
    c.process(x.data() + 128, y.data() + 128, 384);
    CHECK(y[191] == doctest::Approx(1.0f));
    CHECK(y[192] == doctest::Approx(2.0f));
    CHECK_FALSE(c.swapInProgress());
}

TEST_CASE("Kernel swaps from a writer thread while audio runs (area 05 R8)") {
    // Every kernel is a scaled unit tap with gain in [0.5, 1], so the output is always a blend of
    // two such gains times the delayed input: |y| <= |x[n - 64]| and finite.
    PartitionedConvolver c;
    REQUIRE(c.prepare(64, 512, 256));
    std::vector<float> h(512, 0.0f);
    h[0] = 1.0f;
    REQUIRE(c.setKernel(h.data(), 512));
    std::atomic<bool> stop{false};
    std::atomic<int> accepted{0};
    std::thread writer([&] {
        std::vector<float> k(512, 0.0f);
        int i = 0;
        while (!stop.load()) {
            k[0] = 0.5f + 0.5f * static_cast<float>(i % 7) / 6.0f;
            if (c.submitKernel(k.data(), 512)) { ++accepted; ++i; }
            std::this_thread::yield();
        }
    });
    const auto x = noise(48000 * 4, 12);
    std::vector<float> y(x.size());
    for (size_t pos = 0; pos < x.size(); pos += 128) c.process(x.data() + pos, y.data() + pos, 128);
    stop = true;
    writer.join();
    MESSAGE(accepted.load() << " kernels swapped in 4 s of audio");
    CHECK(accepted.load() > 100);
    double worst = 0.0;
    for (size_t n = 64; n < x.size(); ++n) {
        REQUIRE(std::isfinite(y[n]));
        worst = std::max(worst, static_cast<double>(std::abs(y[n])) - std::abs(x[n - 64]));
    }
    CHECK(worst < 1e-5);
}
