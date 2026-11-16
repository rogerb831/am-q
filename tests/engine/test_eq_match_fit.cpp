// EQ Match fitter (area 16 R3-R10, R16-R18; area 16 task "synthetic-curve regression suite and the fuzz
// harness for the fitter").
#include "amq/engine/eq_match_fit.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

using namespace amq;
using engine::FittedBand;
using engine::MatchArray;

namespace {

MatchArray curveOf(const std::vector<FittedBand>& bands, double fs = 48000.0) {
    MatchArray sum{}, r{};
    for (const auto& b : bands) {
        engine::bandResponseDb(b, fs, r);
        for (size_t i = 0; i < sum.size(); ++i) sum[i] += r[i];
    }
    return sum;
}

MatchArray ones() {
    MatchArray w;
    w.fill(1.0);
    return w;
}

}  // namespace

TEST_CASE("Fitter recovers a curve made of known bands (synthetic regression)") {
    const std::vector<FittedBand> truth = {{dsp::Shape::Bell, dsp::Slope::Db12, 1000.0, 6.0, 2.0},
                                           {dsp::Shape::LowShelf, dsp::Slope::Db12, 120.0, -4.0, 0.7071},
                                           {dsp::Shape::Bell, dsp::Slope::Db12, 6000.0, -3.0, 1.0}};
    const auto target = curveOf(truth);
    engine::FitSettings s;
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = engine::fitMatch(target, ones(), s);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  fit: %zu bands, error %.4f -> %.6f dB^2 in %.2f s\n", r.bands.size(), r.initialError, r.finalError, secs);
    CHECK(r.finalError < 0.01 * r.initialError);
    CHECK(r.finalError < 0.02);
    // The true bands are found: a bell near 1 kHz at about +6 dB and a low shelf near 120 Hz at about
    // -4 dB (the shelf covers more of the log grid, so it removes the most error).
    REQUIRE_FALSE(r.bands.empty());
    auto has = [&](dsp::Shape s, double hz, double g) {
        return std::any_of(r.bands.begin(), r.bands.end(), [&](const FittedBand& b) {
            return b.shape == s && std::abs(std::log2(b.hz / hz)) < 0.15 && std::abs(b.gainDb - g) < 0.75;
        });
    };
    CHECK(has(dsp::Shape::Bell, 1000.0, 6.0));
    CHECK(has(dsp::Shape::LowShelf, 120.0, -4.0));
    for (size_t i = 1; i < r.bands.size(); ++i) CHECK(r.bands[i].errorReduction <= r.bands[i - 1].errorReduction * 1.0001);
}

TEST_CASE("Fitter uses a cut for an edge deficit") {
    const auto target = curveOf({{dsp::Shape::LowCut, dsp::Slope::Db24, 90.0, 0.0, 0.7071}});
    MatchArray w = ones();
    const auto r = engine::fitMatch(target, w, {});
    REQUIRE_FALSE(r.bands.empty());
    CHECK(r.bands[0].shape == dsp::Shape::LowCut);
    CHECK(r.finalError < 0.05 * r.initialError);
}

TEST_CASE("Fitter: deterministic, in range, finite for random and hostile curves (fuzz)") {
    std::mt19937 rng(21);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (int trial = 0; trial < 6; ++trial) {
        MatchArray target{}, w = ones();
        double acc = 0.0;
        for (auto& v : target) acc = 0.97 * acc + 3.0 * u(rng), v = acc;  // random wiggles
        if (trial == 4) target.fill(1e9);                                  // hostile: clamped by nothing upstream
        if (trial == 5) target[100] = std::nan(""), w[200] = -1.0;
        engine::FitSettings s;
        s.maxBands = 6;
        const auto a = engine::fitMatch(target, w, s);
        const auto b = engine::fitMatch(target, w, s);
        REQUIRE(a.bands.size() == b.bands.size());
        for (size_t i = 0; i < a.bands.size(); ++i) {
            CHECK(a.bands[i].hz == b.bands[i].hz);  // deterministic (R17)
            CHECK(a.bands[i].gainDb == b.bands[i].gainDb);
            const auto& x = a.bands[i];
            CHECK(std::isfinite(x.hz));
            CHECK(std::isfinite(x.gainDb));
            CHECK(std::isfinite(x.q));
            CHECK(x.hz >= 10.0);
            CHECK(x.hz <= 30000.0);
            CHECK(std::abs(x.gainDb) <= 30.0);
            CHECK(x.q >= 0.025);
            CHECK(x.q <= 40.0);
            if (x.shape == dsp::Shape::LowShelf || x.shape == dsp::Shape::HighShelf || x.shape == dsp::Shape::LowCut ||
                x.shape == dsp::Shape::HighCut)
                CHECK(x.q <= 1.0);  // non-resonant (R9)
        }
        CHECK(std::isfinite(a.finalError));
        CHECK(a.bands.size() <= 6);
    }
}

TEST_CASE("Fitter: zero weight is ignored; cancel returns no bands") {
    auto target = curveOf({{dsp::Shape::Bell, dsp::Slope::Db12, 500.0, 8.0, 1.5}});
    MatchArray w = ones();
    for (int i = 0; i < 480; ++i)
        if (analysis::matchGrid()[static_cast<size_t>(i)] > 10000.0) w[static_cast<size_t>(i)] = 0.0, target[static_cast<size_t>(i)] = 25.0;
    const auto r = engine::fitMatch(target, w, {});
    REQUIRE_FALSE(r.bands.empty());
    CHECK(r.bands[0].hz < 5000.0);  // the unweighted top is not chased
    std::atomic<bool> cancel{true};
    std::atomic<float> progress{0.0f};
    const auto c = engine::fitMatch(target, w, {}, &progress, &cancel);
    CHECK(c.cancelled);
    CHECK(c.bands.empty());
}
