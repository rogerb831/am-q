// Linear Phase mode (area 02 R8, R9, R11, R12; area 05 R4, R5, R8 and the "Linear Phase latency"
// acceptance row): exact latency per resolution and rate, a symmetric impulse response, the
// analog magnitude, the M/S kernels, kernel changes through the standby crossfade, and dynamics.
#include "amq/engine/band_response.hpp"
#include "amq/engine/engine.hpp"
#include "amq/params/mappings.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

using namespace amq;
using params::BandField;
using params::GlobalParam;
using params::ParamSource;

namespace {

constexpr double kPi = 3.14159265358979323846;

void band(params::ParameterTable& t, int b, dsp::Shape s, double hz, double db, double q, int placement = 0,
          dsp::Slope slope = dsp::Slope::Db12) {
    t.set(params::bandParam(b, BandField::Used), 1, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Shape), static_cast<int>(s), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Freq), std::log2(hz), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Gain), db, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Q), params::positionFromQ(q), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Placement), placement, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Slope), static_cast<int>(slope), ParamSource::Preset);
}
void linear(params::ParameterTable& t, int resolution) {
    t.set(params::globalParam(GlobalParam::Mode), static_cast<int>(dsp::ProcessingMode::LinearPhase), ParamSource::Preset);
    t.set(params::globalParam(GlobalParam::Resolution), resolution, ParamSource::Preset);
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
        e.process(ip, 2, op, 2, static_cast<uint32_t>(std::min<size_t>(64, x.l.size() - p)));
    }
    return y;
}
Io render(engine::Engine& e, const Io& x) {
    return render(e, x, [](size_t) {});
}
Io impulse(size_t n, float l = 1.0f, float r = 1.0f) {
    Io x{std::vector<float>(n), std::vector<float>(n)};
    x.l[0] = l, x.r[0] = r;
    return x;
}
double dtftDb(const std::vector<float>& h, double hz, double fs) {
    std::complex<double> s = 0.0;
    const std::complex<double> w = std::polar(1.0, -2.0 * kPi * hz / fs);
    std::complex<double> z = 1.0;
    for (float v : h) s += double(v) * z, z *= w;
    return 20.0 * std::log10(std::abs(s));
}

}  // namespace

TEST_CASE("Linear Phase latency: an impulse comes out exactly FIR/2 + partition late, every resolution") {
    struct Case {
        double fs;
        int res;
    };
    for (Case c : {Case{48000, 0}, Case{48000, 1}, Case{48000, 2}, Case{48000, 3}, Case{48000, 4}, Case{44100, 1},
                   Case{96000, 0}, Case{192000, 0}}) {
        params::ParameterTable t;
        linear(t, c.res);
        engine::Engine e(t);
        REQUIRE(e.prepare(c.fs, 512));
        const uint32_t lat = e.latencySamples();
        CHECK(lat == dsp::latencySamples(dsp::ProcessingMode::LinearPhase, c.res, c.fs, false));
        const auto y = render(e, impulse(lat + 4096));
        size_t peak = 0;
        for (size_t i = 0; i < y.l.size(); ++i)
            if (std::abs(y.l[i]) > std::abs(y.l[peak])) peak = i;
        INFO("fs " << c.fs << " resolution " << c.res << " latency " << lat << " peak at " << peak);
        CHECK(peak == lat);
        CHECK(y.l[peak] == doctest::Approx(1.0).epsilon(1e-5));
        double rest = 0.0;
        for (size_t i = 0; i < y.l.size(); ++i)
            if (i != peak) rest = std::max(rest, double(std::abs(y.l[i])));
        CHECK(rest < 1e-5);
    }
}

TEST_CASE("Linear Phase: the impulse response is symmetric and matches the analog magnitude") {
    params::ParameterTable t;
    linear(t, 2);  // High: 16384 taps at 48 kHz
    band(t, 0, dsp::Shape::Bell, 1000, 9, 2);
    band(t, 1, dsp::Shape::HighShelf, 6000, -6, 0.7);
    band(t, 2, dsp::Shape::LowCut, 120, 0, 1, 0, dsp::Slope::Db24);
    engine::Engine e(t);
    REQUIRE(e.prepare(48000.0, 512));
    const uint32_t lat = e.latencySamples();
    const auto y = render(e, impulse(2 * lat + 1024));
    double asym = 0.0, peak = 0.0;
    for (uint32_t k = 1; k < 8192; ++k) {
        asym = std::max(asym, std::abs(double(y.l[lat + k]) - y.l[lat - k]));
        peak = std::max(peak, double(std::abs(y.l[lat + k])));
    }
    INFO("asymmetry " << asym << " of peak " << peak);
    CHECK(asym < 1e-5 * std::max(peak, 1.0));
    // Magnitude against the analog prototypes of the three bands.
    params::ParameterTable ta;
    band(ta, 0, dsp::Shape::Bell, 1000, 9, 2);
    band(ta, 1, dsp::Shape::HighShelf, 6000, -6, 0.7);
    band(ta, 2, dsp::Shape::LowCut, 120, 0, 1, 0, dsp::Slope::Db24);
    double worst = 0.0;
    for (int i = 0; i <= 200; ++i) {
        const double hz = 200.0 * std::pow(20000.0 / 200.0, i / 200.0);
        double target = 0.0;
        for (int b = 0; b < 3; ++b) {
            const auto p = dsp::designAnalogPrototype(engine::bandSpecFrom(engine::bandDesignInputs(ta, b, 48000.0)));
            target += 20.0 * std::log10(std::abs(p.evaluateHz(hz)));
        }
        worst = std::max(worst, std::abs(dtftDb(y.l, hz, 48000.0) - target));
    }
    INFO("worst magnitude error 200 Hz - 20 kHz: " << worst << " dB");
    CHECK(worst < 0.05);
}

