// M/S stage tests (area 03 R1-R4 and its acceptance tests; area 02 R4, R5).
#include "amq/dsp/mid_side.hpp"
#include "amq/engine/engine.hpp"
#include "amq/params/mappings.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <vector>

using namespace amq;
using params::BandField;
using params::ParamSource;

namespace {

constexpr double kPi = 3.14159265358979323846;
enum Placement { Stereo = 0, Left = 1, Right = 2, Mid = 3, Side = 4 };

void useBand(params::ParameterTable& t, int b, dsp::Shape shape, double hz, double gainDb, double q, int placement) {
    t.set(params::bandParam(b, BandField::Used), 1, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Shape), static_cast<int>(shape), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Freq), std::log2(hz), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Gain), gainDb, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Q), params::positionFromQ(q), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Placement), placement, ParamSource::Preset);
}

struct Stereo2 {
    std::vector<float> l, r;
};

Stereo2 render(engine::Engine& e, const Stereo2& in, size_t piece = 64) {
    Stereo2 out{std::vector<float>(in.l.size()), std::vector<float>(in.l.size())};
    for (size_t p = 0; p < in.l.size(); p += piece) {
        const size_t n = std::min(piece, in.l.size() - p);
        const float* ip[2] = {in.l.data() + p, in.r.data() + p};
        float* op[2] = {out.l.data() + p, out.r.data() + p};
        e.process(ip, 2, op, 2, static_cast<uint32_t>(n));
    }
    return out;
}

double rmsDb(const std::vector<double>& v, size_t from) {
    double s = 0;
    for (size_t i = from; i < v.size(); ++i) s += v[i] * v[i];
    return 10.0 * std::log10(s / double(v.size() - from) + 1e-300);
}

std::vector<double> mid(const Stereo2& x) {
    std::vector<double> m(x.l.size());
    for (size_t i = 0; i < m.size(); ++i) m[i] = 0.5 * (double(x.l[i]) + x.r[i]);
    return m;
}
std::vector<double> side(const Stereo2& x) {
    std::vector<double> s(x.l.size());
    for (size_t i = 0; i < s.size(); ++i) s[i] = 0.5 * (double(x.l[i]) - x.r[i]);
    return s;
}

double maxAbsDiffDb(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0;
    for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(double(a[i]) - b[i]));
    return 20.0 * std::log10(m + 1e-300);
}

Stereo2 sines(size_t n, double fs, double hzL, double ampL, double hzR, double ampR) {
    Stereo2 x{std::vector<float>(n), std::vector<float>(n)};
    for (size_t i = 0; i < n; ++i) {
        x.l[i] = static_cast<float>(ampL * std::sin(2 * kPi * hzL * i / fs));
        x.r[i] = static_cast<float>(ampR * std::sin(2 * kPi * hzR * i / fs));
    }
    return x;
}

}  // namespace

TEST_CASE("M/S matrix: textbook form and an exact round trip") {
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    std::vector<double> l(10000), r(10000);
    for (size_t i = 0; i < l.size(); ++i) l[i] = u(rng), r[i] = u(rng);
    auto a = l, b = r;
    dsp::midSide(dsp::MidSideMode::Encode, a.data(), b.data(), 10000);
    CHECK(a[7] == doctest::Approx(0.5 * (l[7] + r[7])));
    CHECK(b[7] == doctest::Approx(0.5 * (l[7] - r[7])));  // textbook S sign (R1)
    dsp::midSide(dsp::MidSideMode::Decode, a.data(), b.data(), 10000);
    double err = 0;
    for (size_t i = 0; i < l.size(); ++i) err = std::max({err, std::abs(a[i] - l[i]), std::abs(b[i] - r[i])});
    CHECK(20.0 * std::log10(err + 1e-300) <= -140.0);  // R2
}

TEST_CASE("Mid-only band: only (L+R) content changes") {
    const double fs = 48000.0;
    for (int signal = 0; signal < 2; ++signal) {  // L only, then L = R
        params::ParameterTable t;
        useBand(t, 0, dsp::Shape::Bell, 1000.0, 6.0, 1.0, Mid);
        engine::Engine e(t);
        e.prepare(fs, 64);
        const auto x = signal == 0 ? sines(48000, fs, 1000.0, 0.3, 1000.0, 0.0) : sines(48000, fs, 1000.0, 0.3, 1000.0, 0.3);
        const auto y = render(e, x);
        const double gain = rmsDb(mid(y), 9600) - rmsDb(mid(x), 9600);
        INFO("signal " << signal);
        CHECK(gain == doctest::Approx(6.0).epsilon(0.05 / 6.0));  // within 0.05 dB
        // Side content is untouched (exactly zero for L = R).
        const auto sx = side(x), sy = side(y);
        double sErr = 0;
        for (size_t i = 0; i < sx.size(); ++i) sErr = std::max(sErr, std::abs(sy[i] - sx[i]));
        CHECK(20.0 * std::log10(sErr + 1e-300) < -120.0);
    }
}

