// Structure changes and near-zero bands (area 01 R16, R17 and their acceptance tests): a slope or
// shape change crossfades old and new cascades over 100 ms with no discontinuity, and a gain
// shape at 0 dB fades to bypass and is bit-transparent.
#include "amq/engine/engine.hpp"
#include "amq/params/mappings.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using namespace amq;
using params::BandField;
using params::GlobalParam;
using params::ParamSource;

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

void band(params::ParameterTable& t, int b, dsp::Shape s, double hz, double db, double q, dsp::Slope slope) {
    t.set(params::bandParam(b, BandField::Used), 1, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Shape), static_cast<int>(s), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Freq), std::log2(hz), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Gain), db, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Q), params::positionFromQ(q), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Slope), static_cast<int>(slope), ParamSource::Preset);
}

struct Io {
    std::vector<float> l, r;
};

template <typename F>
Io render(engine::Engine& e, const Io& x, F&& at) {
    Io y{std::vector<float>(x.l.size()), std::vector<float>(x.l.size())};
    for (size_t p = 0; p < x.l.size(); p += 64) {
        at(p);
        const float* ip[2] = {x.l.data() + p, x.r.data() + p};
        float* op[2] = {y.l.data() + p, y.r.data() + p};
        e.process(ip, 2, op, 2, 64);
    }
    return y;
}
Io render(engine::Engine& e, const Io& x) {
    return render(e, x, [](size_t) {});
}

Io sine(size_t n, double hz, double amp) {
    Io x{std::vector<float>(n), std::vector<float>(n)};
    for (size_t i = 0; i < n; ++i) x.l[i] = x.r[i] = static_cast<float>(amp * std::sin(2.0 * kPi * hz * i / kFs));
    return x;
}

double maxStep(const std::vector<float>& v, size_t from, size_t to) {
    double m = 0.0;
    for (size_t i = std::max<size_t>(from, 1); i < to; ++i) m = std::max(m, std::abs(double(v[i]) - v[i - 1]));
    return m;
}

}  // namespace

TEST_CASE("Slope change while playing a sine: a 100 ms crossfade with no discontinuity") {
    // 100 Hz sine into a 200 Hz Low Cut: 12 dB/oct passes about -12 dB, 48 dB/oct about -48 dB.
    const size_t n = 24000, at = 6400;
    const auto x = sine(n, 100.0, 0.5);
    params::ParameterTable t;
    band(t, 0, dsp::Shape::LowCut, 200, 0, 0.7071, dsp::Slope::Db12);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    std::vector<double> mix;
    const auto y = render(e, x, [&](size_t p) {
        if (p == at) t.set(params::bandParam(0, BandField::Slope), static_cast<int>(dsp::Slope::Db48), ParamSource::Host);
        mix.push_back(e.structureMix(0));
    });
    CHECK(e.stats().structureFades == 1);
    // The sine itself moves at most 0.5 * 2 pi 100 / 48000 = 0.0065 per sample. A hard switch
    // jumps by about the old output level (0.12); the crossfade must stay near the sine's own.
    const double step = maxStep(y.l, at - 200, at + 6000);
    INFO("largest step around the change " << step);
    CHECK(step < 0.008);
    // Length: 100 ms = 4800 samples, +-5 %. Read the mix at piece starts.
    const size_t piece = at / 64;
    CHECK(mix[piece + 1] > 0.0);
    CHECK(mix[piece + 1] < 0.02);
    CHECK(mix[piece + 74] < 1.0);   // 74 pieces = 4736 samples: still fading (4800 * 0.95 = 4560)
    CHECK(mix[piece + 77] == 1.0);  // 77 pieces = 4928 samples: done (4800 * 1.05 = 5040)
    // After the crossfade the output settles to the 48 dB/oct filter.
    params::ParameterTable t48;
    band(t48, 0, dsp::Shape::LowCut, 200, 0, 0.7071, dsp::Slope::Db48);
    engine::Engine e48(t48);
    REQUIRE(e48.prepare(kFs, 512));
    const auto ref = render(e48, x);
    double tail = 0.0;
    for (size_t i = n - 4000; i < n; ++i) tail = std::max(tail, std::abs(double(y.l[i]) - ref.l[i]));
    CHECK(tail < 1e-4);
}

