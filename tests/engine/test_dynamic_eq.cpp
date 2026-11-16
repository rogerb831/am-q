// Dynamic EQ in the engine (area 06 acceptance rows, area 07 auto threshold, area 08 R2, area 03 M/S
// side chain): range cap and sign, threshold, stereo link, placement and M/S detection, external
// side chain, auto threshold and its offset cap, silence, Auto timing restoring the knobs, the
// reference envelope's independence from the knobs, Side Chain Listen, and smoothness.
#include "amq/engine/engine.hpp"
#include "amq/params/mappings.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <random>
#include <vector>

using namespace amq;
using params::BandField;
using params::GlobalParam;
using params::ParamSource;

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

struct Io {
    std::vector<float> l, r;
};

void bell(params::ParameterTable& t, int b, double hz, double gain, double q, int placement = 0) {
    t.set(params::bandParam(b, BandField::Used), 1, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Shape), static_cast<int>(dsp::Shape::Bell), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Freq), std::log2(hz), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Gain), gain, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Q), params::positionFromQ(q), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Placement), placement, ParamSource::Preset);
}
void dyn(params::ParameterTable& t, int b, double range, bool autoThr, double threshold = -20.0) {
    t.set(params::bandParam(b, BandField::DynEnabled), 1, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::DynRange), range, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::DynAuto), autoThr ? 1 : 0, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Threshold), threshold, ParamSource::Preset);
}

Io sine(size_t n, double hz, double ampL, double ampR) {
    Io x{std::vector<float>(n), std::vector<float>(n)};
    for (size_t i = 0; i < n; ++i) {
        const double s = std::sin(2.0 * kPi * hz * double(i) / kFs);
        x.l[i] = static_cast<float>(ampL * s), x.r[i] = static_cast<float>(ampR * s);
    }
    return x;
}

template <typename F>
Io render(engine::Engine& e, const Io& x, F&& at, const Io* sc = nullptr) {
    Io y{std::vector<float>(x.l.size()), std::vector<float>(x.l.size())};
    for (size_t p = 0; p < x.l.size(); p += 64) {
        at(p);
        const float* ip[2] = {x.l.data() + p, x.r.data() + p};
        float* op[2] = {y.l.data() + p, y.r.data() + p};
        if (sc) {
            const float* sp[2] = {sc->l.data() + p, sc->r.data() + p};
            e.process(ip, 2, op, 2, 64, sp, 2);
        } else {
            e.process(ip, 2, op, 2, 64);
        }
    }
    return y;
}
Io render(engine::Engine& e, const Io& x, const Io* sc = nullptr) {
    return render(e, x, [](size_t) {}, sc);
}

// RMS gain in dB of y over x across the last `tail` samples.
double gainDb(const std::vector<float>& y, const std::vector<float>& x, size_t tail) {
    double sy = 0.0, sx = 0.0;
    for (size_t i = y.size() - tail; i < y.size(); ++i) sy += double(y[i]) * y[i], sx += double(x[i]) * x[i];
    return 10.0 * std::log10(sy / sx);
}

}  // namespace

TEST_CASE("Range cap and sign: 34 dB over threshold gives exactly |range|, cut or boost") {
    for (double range : {-12.0, 6.0}) {
        params::ParameterTable t;
        bell(t, 0, 1000, 0.0, 1.0);
        dyn(t, 0, range, false, -40.0);
        engine::Engine e(t);
        REQUIRE(e.prepare(kFs, 512));
        const auto x = sine(48000, 1000, 0.5, 0.5);  // -6 dBFS: 34 dB over
        const auto y = render(e, x);
        INFO("range " << range);
        CHECK(e.dynActive(0));
        CHECK(e.dynGainReductionDb(0) == doctest::Approx(std::abs(range)).epsilon(1e-3));
        CHECK(gainDb(y.l, x.l, 9600) == doctest::Approx(range).epsilon(0.02));
        CHECK(gainDb(y.r, x.r, 9600) == doctest::Approx(range).epsilon(0.02));
    }
}

TEST_CASE("Below the threshold nothing changes; a manual threshold at the top is just a threshold") {
    for (double thr : {-40.0, 0.0}) {
        params::ParameterTable t;
        bell(t, 0, 1000, 0.0, 1.0);
        dyn(t, 0, -12.0, false, thr);
        engine::Engine e(t);
        REQUIRE(e.prepare(kFs, 512));
        const double amp = thr == 0.0 ? 0.5 : 0.001;  // -6 dBFS under 0 dB; -60 dBFS under -40 dB
        const auto x = sine(24000, 1000, amp, amp);
        const auto y = render(e, x);
        INFO("threshold " << thr);
        CHECK(e.dynGainReductionDb(0) < 1e-3);
        CHECK(std::abs(gainDb(y.l, x.l, 9600)) < 0.01);
    }
}