TEST_CASE("Side-only band on a mono (L = R) signal leaves it unchanged") {
    params::ParameterTable t;
    useBand(t, 0, dsp::Shape::Bell, 1000.0, 6.0, 1.0, Side);
    engine::Engine e(t);
    e.prepare(48000.0, 64);
    const auto x = sines(24000, 48000.0, 1000.0, 0.3, 1000.0, 0.3);
    const auto y = render(e, x);
    CHECK(maxAbsDiffDb(y.l, x.l) <= -140.0);
    CHECK(maxAbsDiffDb(y.r, x.r) <= -140.0);
}

TEST_CASE("Stage order: L/R bands first, then the M/S stage") {
    // A Mid band at index 0 must still run after a Left band at index 1.
    auto run = [](bool swapIndices) {
        params::ParameterTable t;
        useBand(t, swapIndices ? 1 : 0, dsp::Shape::Bell, 1000.0, 9.0, 2.0, Mid);
        useBand(t, swapIndices ? 0 : 1, dsp::Shape::HighShelf, 3000.0, -6.0, 0.7, Left);
        engine::Engine e(t);
        e.prepare(48000.0, 64);
        Stereo2 x{std::vector<float>(8000), std::vector<float>(8000)};
        std::mt19937 rng(3);
        std::uniform_real_distribution<float> u(-0.3f, 0.3f);
        for (size_t i = 0; i < 8000; ++i) x.l[i] = u(rng), x.r[i] = u(rng);
        return render(e, x);
    };
    const auto a = run(false), b = run(true);
    CHECK(maxAbsDiffDb(a.l, b.l) < -120.0);
    CHECK(maxAbsDiffDb(a.r, b.r) < -120.0);
}

TEST_CASE("Toggling a 0 dB Mid band while playing causes no discontinuity") {
    const double fs = 48000.0;
    params::ParameterTable t;
    useBand(t, 0, dsp::Shape::Bell, 1000.0, 0.0, 1.0, Mid);
    t.set(params::bandParam(0, BandField::Enabled), 0, ParamSource::Preset);
    engine::Engine e(t);
    e.prepare(fs, 64);
    const auto x = sines(48000, fs, 440.0, 0.3, 660.0, 0.2);
    Stereo2 y{std::vector<float>(x.l.size()), std::vector<float>(x.l.size())};
    for (size_t p = 0; p < x.l.size(); p += 64) {
        if (p == 9600) t.set(params::bandParam(0, BandField::Enabled), 1, ParamSource::Host);
        if (p == 28800) t.set(params::bandParam(0, BandField::Enabled), 0, ParamSource::Host);
        const float* ip[2] = {x.l.data() + p, x.r.data() + p};
        float* op[2] = {y.l.data() + p, y.r.data() + p};
        e.process(ip, 2, op, 2, 64);
    }
    // A 0 dB Bell is flat, so the matrix insert and removal must leave the signal unchanged.
    const double diff = std::max(maxAbsDiffDb(y.l, x.l), maxAbsDiffDb(y.r, x.r));
    MESSAGE("0 dB Mid band toggled on and off: largest deviation " << diff << " dBFS");
    CHECK(diff <= -120.0);
}

TEST_CASE("Mono layout: only Stereo bands affect audio") {
    params::ParameterTable t;
    useBand(t, 0, dsp::Shape::Bell, 1000.0, 12.0, 1.0, Mid);
    useBand(t, 1, dsp::Shape::Bell, 2000.0, 12.0, 1.0, Side);
    useBand(t, 2, dsp::Shape::Bell, 3000.0, 12.0, 1.0, Left);
    useBand(t, 3, dsp::Shape::Bell, 4000.0, 12.0, 1.0, Right);
    engine::Engine e(t);
    e.prepare(48000.0, 64);
    std::vector<float> x(4096), y(4096);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.3f * static_cast<float>(std::sin(i * 0.13));
    for (size_t p = 0; p < x.size(); p += 64) {
        const float* ip[1] = {x.data() + p};
        float* op[1] = {y.data() + p};
        e.process(ip, 1, op, 1, 64);
    }
    CHECK(y == x);
    useBand(t, 4, dsp::Shape::Bell, 1000.0, 12.0, 1.0, Stereo);
    for (size_t p = 0; p < x.size(); p += 64) {
        const float* ip[1] = {x.data() + p};
        float* op[1] = {y.data() + p};
        e.process(ip, 1, op, 1, 64);
    }
    CHECK(y != x);
}

TEST_CASE("Block-size robustness with L/R and M/S bands") {
    auto run = [](size_t piece) {
        params::ParameterTable t;
        useBand(t, 0, dsp::Shape::LowShelf, 150.0, 3.0, 0.7, Stereo);
        useBand(t, 1, dsp::Shape::Bell, 2500.0, -5.0, 2.0, Side);
        useBand(t, 2, dsp::Shape::HighCut, 15000.0, 0.0, 0.7, Mid);
        engine::Engine e(t);
        e.prepare(48000.0, 4096);
        Stereo2 x{std::vector<float>(9000), std::vector<float>(9000)};
        std::mt19937 rng(5);
        std::uniform_real_distribution<float> u(-0.3f, 0.3f);
        for (size_t i = 0; i < 9000; ++i) x.l[i] = u(rng), x.r[i] = u(rng);
        return render(e, x, piece).l;
    };
    const auto ref = run(64);
    CHECK(run(1) == ref);
    CHECK(run(7) == ref);
    CHECK(run(4096) == ref);
}
