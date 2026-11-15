// Refined design tier (area 01 R12, R19; "Design tiers" in area 01).
#include "amq/dsp/realtime_design.hpp"
#include "amq/engine/engine.hpp"
#include "amq/params/mappings.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <complex>
#include <functional>
#include <thread>
#include <vector>

using namespace amq;
using params::BandField;
using params::ParamSource;

namespace {

constexpr double kPi = 3.14159265358979323846;

void useBand(params::ParameterTable& t, int b, dsp::Shape shape, double hz, double gainDb, double q,
             dsp::Slope slope = dsp::Slope::Db12) {
    t.set(params::bandParam(b, BandField::Used), 1, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Shape), static_cast<int>(shape), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Slope), static_cast<int>(slope), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Freq), std::log2(hz), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Gain), gainDb, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Q), params::positionFromQ(q), ParamSource::Preset);
}

// Runs n samples of mono-duplicated input through the engine in 64-sample pieces.
std::vector<float> run(engine::Engine& e, const std::vector<float>& x, size_t piece = 64,
                       const std::function<void()>& between = {}) {
    std::vector<float> l(x.size()), r(x.size());
    for (size_t p = 0; p < x.size(); p += piece) {
        const size_t n = std::min(piece, x.size() - p);
        const float* ip[2] = {x.data() + p, x.data() + p};
        float* op[2] = {l.data() + p, r.data() + p};
        e.process(ip, 2, op, 2, static_cast<uint32_t>(n));
        if (between) between();
    }
    return l;
}

// Max dB error of the engine's impulse response against the analog prototype, below 0.9 Nyquist.
double maxErrorDb(engine::Engine& e, const dsp::BandSpec& spec, double fs) {
    std::vector<float> imp(32768, 0.0f);
    imp[0] = 1.0f;
    const auto h = run(e, imp);
    const auto proto = dsp::designAnalogPrototype(spec);
    double worst = 0.0;
    for (int i = 0; i < 240; ++i) {
        const double f = 20.0 * std::pow(0.45 * fs / 20.0, i / 239.0);
        std::complex<double> acc = 0.0;
        const double w = 2.0 * kPi * f / fs;
        for (size_t n = 0; n < h.size(); ++n) acc += double(h[n]) * std::polar(1.0, -w * double(n));
        const double ref = std::abs(proto.evaluateHz(f));
        const double got = std::abs(acc);
        if (20.0 * std::log10(ref) < -60.0 && 20.0 * std::log10(got) < -60.0) continue;  // R12 floor rule
        worst = std::max(worst, std::abs(20.0 * std::log10(got / ref)));
    }
    return worst;
}

const dsp::BandSpec kHard{dsp::Shape::Bell, 8000.0, 12.0, 6.0, dsp::Slope::Db12};  // used for click and offline tests

// A band where the fast three-point design misses R12 and the refined design meets it,
// found with core/dsp's own dense error check.
dsp::BandSpec findHardCase(double fs, double& fastDb, double& refinedDb) {
    const dsp::Shape shapes[] = {dsp::Shape::Bell, dsp::Shape::HighShelf, dsp::Shape::HighCut, dsp::Shape::Notch};
    for (auto sh : shapes)
        for (double f : {6000.0, 9000.0, 12000.0, 15000.0, 18000.0})
            for (double q : {0.7, 2.0, 5.0, 10.0}) {
                const dsp::BandSpec s{sh, f, 12.0, q, dsp::Slope::Db12};
                const auto proto = dsp::designAnalogPrototype(s);
                if (!(dsp::r12ToleranceDb(f, q, fs) <= 0.1)) continue;
                fastDb = dsp::denseCheckMaxErrorDb(proto, dsp::designMatched(proto, fs, dsp::DesignMethod::MatchedThreePoint));
                if (fastDb < 0.3) continue;
                refinedDb = dsp::denseCheckMaxErrorDb(proto, dsp::designDigital(proto, fs, dsp::DesignMethod::MagnitudeMatched));
                if (refinedDb <= 0.1) return s;
            }
    return {};
}

}  // namespace

