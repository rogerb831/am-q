// Solo audition (area 02 R14, R15; area 03 R13; area 04 R7): the audition filter designs, the
// acceptance test (band-pass centred within 1 % of the band frequency at several Q, Solo Gain
// only while soloed), the boost rule, the M/S decode of the monitor path, the crossfades, and
// how solo meets bypass and Natural Phase.
#include "amq/dsp/audition.hpp"
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

double magAt(const dsp::SvfParams& p, double hz, double fs) {
    return std::abs(dsp::svfResponse(p, std::polar(1.0, 2.0 * kPi * hz / fs)));
}

struct Io {
    std::vector<float> l, r;
};

void band(params::ParameterTable& t, int b, dsp::Shape s, double hz, double db, double q, int placement = 0,
          bool solo = false) {
    t.set(params::bandParam(b, BandField::Used), 1, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Shape), static_cast<int>(s), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Freq), std::log2(hz), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Gain), db, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Q), params::positionFromQ(q), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Placement), placement, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Solo), solo ? 1 : 0, ParamSource::Preset);
}
void solo(params::ParameterTable& t, int b, bool on, ParamSource src = ParamSource::Host) {
    t.set(params::bandParam(b, BandField::Solo), on ? 1 : 0, src);
}

template <typename F>
Io render(engine::Engine& e, const Io& x, F&& at, uint32_t ch = 2) {
    Io y{std::vector<float>(x.l.size()), std::vector<float>(x.l.size())};
    for (size_t p = 0; p < x.l.size(); p += 64) {
        at(p);
        const float* ip[2] = {x.l.data() + p, x.r.data() + p};
        float* op[2] = {y.l.data() + p, y.r.data() + p};
        e.process(ip, ch, op, ch, 64);
    }
    return y;
}
Io render(engine::Engine& e, const Io& x) {
    return render(e, x, [](size_t) {});
}

Io noise(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    Io x{std::vector<float>(n), std::vector<float>(n)};
    for (size_t i = 0; i < n; ++i) x.l[i] = u(rng), x.r[i] = u(rng);
    return x;
}
Io impulse(size_t n, size_t at = 0) {
    Io x{std::vector<float>(n), std::vector<float>(n)};
    x.l[at] = x.r[at] = 1.0f;
    return x;
}

// |DTFT| of h at hz.
double dtft(const std::vector<float>& h, double hz) {
    std::complex<double> s = 0.0;
    const std::complex<double> w = std::polar(1.0, -2.0 * kPi * hz / kFs);
    std::complex<double> z = 1.0;
    for (float v : h) s += double(v) * z, z *= w;
    return std::abs(s);
}

// Frequency of the largest |DTFT| between lo and hi, on a fine log grid refined twice.
double peakHz(const std::vector<float>& h, double lo, double hi) {
    double best = lo, bestMag = -1.0;
    for (int pass = 0; pass < 3; ++pass) {
        const int steps = 120;
        for (int i = 0; i <= steps; ++i) {
            const double hz = lo * std::pow(hi / lo, double(i) / steps);
            const double m = dtft(h, hz);
            if (m > bestMag) bestMag = m, best = hz;
        }
        const double span = std::pow(hi / lo, 2.0 / steps);
        lo = best / span, hi = best * span;
    }
    return best;
}

}  // namespace

TEST_CASE("Audition band-pass: 0 dB peak exactly at the band frequency, octave width kept by the pre-warp") {
    for (double fs : {44100.0, 48000.0, 96000.0})
        for (double hz : {60.0, 1000.0, 8000.0, 15000.0})
            for (double q : {0.3, 0.7, 2.0, 8.0}) {
                const auto p = dsp::auditionDesign(dsp::AuditionKind::BandPass, hz, q, fs);
                INFO("fs " << fs << " hz " << hz << " q " << q);
                CHECK(magAt(p, hz, fs) == doctest::Approx(1.0).epsilon(1e-12));
                CHECK(magAt(p, hz * 1.001, fs) < 1.0);
                CHECK(magAt(p, hz / 1.001, fs) < 1.0);
                // The -3 dB edges are 2^N apart, N the analog octave width (when below Nyquist).
                const double n = dsp::bandPassOctaves(q);
                const double target = 1.0 / std::sqrt(2.0);
                auto edge = [&](double a, double b) {  // |H| crosses target once between a and b
                    for (int i = 0; i < 200; ++i) {
                        const double m = 0.5 * (a + b);
                        ((magAt(p, m, fs) > target) == (magAt(p, a, fs) > target) ? a : b) = m;
                    }
                    return 0.5 * (a + b);
                };
                const double f1 = edge(1.0, hz);
                if (hz * std::exp2(n) < 0.45 * fs) {
                    const double f2 = edge(hz, 0.4999 * fs);
                    CHECK(std::log2(f2 / f1) == doctest::Approx(n).epsilon(1e-9));
                }
            }
}