TEST_CASE("Linear Phase M/S: a Mid band filters the Mid component only") {
    params::ParameterTable t;
    linear(t, 1);
    band(t, 0, dsp::Shape::Bell, 1000, 12, 1, 3);  // Mid
    engine::Engine e(t);
    REQUIRE(e.prepare(48000.0, 512));
    const uint32_t lat = e.latencySamples();
    const auto side = render(e, impulse(lat + 9000, 1.0f, -1.0f));  // pure Side: passes as a delay
    CHECK(side.l[lat] == doctest::Approx(1.0).epsilon(1e-4));
    CHECK(side.r[lat] == doctest::Approx(-1.0).epsilon(1e-4));
    engine::Engine e2(t);
    REQUIRE(e2.prepare(48000.0, 512));
    const auto mid = render(e2, impulse(lat + 9000));
    CHECK(dtftDb(mid.l, 1000, 48000.0) == doctest::Approx(12.0).epsilon(0.01));
    CHECK(mid.l == mid.r);
}

TEST_CASE("Linear Phase kernel change: built, swapped into the standby set, crossfaded") {
    params::ParameterTable t;
    linear(t, 0);
    band(t, 0, dsp::Shape::Bell, 1000, 0.5, 1);
    engine::Engine e(t);  // Manual design mode: the test thread runs the kernel jobs
    REQUIRE(e.prepare(48000.0, 512));
    const uint32_t lat = e.latencySamples();
    const size_t n = 48000;
    Io x{std::vector<float>(n), std::vector<float>(n)};
    for (size_t i = 0; i < n; ++i) x.l[i] = x.r[i] = static_cast<float>(0.25 * std::sin(2 * kPi * 1000 * i / 48000.0));
    const auto y = render(e, x, [&](size_t p) {
        if (p == 4096) t.set(params::bandParam(0, BandField::Gain), 12.0, ParamSource::Host);
        e.runKernelJobsForTests();
    });
    CHECK(e.kernelSwaps() >= 1);
    // At the end the 1 kHz sine is 12 dB up (the new kernel), smoothly reached.
    double amp = 0.0;
    for (size_t i = n - 2000; i < n; ++i) amp = std::max(amp, double(std::abs(y.l[i])));
    CHECK(20.0 * std::log10(amp / 0.25) == doctest::Approx(12.0).epsilon(0.01));
    double step = 0.0;
    for (size_t i = lat + 1; i < n; ++i) step = std::max(step, std::abs(double(y.l[i]) - y.l[i - 1]));
    CHECK(step < 0.25 * 4.0 * 2 * kPi * 1000 / 48000.0 * 1.1);  // never faster than the loudest sine
}

TEST_CASE("Linear Phase with a dynamic band: the range applies on top of the static FIR") {
    params::ParameterTable t;
    linear(t, 1);
    band(t, 0, dsp::Shape::Bell, 1000, 3, 1);
    t.set(params::bandParam(0, BandField::DynEnabled), 1, ParamSource::Preset);
    t.set(params::bandParam(0, BandField::DynRange), -9.0, ParamSource::Preset);
    t.set(params::bandParam(0, BandField::DynAuto), 0, ParamSource::Preset);
    t.set(params::bandParam(0, BandField::Threshold), -40.0, ParamSource::Preset);
    engine::Engine e(t);
    REQUIRE(e.prepare(48000.0, 512));
    const size_t n = 48000;
    Io x{std::vector<float>(n), std::vector<float>(n)};
    for (size_t i = 0; i < n; ++i) x.l[i] = x.r[i] = static_cast<float>(0.5 * std::sin(2 * kPi * 1000 * i / 48000.0));
    const auto y = render(e, x);
    double amp = 0.0;
    for (size_t i = n - 4000; i < n; ++i) amp = std::max(amp, double(std::abs(y.l[i])));
    CHECK(20.0 * std::log10(amp / 0.5) == doctest::Approx(3.0 - 9.0).epsilon(0.02));
}
