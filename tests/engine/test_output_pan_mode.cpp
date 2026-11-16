// Output Pan Mode (area 04 R1-R5, R8 and its acceptance tests): the L/R and M/S balance laws,
// Level and Invert alike in both modes, the 20 ms crossfade between the two output graphs, and
// no pan on mono layouts.
#include "amq/engine/engine.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace amq;
using params::GlobalParam;
using params::ParamSource;

namespace {

constexpr double kFs = 48000.0;

const params::ParamIndex kPan = params::globalParam(GlobalParam::OutputPan);
const params::ParamIndex kPanMode = params::globalParam(GlobalParam::PanMode);
const params::ParamIndex kLevel = params::globalParam(GlobalParam::OutputLevel);
const params::ParamIndex kInvert = params::globalParam(GlobalParam::Invert);

struct Io {
    std::vector<float> l, r;
};

Io noise(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    Io x{std::vector<float>(n), std::vector<float>(n)};
    for (size_t i = 0; i < n; ++i) x.l[i] = u(rng), x.r[i] = u(rng);
    return x;
}

// Renders in 64-sample pieces; `at(sample)` may change the table before a piece.
template <typename F>
Io render(engine::Engine& e, const Io& x, F&& at, uint32_t channels = 2) {
    Io y{std::vector<float>(x.l.size()), std::vector<float>(x.l.size())};
    for (size_t p = 0; p < x.l.size(); p += 64) {
        at(p);
        const float* ip[2] = {x.l.data() + p, x.r.data() + p};
        float* op[2] = {y.l.data() + p, y.r.data() + p};
        e.process(ip, channels, op, channels, 64);
    }
    return y;
}
Io render(engine::Engine& e, const Io& x) {
    return render(e, x, [](size_t) {});
}

// Renders x through a fresh engine with the given pan, mode, level and invert.
Io run(const Io& x, double pan, bool ms, double levelDb = 0.0, bool invert = false) {
    params::ParameterTable t;
    t.set(kPan, pan, ParamSource::Preset);
    t.set(kPanMode, ms ? 1 : 0, ParamSource::Preset);
    t.set(kLevel, levelDb, ParamSource::Preset);
    t.set(kInvert, invert ? 1 : 0, ParamSource::Preset);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    return render(e, x);
}

double peak(const std::vector<float>& v) {
    double m = 0.0;
    for (float s : v) m = std::max(m, static_cast<double>(std::abs(s)));
    return m;
}

}  // namespace

TEST_CASE("L/R pan: full right leaves the left silent and the right at unity") {
    const auto x = noise(4096, 1);
    const auto y = run(x, 100.0, false);
    CHECK(peak(y.l) == 0.0);
    CHECK(y.r == x.r);
}

TEST_CASE("M/S pan right: lowers Mid. Full right silences L=R input and passes pure side at unity") {
    auto x = noise(4096, 2);
    x.r = x.l;  // L = R: Mid only
    const auto y = run(x, 100.0, true);
    CHECK(peak(y.l) == 0.0);
    CHECK(peak(y.r) == 0.0);

    for (size_t i = 0; i < x.l.size(); ++i) x.r[i] = -x.l[i];  // L = -R: Side only
    const auto s = run(x, 100.0, true);
    double worst = 0.0;
    for (size_t i = 0; i < x.l.size(); ++i)
        worst = std::max({worst, std::abs(double(s.l[i]) - x.l[i]), std::abs(double(s.r[i]) - x.r[i])});
    CHECK(worst < 1e-7);
}

TEST_CASE("M/S pan left: lowers Side. Full left makes wide input mono, at Mid level") {
    const auto x = noise(4096, 3);
    const auto y = run(x, -100.0, true);
    CHECK(y.l == y.r);
    double worst = 0.0;
    for (size_t i = 0; i < x.l.size(); ++i) worst = std::max(worst, std::abs(y.l[i] - 0.5 * (double(x.l[i]) + x.r[i])));
    CHECK(worst < 1e-7);
}