TEST_CASE("Stereo link: a signal on one channel gives both channels the same gain") {
    params::ParameterTable t;
    bell(t, 0, 1000, 0.0, 1.0, 0);
    dyn(t, 0, -9.0, false, -30.0);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    const auto x = sine(48000, 1000, 0.5, 0.005);  // loud L, quiet R (below threshold alone)
    const auto y = render(e, x);
    const double gl = gainDb(y.l, x.l, 9600), gr = gainDb(y.r, x.r, 9600);
    CHECK(gl == doctest::Approx(-9.0).epsilon(0.02));
    CHECK(gr == doctest::Approx(gl).epsilon(1e-3));
}

TEST_CASE("Placement: a Left band acts on L only; a Mid band detects Mid, not Side") {
    {
        params::ParameterTable t;
        bell(t, 0, 1000, 0.0, 1.0, 1);
        dyn(t, 0, -9.0, false, -30.0);
        engine::Engine e(t);
        REQUIRE(e.prepare(kFs, 512));
        const auto x = sine(48000, 1000, 0.5, 0.5);
        const auto y = render(e, x);
        CHECK(gainDb(y.l, x.l, 9600) == doctest::Approx(-9.0).epsilon(0.02));
        CHECK(y.r == x.r);
    }
    {
        params::ParameterTable t;
        bell(t, 0, 1000, 0.0, 1.0, 3);  // Mid
        dyn(t, 0, -9.0, false, -30.0);
        engine::Engine e(t);
        REQUIRE(e.prepare(kFs, 512));
        const auto side = sine(48000, 1000, 0.5, -0.5);  // pure Side: the Mid detector sees nothing
        render(e, side);
        CHECK(e.dynGainReductionDb(0) < 1e-3);
        const auto mid = sine(48000, 1000, 0.5, 0.5);  // pure Mid
        render(e, mid);
        CHECK(e.dynGainReductionDb(0) == doctest::Approx(9.0).epsilon(1e-3));
    }
}

TEST_CASE("External side chain: the side-chain bus drives the band; without it nothing happens") {
    params::ParameterTable t;
    bell(t, 0, 1000, 0.0, 1.0);
    dyn(t, 0, -9.0, false, -30.0);
    t.set(params::bandParam(0, BandField::ScExternal), 1, ParamSource::Preset);
    const auto x = sine(48000, 1000, 0.01, 0.01);   // main input below threshold
    const auto key = sine(48000, 1000, 0.5, 0.5);   // loud key
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    const auto y = render(e, x, &key);
    CHECK(gainDb(y.l, x.l, 9600) == doctest::Approx(-9.0).epsilon(0.02));
    engine::Engine e2(t);
    REQUIRE(e2.prepare(kFs, 512));
    const auto loud = sine(48000, 1000, 0.5, 0.5);
    const auto y2 = render(e2, loud);  // no side-chain bus: the loud main input does not count
    CHECK(e2.dynGainReductionDb(0) < 1e-3);
    CHECK(std::abs(gainDb(y2.l, loud.l, 9600)) < 0.01);
}

TEST_CASE("Auto threshold: steady input sits at the reference, so the full range applies; silence does nothing") {
    params::ParameterTable t;
    bell(t, 0, 1000, 0.0, 1.0);
    dyn(t, 0, -6.0, true);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    const auto x = sine(96000, 1000, 0.1, 0.1);
    const auto y = render(e, x);
    CHECK(e.dynReferenceDb(0) == doctest::Approx(-20.0).epsilon(0.01));
    CHECK(gainDb(y.l, x.l, 9600) == doctest::Approx(-6.0).epsilon(0.05));
    // Silence, then -120 dBFS noise: no gain change, finite output.
    params::ParameterTable ts;
    bell(ts, 0, 1000, 0.0, 1.0);
    dyn(ts, 0, -12.0, true);
    engine::Engine es(ts);
    REQUIRE(es.prepare(kFs, 512));
    Io quiet{std::vector<float>(48000), std::vector<float>(48000)};
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(-1e-6f, 1e-6f);
    for (size_t i = 24000; i < quiet.l.size(); ++i) quiet.l[i] = u(rng), quiet.r[i] = u(rng);
    const auto yq = render(es, quiet);
    CHECK(es.dynGainReductionDb(0) < 1e-6);
    bool finite = true;
    for (float v : yq.l) finite &= std::isfinite(v);
    CHECK(finite);
}

TEST_CASE("Auto offset cap: Gain Scale cannot push the auto offset past 30 dB") {
    params::ParameterTable t;
    bell(t, 0, 1000, 0.0, 1.0);
    dyn(t, 0, -30.0, true);
    t.set(params::globalParam(GlobalParam::GainScale), 200.0, ParamSource::Preset);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    const auto x = sine(48000, 1000, 0.1, 0.1);
    render(e, x);
    CHECK(e.dynGainReductionDb(0) <= 30.0 + 1e-9);
}