TEST_CASE("Audition low-pass and high-pass: -3 dB at the band frequency, unity in the passband") {
    for (double hz : {80.0, 2000.0, 12000.0}) {
        const auto lp = dsp::auditionDesign(dsp::AuditionKind::LowPass, hz, 1.0, kFs);
        const auto hp = dsp::auditionDesign(dsp::AuditionKind::HighPass, hz, 1.0, kFs);
        CHECK(magAt(lp, hz, kFs) == doctest::Approx(1.0 / std::sqrt(2.0)).epsilon(1e-12));
        CHECK(magAt(hp, hz, kFs) == doctest::Approx(1.0 / std::sqrt(2.0)).epsilon(1e-12));
        CHECK(magAt(lp, 1.0, kFs) == doctest::Approx(1.0).epsilon(1e-6));
        CHECK(magAt(hp, 0.49 * kFs, kFs) == doctest::Approx(1.0).epsilon(1e-3));
    }
    CHECK(dsp::auditionKind(dsp::Shape::LowCut) == dsp::AuditionKind::LowPass);
    CHECK(dsp::auditionKind(dsp::Shape::HighShelf) == dsp::AuditionKind::HighPass);
    CHECK(dsp::auditionKind(dsp::Shape::Notch) == dsp::AuditionKind::BandPass);
    CHECK(dsp::auditionKind(dsp::Shape::FlatTilt) == dsp::AuditionKind::Pass);
}

TEST_CASE("Solo a Bell and a Notch at several Q: band-pass centred within 1 % of the band frequency") {
    for (auto shape : {dsp::Shape::Bell, dsp::Shape::Notch})
        for (double hz : {120.0, 1000.0, 9000.0})
            for (double q : {0.5, 1.0, 4.0, 16.0}) {
                params::ParameterTable t;
                band(t, 0, shape, hz, -6.0, q, 0, true);
                engine::Engine e(t);
                REQUIRE(e.prepare(kFs, 512));
                const auto y = render(e, impulse(16384));
                const double peak = peakHz(y.l, hz / 4.0, std::min(hz * 4.0, 0.45 * kFs));
                INFO("shape " << static_cast<int>(shape) << " hz " << hz << " q " << q << ": peak " << peak);
                CHECK(std::abs(peak / hz - 1.0) < 0.01);
                CHECK(dtft(y.l, hz) == doctest::Approx(1.0).epsilon(1e-3));  // cut Bell: 0 dB, no boost
                CHECK(y.l == y.r);
            }
}

TEST_CASE("Solo Gain applies only while a band is soloed; a boosted Bell includes its boost") {
    const auto x = noise(8192, 1);
    params::ParameterTable t0;
    band(t0, 0, dsp::Shape::Bell, 1000, 6.0, 1.0);
    engine::Engine e0(t0);
    REQUIRE(e0.prepare(kFs, 512));
    const auto ref = render(e0, x);

    params::ParameterTable t;
    band(t, 0, dsp::Shape::Bell, 1000, 6.0, 1.0);
    t.set(params::globalParam(GlobalParam::SoloGain), 12.0, ParamSource::Preset);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    const auto y = render(e, x);
    CHECK(y.l == ref.l);  // not soloed: Solo Gain does nothing
    CHECK(y.r == ref.r);

    for (double gain : {9.0, -9.0})
        for (double soloGain : {0.0, -6.0, 6.0}) {
            params::ParameterTable ts;
            band(ts, 0, dsp::Shape::Bell, 2000, gain, 2.0, 0, true);
            ts.set(params::globalParam(GlobalParam::SoloGain), soloGain, ParamSource::Preset);
            engine::Engine es(ts);
            REQUIRE(es.prepare(kFs, 512));
            const auto h = render(es, impulse(16384));
            const double expectDb = soloGain + std::max(gain, 0.0);
            INFO("gain " << gain << ", Solo Gain " << soloGain);
            CHECK(20.0 * std::log10(dtft(h.l, 2000)) == doctest::Approx(expectDb).epsilon(1e-4));
        }
}

TEST_CASE("Gain Scale scales the boost heard in solo") {
    params::ParameterTable t;
    band(t, 0, dsp::Shape::Bell, 2000, 10.0, 2.0, 0, true);
    t.set(params::globalParam(GlobalParam::GainScale), 50.0, ParamSource::Preset);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    const auto h = render(e, impulse(16384));
    CHECK(20.0 * std::log10(dtft(h.l, 2000)) == doctest::Approx(5.0).epsilon(1e-4));
}

TEST_CASE("Monitor decode: Left, Right, Mid and Side bands are heard in L/R") {
    const auto x = noise(8192, 2);
    auto run = [&](int placement) {
        params::ParameterTable t;
        band(t, 0, dsp::Shape::Bell, 1000, 0.0, 1.0, placement, true);
        engine::Engine e(t);
        REQUIRE(e.prepare(kFs, 512));
        return render(e, x);
    };
    auto silent = [](const std::vector<float>& v) { return std::all_of(v.begin(), v.end(), [](float s) { return s == 0.0f; }); };
    const auto st = run(0), lf = run(1), rt = run(2), mid = run(3), side = run(4);
    CHECK(lf.l == st.l);  // Left: the left channel's region, right silent
    CHECK(silent(lf.r));
    CHECK(rt.r == st.r);
    CHECK(silent(rt.l));
    CHECK(mid.l == mid.r);  // Mid: one signal on both sides
    CHECK_FALSE(silent(mid.l));
    bool opposite = true;  // Side: +S on the left, -S on the right
    for (size_t i = 0; i < side.l.size(); ++i) opposite &= side.l[i] == -side.r[i];
    CHECK(opposite);
    CHECK_FALSE(silent(side.l));
    // Mid + Side gives back the stereo audition (the filter is linear and the same).
    double worst = 0.0;
    for (size_t i = 0; i < st.l.size(); ++i)
        worst = std::max({worst, std::abs(double(mid.l[i]) + side.l[i] - st.l[i]),
                          std::abs(double(mid.r[i]) + side.r[i] - st.r[i])});
    CHECK(worst < 1e-6);
}