TEST_CASE("Shape change crossfades too; a change during a crossfade waits for it to end") {
    const size_t n = 24000;
    std::mt19937 rng(4);
    std::uniform_real_distribution<float> u(-0.3f, 0.3f);
    Io x{std::vector<float>(n), std::vector<float>(n)};
    for (size_t i = 0; i < n; ++i) x.l[i] = x.r[i] = u(rng);
    params::ParameterTable t;
    band(t, 0, dsp::Shape::Bell, 1000, 9, 2, dsp::Slope::Db12);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    std::vector<double> mix;
    render(e, x, [&](size_t p) {
        if (p == 3200) t.set(params::bandParam(0, BandField::Shape), static_cast<int>(dsp::Shape::HighShelf), ParamSource::Host);
        if (p == 4800) t.set(params::bandParam(0, BandField::Shape), static_cast<int>(dsp::Shape::LowShelf), ParamSource::Host);
        mix.push_back(e.structureMix(0));
    });
    CHECK(e.stats().structureFades == 2);
    // The second change starts once the first crossfade is over (3200 + 4800 = 8000).
    CHECK(mix[4800 / 64 + 10] < 1.0);   // first still running
    CHECK(mix[8000 / 64 + 2] < 0.1);    // second started right after it
    CHECK(mix.back() == 1.0);
}

TEST_CASE("Near-zero bands fade to bypass: a 0 dB Bell or shelf is bit-identical to no band") {
    const auto x = sine(8192, 440.0, 0.4);
    params::ParameterTable t0;
    engine::Engine e0(t0);
    REQUIRE(e0.prepare(kFs, 512));
    const auto ref = render(e0, x);
    for (auto s : {dsp::Shape::Bell, dsp::Shape::LowShelf, dsp::Shape::HighShelf, dsp::Shape::TiltShelf, dsp::Shape::FlatTilt})
        for (double g : {0.0, 0.0005, -0.0005}) {
            params::ParameterTable t;
            band(t, 0, s, 440, g, 1.0, dsp::Slope::Db12);
            engine::Engine e(t);
            REQUIRE(e.prepare(kFs, 512));
            const auto y = render(e, x);
            INFO("shape " << static_cast<int>(s) << " gain " << g);
            CHECK(y.l == ref.l);
            CHECK(y.r == ref.r);
        }
    // Gain Scale 0 % makes every gain shape near zero.
    params::ParameterTable t;
    band(t, 0, dsp::Shape::Bell, 440, 12, 1.0, dsp::Slope::Db12);
    t.set(params::globalParam(GlobalParam::GainScale), 0.0, ParamSource::Preset);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    CHECK(render(e, x).l == ref.l);
}

TEST_CASE("A gain moved to 0 dB fades out smoothly, and back up fades in") {
    const size_t n = 24000;
    const auto x = sine(n, 440.0, 0.4);
    params::ParameterTable t;
    band(t, 0, dsp::Shape::Bell, 440, 6, 1.0, dsp::Slope::Db12);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    params::ParameterTable t0;
    engine::Engine e0(t0);
    REQUIRE(e0.prepare(kFs, 512));
    const auto ref = render(e0, x);
    const auto y = render(e, x, [&](size_t p) {
        if (p == 4096) t.set(params::bandParam(0, BandField::Gain), 0.0, ParamSource::Host);
        if (p == 16384) t.set(params::bandParam(0, BandField::Gain), 6.0, ParamSource::Host);
    });
    // Bypassed in between: exactly the dry sine once the ramp and the fade are done.
    bool exact = true;
    for (size_t i = 10000; i < 16384; ++i) exact &= y.l[i] == ref.l[i];
    CHECK(exact);
    // Never a jump beyond the sine's own movement (0.4 * 2 pi 440 / 48000 = 0.023) times the boost.
    CHECK(maxStep(y.l, 1, n) < 0.05);
}
