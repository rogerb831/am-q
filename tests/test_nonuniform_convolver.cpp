// Non-uniformly partitioned convolution (area 05 R4): equals direct convolution delayed by the
// head partition, for long kernels and random host block sizes; the segment plan covers the kernel
// with valid delays; a submitted kernel takes over completely once the swap is done.
#include "amq/dsp/nonuniform_convolver.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace amq::dsp;

TEST_CASE("Segment plan: covers the kernel, partitions grow by 4 up to the cap, delays are valid") {
    for (int L : {100, 4096, 8192, 131072, 1048576}) {
        const auto p = NonUniformConvolver::plan(64, 4096, L);
        REQUIRE_FALSE(p.empty());
        int at = 0;
        for (size_t i = 0; i < p.size(); ++i) {
            CHECK(p[i].start == at);
            CHECK(p[i].delay == p[i].start + 64 - p[i].partition);
            CHECK(p[i].delay >= 0);
            if (i > 0) CHECK(p[i].partition == std::min(p[i - 1].partition * 4, 4096));
            at += p[i].length;
        }
        CHECK(at == L);
    }
}

TEST_CASE("Non-uniform convolution equals direct convolution delayed by the head partition") {
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (int L : {300, 5000, 20000}) {
        std::vector<float> h(static_cast<size_t>(L));
        for (size_t k = 0; k < h.size(); ++k) h[k] = u(rng) * std::exp(-3.0f * float(k) / float(L));
        NonUniformConvolver c;
        REQUIRE(c.prepare(64, 1024, L));
        REQUIRE(c.setKernel(h.data(), L));
        const size_t n = static_cast<size_t>(L) + 6000;
        std::vector<float> x(n), y(n);
        for (auto& v : x) v = u(rng);
        std::uniform_int_distribution<int> blk(1, 700);
        for (size_t at = 0; at < n;) {
            const size_t m = std::min<size_t>(static_cast<size_t>(blk(rng)), n - at);
            c.process(x.data() + at, y.data() + at, m);
            at += m;
        }
        double worst = 0.0, peak = 0.0;
        for (size_t t = 64; t < n; ++t) {
            double ref = 0.0;
            const size_t tt = t - 64;
            for (size_t k = 0; k <= tt && k < h.size(); ++k) ref += double(h[k]) * x[tt - k];
            worst = std::max(worst, std::abs(ref - y[t]));
            peak = std::max(peak, std::abs(ref));
        }
        INFO("kernel " << L << ": worst " << worst << " of peak " << peak);
        CHECK(worst < 2e-5 * peak);
        for (size_t t = 0; t < 64; ++t) CHECK(y[t] == 0.0f);
    }
}

TEST_CASE("Submitted kernel: once the swap is done the output is the new convolution") {
    const int L = 6000;
    std::vector<float> a(L, 0.0f), b(L, 0.0f);
    a[0] = 1.0f;          // identity
    b[L - 1] = 0.5f;      // a late echo, in the last segment
    NonUniformConvolver c;
    REQUIRE(c.prepare(64, 1024, L));
    REQUIRE(c.setKernel(a.data(), L));
    std::vector<float> x(64), y(64);
    std::mt19937 rng(2);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<float> hist;
    REQUIRE(c.submitKernel(b.data(), L));
    CHECK(c.swapInProgress());
    int blocks = 0;
    while (c.swapInProgress() && blocks < 1000) {
        for (auto& v : x) v = u(rng);
        hist.insert(hist.end(), x.begin(), x.end());
        c.process(x.data(), y.data(), 64);
        ++blocks;
    }
    CHECK(blocks * 64 <= 1024 + 64);  // done within the largest partition
    // From here on: y[t] = 0.5 x[t - 64 - (L - 1)].
    double worst = 0.0;
    for (int k = 0; k < 200; ++k) {
        for (auto& v : x) v = u(rng);
        hist.insert(hist.end(), x.begin(), x.end());
        c.process(x.data(), y.data(), 64);
        const size_t t0 = hist.size() - 64;
        for (size_t i = 0; i < 64; ++i) {
            const size_t t = t0 + i;
            const double ref = t >= size_t(64 + L - 1) ? 0.5 * hist[t - 64 - (L - 1)] : 0.0;
            if (k > 100) worst = std::max(worst, std::abs(ref - y[i]));
        }
    }
    CHECK(worst < 1e-5);
}
