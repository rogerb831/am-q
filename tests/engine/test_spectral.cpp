// Spectral dynamics (area 07 acceptance rows; area 02 R9, R12): latency and reconstruction, the
// restart when the spectral path switches on, range at the band centre, selectivity between tones an
// octave apart, M/S, silence with Auto, the Density grid, and Density sweeps.
#include "amq/dsp/spectral_grid.hpp"
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

void spectralBell(params::ParameterTable& t, int b, double hz, double q, double range, bool autoThr,
                  double threshold, double density = 50.0, int placement = 0) {
    t.set(params::bandParam(b, BandField::Used), 1, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Shape), static_cast<int>(dsp::Shape::Bell), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Freq), std::log2(hz), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Gain), 0.0, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Q), params::positionFromQ(q), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Placement), placement, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::DynEnabled), 1, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::DynRange), range, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::DynAuto), autoThr ? 1 : 0, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Threshold), threshold, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Spectral), 1, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::SpectralDensity), density, ParamSource::Preset);
}

struct Io {
    std::vector<float> l, r;
};

template <typename F>
Io render(engine::Engine& e, const Io& x, F&& at) {
    Io y{std::vector<float>(x.l.size()), std::vector<float>(x.l.size())};
    for (size_t p = 0; p < x.l.size(); p += 64) {
        at(p);
        const auto m = static_cast<uint32_t>(std::min<size_t>(64, x.l.size() - p));
        const float* ip[2] = {x.l.data() + p, x.r.data() + p};
        float* op[2] = {y.l.data() + p, y.r.data() + p};
        e.process(ip, 2, op, 2, m);
    }
    return y;
}
Io render(engine::Engine& e, const Io& x) {
    return render(e, x, [](size_t) {});
}

Io tones(size_t n, double f1, double a1, double f2, double a2, double sideSign = 1.0) {
    Io x{std::vector<float>(n), std::vector<float>(n)};
    for (size_t i = 0; i < n; ++i) {
        const double v = a1 * std::sin(2 * kPi * f1 * i / kFs) + a2 * std::sin(2 * kPi * f2 * i / kFs);
        x.l[i] = static_cast<float>(v), x.r[i] = static_cast<float>(sideSign * v);
    }
    return x;
}

// Amplitude of a tone at hz over the last `len` samples (Hann-windowed DTFT, normalised).
double toneAmp(const std::vector<float>& y, double hz, size_t len) {
    std::complex<double> s = 0.0;
    double wsum = 0.0;
    const size_t start = y.size() - len;
    for (size_t i = 0; i < len; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * kPi * i / len);
        s += w * double(y[start + i]) * std::polar(1.0, -2 * kPi * hz * double(start + i) / kFs);
        wsum += w;
    }
    return 2.0 * std::abs(s) / wsum;
}

}  // namespace

TEST_CASE("Spectral path: reported latency, and unity gains reconstruct the input exactly that late") {
    for (auto mode : {dsp::ProcessingMode::ZeroLatency, dsp::ProcessingMode::NaturalPhase})
        for (int res : {0, 1, 3}) {
            params::ParameterTable t;
            spectralBell(t, 0, 1000, 1.0, -12.0, false, 0.0);  // threshold at the top: no reduction
            t.set(params::globalParam(GlobalParam::Mode), static_cast<int>(mode), ParamSource::Preset);
            t.set(params::globalParam(GlobalParam::Resolution), res, ParamSource::Preset);
            engine::Engine e(t);
            REQUIRE(e.prepare(kFs, 512));
            REQUIRE(e.activeSpectral());
            const uint32_t lat = e.latencySamples();
            CHECK(lat == dsp::latencySamples(mode, res, kFs, true));
            Io x{std::vector<float>(lat + 20000), std::vector<float>(lat + 20000)};
            x.l[0] = x.r[0] = 0.5f;
            const auto y = render(e, x);
            size_t peak = 0;
            for (size_t i = 0; i < y.l.size(); ++i)
                if (std::abs(y.l[i]) > std::abs(y.l[peak])) peak = i;
            INFO("mode " << static_cast<int>(mode) << " resolution " << res << " latency " << lat);
            CHECK(peak == lat);
            CHECK(y.l[peak] == doctest::Approx(0.5).epsilon(1e-4));
        }
}

TEST_CASE("Switching a band to Spectral asks for a restart; the next activation reports the FFT-path latency") {
    params::ParameterTable t;
    spectralBell(t, 0, 1000, 1.0, -12.0, true, -20.0);
    t.set(params::bandParam(0, BandField::Spectral), 0, ParamSource::Preset);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    CHECK_FALSE(e.activeSpectral());
    CHECK(e.latencySamples() == 0);
    t.set(params::bandParam(0, BandField::Spectral), 1, ParamSource::Host);
    CHECK(e.requestedSpectral());
    CHECK(e.restartNeeded());
    REQUIRE(e.prepare(kFs, 512));
    CHECK(e.activeSpectral());
    CHECK(e.latencySamples() == dsp::fftPathLatency(1, kFs));
    CHECK_FALSE(e.restartNeeded());
    // Linear Phase never uses the spectral path.
    t.set(params::globalParam(GlobalParam::Mode), static_cast<int>(dsp::ProcessingMode::LinearPhase), ParamSource::Host);
    CHECK_FALSE(e.requestedSpectral());
}