TEST_CASE("Solo on, another band, and off: crossfades without a jump") {
    // A 300 Hz sine; band 0 at 300 Hz passes it, band 1 at 5 kHz nearly removes it.
    const size_t n = 24000;
    Io x{std::vector<float>(n), std::vector<float>(n)};
    for (size_t i = 0; i < n; ++i) x.l[i] = x.r[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 300.0 * i / kFs));
    params::ParameterTable t;
    band(t, 0, dsp::Shape::Bell, 300, 0.0, 1.0);
    band(t, 1, dsp::Shape::Bell, 5000, 0.0, 1.0);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    std::vector<int> bandAt;
    std::vector<double> mixAt;
    const auto y = render(e, x, [&](size_t p) {
        if (p == 4096) solo(t, 0, true);
        if (p == 9984) solo(t, 0, false), solo(t, 1, true);
        if (p == 16384) solo(t, 1, false);
        bandAt.push_back(e.soloBand());
        mixAt.push_back(e.soloMix());
    });
    // The largest step between samples stays near the sine's own (0.5 * 2 pi 300 / 48000 = 0.02).
    double step = 0.0;
    for (size_t i = 1; i < n; ++i) step = std::max(step, std::abs(double(y.l[i]) - y.l[i - 1]));
    CHECK(step < 0.03);
    const uint32_t fade = static_cast<uint32_t>(std::lround(engine::kSoloFadeMs / 0.9 * 1e-3 * kFs));
    CHECK(bandAt[4096 / 64 + (fade + 63) / 64 + 1] == 0);  // fully in on band 0
    CHECK(mixAt[4096 / 64 + (fade + 63) / 64 + 1] == 1.0);
    CHECK(bandAt[9984 / 64 + 1] == 0);  // still band 0 while fading out
    CHECK(bandAt[9984 / 64 + 2 * (fade + 63) / 64 + 2] == 1);  // then band 1, fading in
    CHECK(bandAt.back() == -1);
    CHECK(e.soloMix() == 0.0);
    // Band 1 (5 kHz) removes most of the 300 Hz sine once fully in.
    double tail = 0.0;
    for (size_t i = 14000; i < 16384; ++i) tail = std::max(tail, double(std::abs(y.l[i])));
    CHECK(tail < 0.06);
}

TEST_CASE("Bypass wins over solo; an unused band cannot be soloed") {
    const auto x = noise(4096, 3);
    params::ParameterTable t;
    band(t, 0, dsp::Shape::Bell, 1000, 6.0, 1.0, 0, true);
    t.set(params::globalParam(GlobalParam::Bypass), 1, ParamSource::Preset);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    const auto y = render(e, x);
    CHECK(y.l == x.l);
    CHECK(y.r == x.r);

    params::ParameterTable tu;
    tu.set(params::bandParam(5, BandField::Solo), 1, ParamSource::Preset);
    engine::Engine eu(tu);
    REQUIRE(eu.prepare(kFs, 512));
    const auto yu = render(eu, x);
    CHECK(yu.l == x.l);
    CHECK(eu.soloBand() == -1);
}

TEST_CASE("Natural Phase: the audition is aligned with the reported latency") {
    params::ParameterTable t;
    band(t, 0, dsp::Shape::Bell, 1000, 0.0, 1.0, 0, true);
    t.set(params::globalParam(GlobalParam::Mode), static_cast<int>(dsp::ProcessingMode::NaturalPhase), ParamSource::Preset);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, 512));
    REQUIRE(e.latencySamples() == 320);
    const auto y = render(e, impulse(16384));
    bool before = true;
    for (size_t i = 0; i < 320; ++i) before &= y.l[i] == 0.0f;
    CHECK(before);
    CHECK(y.l[321] != 0.0f);
    // The same audition as in Zero Latency, 320 samples later.
    params::ParameterTable tz;
    band(tz, 0, dsp::Shape::Bell, 1000, 0.0, 1.0, 0, true);
    engine::Engine ez(tz);
    REQUIRE(ez.prepare(kFs, 512));
    const auto z = render(ez, impulse(16384));
    double worst = 0.0;
    for (size_t i = 0; i + 320 < z.l.size(); ++i) worst = std::max(worst, std::abs(double(y.l[i + 320]) - z.l[i]));
    CHECK(worst < 1e-7);
}