TEST_CASE("M/S pan at 50 %: the exact M/S balance matrix") {
    const auto x = noise(4096, 4);
    for (double pan : {-50.0, 50.0}) {
        const auto y = run(x, pan, true);
        const double gm = pan > 0 ? 0.5 : 1.0, gs = pan < 0 ? 0.5 : 1.0;
        double worst = 0.0;
        for (size_t i = 0; i < x.l.size(); ++i) {
            const double m = 0.5 * (double(x.l[i]) + x.r[i]), s = 0.5 * (double(x.l[i]) - x.r[i]);
            worst = std::max({worst, std::abs(y.l[i] - (gm * m + gs * s)), std::abs(y.r[i] - (gm * m - gs * s))});
        }
        INFO("pan " << pan);
        CHECK(worst < 1e-7);
    }
}

TEST_CASE("Level and Invert give the same result in both modes; centre pan is exact in M/S") {
    const auto x = noise(4096, 5);
    CHECK(run(x, 0.0, true).l == x.l);
    CHECK(run(x, 0.0, true).r == x.r);
    const auto a = run(x, 0.0, false, -7.5, true);
    const auto b = run(x, 0.0, true, -7.5, true);
    CHECK(a.l == b.l);
    CHECK(a.r == b.r);
}

TEST_CASE("Pan Mode switch at 50 % pan during noise: a 20 ms linear crossfade between the two graphs") {
    const auto x = noise(16384, 6);
    const auto lr = run(x, 50.0, false);
    const auto ms = run(x, 50.0, true);
    const size_t onAt = 4096, offAt = 10240, backAt = 12288 + 256;  // the last one reverses midway
    params::ParameterTable t;
    t.set(kPan, 50.0, ParamSource::Preset);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    const auto y = render(e, x, [&](size_t p) {
        if (p == onAt) t.set(kPanMode, 1, ParamSource::Host);
        if (p == offAt) t.set(kPanMode, 0, ParamSource::Host);
        if (p == 12288) t.set(kPanMode, 1, ParamSource::Host);
        if (p == backAt) t.set(kPanMode, 0, ParamSource::Host);
    });
    const double len = engine::kPanModeFadeMs * 1e-3 * kFs;  // 960 samples
    // The ideal mix: the ramp moves at the event's sample (LinearRamp::next) and reverses from
    // where it is.
    std::vector<double> a(x.l.size());
    double cur = 0.0, tgt = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (i == onAt || i == 12288) tgt = 1.0;
        if (i == offAt || i == backAt) tgt = 0.0;
        cur = tgt > cur ? std::min(tgt, cur + 1.0 / len) : std::max(tgt, cur - 1.0 / len);
        a[i] = cur;
    }
    double worst = 0.0;
    size_t first = 0, last = 0;  // first and last samples of the switch-on crossfade
    for (size_t i = 0; i < x.l.size(); ++i) {
        const double il = lr.l[i] + a[i] * (ms.l[i] - lr.l[i]), ir = lr.r[i] + a[i] * (ms.r[i] - lr.r[i]);
        worst = std::max({worst, std::abs(y.l[i] - il), std::abs(y.r[i] - ir)});
        if (i >= onAt && i < offAt) {
            if (!first && y.l[i] != lr.l[i]) first = i;
            if (std::abs(y.l[i] - ms.l[i]) > 1e-7 || std::abs(y.r[i] - ms.r[i]) > 1e-7) last = i;
        }
    }
    INFO("worst deviation from the ideal crossfade " << worst << ", crossfade samples " << first << ".." << last);
    // No discontinuity: within -120 dBFS of a continuous crossfade (criterion: -90 dBFS).
    CHECK(worst < 1e-6);
    // 20 ms +-5 %.
    const double measured = double(last - first + 2);  // the ramp reaches 1 one sample after `last`
    CHECK(measured >= 0.95 * len);
    CHECK(measured <= 1.05 * len);
    CHECK(e.panModeMix() == 0.0);
    CHECK(y.l.back() == lr.l.back());
}

TEST_CASE("Mono layouts: Pan and Pan Mode do nothing") {
    const auto x = noise(4096, 7);
    for (bool ms : {false, true}) {
        params::ParameterTable t;
        t.set(kPan, 100.0, ParamSource::Preset);
        t.set(kPanMode, ms ? 1 : 0, ParamSource::Preset);
        engine::Engine e(t);
        REQUIRE(e.prepare(kFs, 512));
        const auto y = render(e, x, [](size_t) {}, 1);
        CHECK(y.l == x.l);
    }
}