TEST_CASE("Refined swap brings a settled band within R12 where the fast design misses") {
    const double fs = 44100.0;
    double fastDense = 0, refinedDense = 0;
    const dsp::BandSpec hard = findHardCase(fs, fastDense, refinedDense);
    REQUIRE(hard.frequencyHz > 0.0);
    double fastErr = 0, refinedErr = 0;
    for (bool refine : {false, true}) {
        params::ParameterTable t;
        useBand(t, 0, hard.shape, hard.frequencyHz, hard.gainDb, hard.q, hard.slope);
        engine::Engine e(t);
        e.prepare(fs, 64);
        run(e, std::vector<float>(64 * 8, 0.0f));  // settle and post the request
        if (refine) CHECK(e.runDesignJobsForTests() == 1);
        run(e, std::vector<float>(static_cast<size_t>(0.1 * fs), 0.0f));  // past the 50 ms delay and 20 ms glide
        CHECK(e.bandRefined(0) == refine);
        (refine ? refinedErr : fastErr) = maxErrorDb(e, hard, fs);
    }
    MESSAGE(dsp::shapeName(hard.shape) << " " << hard.frequencyHz << " Hz +12 dB Q " << hard.q
            << " at 44.1 kHz, measured through the engine: fast " << fastErr << " dB, refined " << refinedErr
            << " dB (design check: " << fastDense << " / " << refinedDense << ")");
    CHECK(fastErr > 0.1);
    CHECK(refinedErr <= 0.11);  // R12 0.1 dB, plus impulse truncation and float I/O
}

TEST_CASE("The swap to the refined design is click-free") {
    const double fs = 44100.0;
    const size_t n = static_cast<size_t>(0.3 * fs);
    std::vector<float> sine(n);
    for (size_t i = 0; i < n; ++i) sine[i] = 0.3f * static_cast<float>(std::sin(2 * kPi * 7000.0 * i / fs));
    std::vector<float> outs[2];
    for (int refine = 0; refine < 2; ++refine) {
        params::ParameterTable t;
        useBand(t, 0, kHard.shape, kHard.frequencyHz, kHard.gainDb, kHard.q, kHard.slope);
        engine::Engine e(t);
        e.prepare(fs, 64);
        outs[refine] = run(e, sine, 64, [&] {
            if (refine) e.runDesignJobsForTests();
        });
    }
    // The difference between the runs grows smoothly from 0 to the design difference.
    double maxStep = 0, finalDiff = 0;
    for (size_t i = 1; i < n; ++i) {
        const double d0 = double(outs[1][i - 1]) - outs[0][i - 1], d1 = double(outs[1][i]) - outs[0][i];
        maxStep = std::max(maxStep, std::abs(d1 - d0));
        finalDiff = std::max(finalDiff, i > n - 2000 ? std::abs(d1) : 0.0);
    }
    MESSAGE("largest per-sample change of the difference: " << maxStep << " (steady difference " << finalDiff << ")");
    CHECK(maxStep < 0.02);
}

TEST_CASE("A stale refined result is never applied") {
    params::ParameterTable t;
    useBand(t, 0, kHard.shape, kHard.frequencyHz, kHard.gainDb, kHard.q, kHard.slope);
    engine::Engine e(t);
    e.prepare(44100.0, 64);
    run(e, std::vector<float>(64 * 8, 0.0f));
    CHECK(e.stats().refineRequests == 1);
    t.set(params::bandParam(0, BandField::Gain), 6.0, ParamSource::Host);  // moves before the result lands
    run(e, std::vector<float>(64, 0.0f));
    e.runDesignJobsForTests();  // designs the old request
    run(e, std::vector<float>(64 * 20, 0.0f));
    CHECK(e.stats().refinedSwaps == 0);
    CHECK_FALSE(e.bandRefined(0));
    run(e, std::vector<float>(4410, 0.0f));  // settles again: a new request
    CHECK(e.stats().refineRequests == 2);
    e.runDesignJobsForTests();
    run(e, std::vector<float>(4410, 0.0f));
    CHECK(e.stats().refinedSwaps == 1);
    CHECK(e.bandRefined(0));
}