TEST_CASE("Auto timing keeps the knobs: Attack 80 reads back, and timing follows it once Auto is off") {
    auto attackRise = [](bool autoOn) {
        params::ParameterTable t;
        bell(t, 0, 1000, 0.0, 1.0);
        dyn(t, 0, -20.0, false, -40.0);
        t.set(params::bandParam(0, BandField::Attack), 80.0, ParamSource::Preset);
        t.set(params::bandParam(0, BandField::DynAuto), autoOn ? 1 : 0, ParamSource::Preset);
        t.set(params::bandParam(0, BandField::DynAuto), 0, ParamSource::Preset);  // back off before the run
        if (autoOn) t.set(params::bandParam(0, BandField::DynAuto), 1, ParamSource::Preset);
        CHECK(t.value(params::bandParam(0, BandField::Attack)) == 80.0);
        engine::Engine e(t);
        e.prepare(kFs, 512);
        const auto x = sine(48000, 1000, 0.5, 0.5);
        size_t n = 0;
        render(e, x, [&](size_t p) {
            if (n == 0 && e.dynGainReductionDb(0) >= 0.9 * 20.0) n = p;
        });
        return n;
    };
    const size_t manual = attackRise(false), autoTiming = attackRise(true);
    CHECK(manual > 2 * autoTiming);  // knob 80 is slower than the knob-50 Auto timing
}

TEST_CASE("Reference envelope: sweeping Attack and Release leaves it untouched (area 08 R2)") {
    params::ParameterTable t;
    bell(t, 0, 1000, 0.0, 1.0);
    dyn(t, 0, -6.0, true);
    t.set(params::bandParam(0, BandField::DynAuto), 0, ParamSource::Preset);
    t.set(params::bandParam(0, BandField::DynAuto), 1, ParamSource::Preset);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    const auto x = sine(96000, 1000, 0.1, 0.1);
    double refAt = 0.0, worst = 0.0;
    render(e, x, [&](size_t p) {
        if (p == 48000) {
            refAt = e.dynReferenceDb(0);
            t.set(params::bandParam(0, BandField::DynAuto), 0, ParamSource::Host);  // knobs live
        }
        if (p > 48000) {
            t.set(params::bandParam(0, BandField::Attack), double(p % 100), ParamSource::Host);
            t.set(params::bandParam(0, BandField::Release), double((p / 64) % 100), ParamSource::Host);
            worst = std::max(worst, std::abs(e.dynReferenceDb(0) - refAt));
        }
    });
    INFO("reference moved by " << worst << " dB");
    CHECK(worst < 0.01);
}

TEST_CASE("Side Chain Listen plays the band's filtered side chain at unity, on the monitor path") {
    params::ParameterTable t;
    bell(t, 0, 1000, 6.0, 2.0);
    t.set(params::bandParam(0, BandField::ScListen), 1, ParamSource::Preset);
    t.set(params::globalParam(GlobalParam::SoloGain), 12.0, ParamSource::Preset);  // not for listen
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    Io imp{std::vector<float>(16384), std::vector<float>(16384)};
    imp.l[0] = imp.r[0] = 1.0f;
    const auto y = render(e, imp);
    CHECK(e.soloListening());
    std::complex<double> s = 0.0;
    for (size_t i = 0; i < y.l.size(); ++i) s += double(y.l[i]) * std::polar(1.0, -2.0 * kPi * 1000.0 * double(i) / kFs);
    CHECK(std::abs(s) == doctest::Approx(1.0).epsilon(1e-3));  // 0 dB band-pass peak at the band frequency
}

TEST_CASE("Moving Attack, Release, Threshold and Range during a sine stays smooth") {
    params::ParameterTable t;
    bell(t, 0, 1000, 0.0, 1.0);
    dyn(t, 0, -12.0, false, -30.0);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    const auto x = sine(96000, 220, 0.3, 0.3);
    const auto y = render(e, x, [&](size_t p) {
        const double ph = double(p) / 96000.0;
        t.set(params::bandParam(0, BandField::Attack), 100.0 * ph, ParamSource::Host);
        t.set(params::bandParam(0, BandField::Release), 100.0 * (1.0 - ph), ParamSource::Host);
        t.set(params::bandParam(0, BandField::Threshold), -40.0 + 30.0 * ph, ParamSource::Host);
        t.set(params::bandParam(0, BandField::DynRange), -12.0 + 18.0 * ph, ParamSource::Host);
    });
    double step = 0.0;  // the sine moves at most 0.3 * 2 pi 220 / 48000 = 0.0086 per sample
    for (size_t i = 1; i < y.l.size(); ++i) step = std::max(step, std::abs(double(y.l[i]) - y.l[i - 1]));
    CHECK(step < 0.012);
    bool finite = true;
    for (float v : y.l) finite &= std::isfinite(v);
    CHECK(finite);
}