TEST_CASE("Range at the band centre and selectivity: a tone an octave away changes by less than 0.5 dB") {
    params::ParameterTable t;
    spectralBell(t, 0, 1000, 0.4, -12.0, false, -30.0, 100.0);  // wide band covering both tones
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    REQUIRE(e.activeSpectral());
    const auto x = tones(96000, 1000, 0.5, 2000, 0.003);  // 1 kHz 24 dB over, 2 kHz 20 dB under
    const auto y = render(e, x);
    const double a1 = 20 * std::log10(toneAmp(y.l, 1000, 16384) / 0.5);
    const double a2 = 20 * std::log10(toneAmp(y.l, 2000, 16384) / 0.003);
    INFO("1 kHz changes " << a1 << " dB, 2 kHz changes " << a2 << " dB");
    CHECK(a1 == doctest::Approx(-12.0).epsilon(0.05));
    CHECK(std::abs(a2) < 0.5);
    CHECK(e.dynGainReductionDb(0) == doctest::Approx(12.0).epsilon(0.02));
}

TEST_CASE("Spectral M/S: a Mid band ignores a pure Side signal") {
    params::ParameterTable t;
    spectralBell(t, 0, 1000, 1.0, -12.0, false, -30.0, 50.0, 3);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    const auto side = tones(48000, 1000, 0.5, 2000, 0.0, -1.0);
    const auto y = render(e, side);
    CHECK(e.dynGainReductionDb(0) < 1e-3);
    CHECK(std::abs(20 * std::log10(toneAmp(y.l, 1000, 16384) / 0.5)) < 0.05);
}

TEST_CASE("Spectral Auto on silence and very low noise: no gain change, finite output") {
    params::ParameterTable t;
    spectralBell(t, 0, 1000, 1.0, -12.0, true, -20.0);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    Io x{std::vector<float>(48000), std::vector<float>(48000)};
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> u(-1e-6f, 1e-6f);
    for (size_t i = 24000; i < x.l.size(); ++i) x.l[i] = u(rng), x.r[i] = u(rng);
    const auto y = render(e, x);
    CHECK(e.dynGainReductionDb(0) < 1e-6);
    bool finite = true;
    for (float v : y.l) finite &= std::isfinite(v);
    CHECK(finite);
}

TEST_CASE("Density grid: widths follow the spec within 5 %, counts within bounds, finer with Density") {
    int prev = 0;
    for (double d : {0.0, 50.0, 100.0}) {
        dsp::SpectralGrid g;
        g.build(500.0, 2000.0, d, kFs / 4096.0);  // a 2-octave band at 1 kHz
        CHECK(g.count >= dsp::kSpectralMinBands);
        CHECK(g.count <= dsp::kSpectralMaxBands);
        CHECK(g.count > prev);
        prev = g.count;
        for (int i = 0; i + 1 < g.count; ++i) {
            const double step = std::log2(g.centreHz[static_cast<size_t>(i + 1)] / g.centreHz[static_cast<size_t>(i)]);
            CHECK(step == doctest::Approx(dsp::spectralWidthOctaves(d, g.centreHz[static_cast<size_t>(i)])).epsilon(0.05));
        }
        // Weights: a partition of unity between neighbours.
        int a, b;
        double wa;
        g.weights(1000.0, a, b, wa);
        CHECK(wa >= 0.0);
        CHECK(wa <= 1.0);
    }
    CHECK(dsp::spectralWidthOctaves(0, 1000) == doctest::Approx(0.25));
    CHECK(dsp::spectralWidthOctaves(100, 1000) == doctest::Approx(0.0625));
    CHECK(dsp::spectralWidthOctaves(50, 10000) < dsp::spectralWidthOctaves(50, 100));  // finer at high frequencies
    CHECK(dsp::spectralReleaseSeconds(50, 100) > dsp::spectralReleaseSeconds(50, 10000));  // longer at low frequencies
}

TEST_CASE("Sweeping Density and the band frequency during noise stays smooth and finite") {
    params::ParameterTable t;
    spectralBell(t, 0, 1000, 0.7, -9.0, true, -20.0);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    std::mt19937 rng(4);
    std::normal_distribution<float> nrm(0.0f, 0.1f);
    Io x{std::vector<float>(96000), std::vector<float>(96000)};
    for (size_t i = 0; i < x.l.size(); ++i) x.l[i] = x.r[i] = nrm(rng);
    const uint64_t before = e.spectralStage().gridRebuilds();
    const auto y = render(e, x, [&](size_t p) {
        t.set(params::bandParam(0, BandField::SpectralDensity), 50.0 + 50.0 * std::sin(p / 4000.0), ParamSource::Host);
        t.set(params::bandParam(0, BandField::Freq), std::log2(1000.0 * std::exp2(std::sin(p / 9000.0))), ParamSource::Host);
    });
    CHECK(e.spectralStage().gridRebuilds() > before + 10);
    bool finite = true;
    double peak = 0.0;
    for (float v : y.l) finite &= std::isfinite(v), peak = std::max(peak, double(std::abs(v)));
    CHECK(finite);
    CHECK(peak < 1.0);  // no blow-up
}