TEST_CASE("Worker thread mode refines settled bands") {
    params::ParameterTable t;
    for (int b = 0; b < 6; ++b) useBand(t, b, dsp::Shape::Bell, 500.0 * (b + 1), 6.0, 4.0);
    engine::Engine e(t);
    e.setDesignMode(engine::DesignMode::Thread);
    e.prepare(48000.0, 64);
    const auto start = std::chrono::steady_clock::now();
    std::vector<float> silence(64 * 64, 0.0f);
    while (e.stats().refinedSwaps < 6 && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
        run(e, silence);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(e.stats().refinedSwaps == 6);
    for (int b = 0; b < 6; ++b) CHECK(e.bandRefined(b));
    e.release();  // joins the worker
}

TEST_CASE("Offline render: refined at once and identical for any block size") {
    auto render = [](size_t piece) {
        params::ParameterTable t;
        useBand(t, 0, kHard.shape, kHard.frequencyHz, kHard.gainDb, kHard.q, kHard.slope);
        useBand(t, 1, dsp::Shape::LowShelf, 120.0, 4.0, 0.7, dsp::Slope::Db24);
        engine::Engine e(t);
        e.setOfflineRender(true);
        e.prepare(44100.0, 4096);
        std::vector<float> x(20000);
        for (size_t i = 0; i < x.size(); ++i) x[i] = 0.2f * static_cast<float>(std::sin(i * 0.37) + std::sin(i * 0.011));
        auto y = run(e, x, piece);
        CHECK(e.bandRefined(0));
        return y;
    };
    const auto ref = render(64);
    CHECK(render(1) == ref);
    CHECK(render(999) == ref);
    CHECK(render(4096) == ref);
}

TEST_CASE("Section-count changes glide both ways (correction section added, then dropped)") {
    const double fs = 44100.0;
    // Find a band whose refined design adds the correction section.
    dsp::BandSpec found{};
    size_t fastN = 0, refinedN = 0;
    for (double f = 2000.0; f < 20000.0 && !refinedN; f *= 1.07) {
        const dsp::BandSpec s{dsp::Shape::BandPass, f, 0.0, 2.0, dsp::Slope::Db12};
        dsp::SvfCascade fast, refined;
        (void)dsp::designBandRealtime(s, fs, fast);
        engine::DesignInputs in{fs, std::log2(f), 0.0, params::positionFromQ(2.0), static_cast<int>(s.shape),
                                static_cast<int>(s.slope)};
        if (engine::designRefinedCascade(in, refined) && refined.sections.size() > fast.sections.size()) {
            found = s;
            fastN = fast.sections.size();
            refinedN = refined.sections.size();
        }
    }
    REQUIRE(refinedN > fastN);
    MESSAGE("Band Pass at " << found.frequencyHz << " Hz: fast " << fastN << " sections, refined " << refinedN);
    params::ParameterTable t;
    useBand(t, 0, found.shape, found.frequencyHz, 0.0, found.q, found.slope);
    engine::Engine e(t);
    e.prepare(fs, 64);
    std::vector<float> noise(static_cast<size_t>(0.1 * fs));
    for (size_t i = 0; i < noise.size(); ++i) noise[i] = 0.1f * static_cast<float>(std::sin(i * 1.3) * std::cos(i * 0.07));
    run(e, noise, 64, [&] { e.runDesignJobsForTests(); });
    CHECK(e.bandRefined(0));
    CHECK(e.bandSectionCount(0) == refinedN);
    t.set(params::bandParam(0, BandField::Q), params::positionFromQ(2.1), ParamSource::Host);  // moves: back to fast
    const auto y = run(e, std::vector<float>(2048, 0.0f));
    bool finite = true;
    for (float v : y) finite &= std::isfinite(v);
    CHECK(finite);
    CHECK_FALSE(e.bandRefined(0));
    CHECK(e.bandSectionCount(0) == fastN);  // the extra section glided to identity and was dropped
}
