// EQ Match through the engine (area 19 "hook the accumulators into the engine at highest resolution";
// area 16 R1, R15, R16; area 17 R1, R2, R8; area 20 R2, R7, R8): input from the plugin input, reference
// from the side chain, the published curve, and a fit that finds the difference.
#include "amq/engine/engine.hpp"
#include "amq/params/mappings.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

using namespace amq;
using params::BandField;
using params::ParamSource;

namespace {

std::vector<float> pink(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, 1.0);
    std::vector<float> x(n);
    double b[7] = {};
    for (auto& v : x) {
        const double w = g(rng);
        b[0] = 0.99886 * b[0] + w * 0.0555179;
        b[1] = 0.99332 * b[1] + w * 0.0750759;
        b[2] = 0.96900 * b[2] + w * 0.1538520;
        b[3] = 0.86650 * b[3] + w * 0.3104856;
        b[4] = 0.55000 * b[4] + w * 0.5329522;
        b[5] = -0.7616 * b[5] - w * 0.0168980;
        v = static_cast<float>(0.05 * (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362));
        b[6] = w * 0.115926;
    }
    return x;
}

}  // namespace

TEST_CASE("EQ Match in the engine: input and side-chain captures, the curve, and a fit that finds a bell") {
    const double fs = 48000.0;
    const size_t n = static_cast<size_t>(8 * fs);
    const auto x = pink(n, 5);
    // The reference: the same programme through a +6 dB bell at 1 kHz.
    params::ParameterTable tr;
    tr.set(params::bandParam(0, BandField::Used), 1, ParamSource::Preset);
    tr.set(params::bandParam(0, BandField::Freq), std::log2(1000.0), ParamSource::Preset);
    tr.set(params::bandParam(0, BandField::Gain), 6.0, ParamSource::Preset);
    tr.set(params::bandParam(0, BandField::Q), params::positionFromQ(1.5), ParamSource::Preset);
    engine::Engine er(tr);
    er.setOfflineRender(true);
    REQUIRE(er.prepare(fs, 1024));
    std::vector<float> ref(n), refR(n);
    for (size_t p = 0; p < n; p += 1024) {
        const float* ip[2] = {x.data() + p, x.data() + p};
        float* op[2] = {ref.data() + p, refR.data() + p};
        er.process(ip, 2, op, 2, 1024);
    }
    // The engine under test: flat, input = x, side chain = the reference.
    params::ParameterTable t;
    engine::Engine e(t);
    REQUIRE(e.prepare(fs, 1024));
    auto& m = e.eqMatch();
    m.setLevelAlign(false);  // keep the bell's own level in the curve
    engine::MatchSnapshot s;
    CHECK_FALSE(m.startFit());  // no curve yet: Match is disabled (area 16 R15)
    m.setRecording(0, true);
    m.setRecording(1, true);
    std::vector<float> ol(1024), orr(1024);
    for (size_t p = 0; p < n; p += 1024) {
        const float* ip[2] = {x.data() + p, x.data() + p};
        const float* sp[2] = {ref.data() + p, ref.data() + p};
        float* op[2] = {ol.data(), orr.data()};
        e.process(ip, 2, op, 2, 1024, sp, 2);
        e.runAnalyzerForTests();
    }
    REQUIRE(m.latest(s));
    CHECK(s.inputReady);
    CHECK(s.referenceReady);
    CHECK(s.inputSeconds > 5.0);
    REQUIRE(s.curve.valid);
    // The curve: about +6 dB at 1 kHz, about 0 dB two octaves away.
    const auto& g = analysis::matchGrid();
    auto at = [&](double hz) {
        size_t k = 0;
        for (size_t i = 0; i < g.size(); ++i)
            if (std::abs(std::log2(g[i] / hz)) < std::abs(std::log2(g[k] / hz))) k = i;
        return s.curve.target[k];
    };
    CHECK(at(1000.0) == doctest::Approx(6.0).epsilon(0.15));
    CHECK(std::abs(at(100.0)) < 1.0);
    CHECK(std::abs(at(8000.0)) < 1.0);
    // Match: a fit on its thread, then the result once.
    REQUIRE(m.startFit());
    for (int i = 0; i < 600 && m.fitRunning(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    engine::FitResult r;
    REQUIRE(m.takeFit(r));
    CHECK_FALSE(m.takeFit(r));  // taken once
    REQUIRE_FALSE(r.bands.empty());
    const auto& b = r.bands[0];
    CHECK(b.shape == dsp::Shape::Bell);
    CHECK(std::abs(std::log2(b.hz / 1000.0)) < 0.2);
    CHECK(b.gainDb == doctest::Approx(6.0).epsilon(0.15));
    // Reset clears a capture (area 17 R7).
    m.reset(0);
    for (int k = 0; k < 4; ++k) {
        const float* ip[2] = {x.data(), x.data()};
        float* op[2] = {ol.data(), orr.data()};
        e.process(ip, 2, op, 2, 1024);
        e.runAnalyzerForTests();
    }
    m.updateNowForTests();
    REQUIRE(m.latest(s));
    CHECK_FALSE(s.inputReady);
}

TEST_CASE("EQ Match overflow: a stalled analysis worker drops audio, counted; the capture stays valid (area 17 R6)") {
    const double fs = 48000.0;
    params::ParameterTable t;
    engine::Engine e(t);
    REQUIRE(e.prepare(fs, 1024));
    auto& m = e.eqMatch();
    m.setRecording(0, true);
    const auto x = pink(static_cast<size_t>(4 * fs), 9);
    std::vector<float> ol(1024), orr(1024);
    // Two seconds without running the analyzer: far more than its 0.5 s tap ring.
    for (size_t p = 0; p + 1024 <= static_cast<size_t>(2 * fs); p += 1024) {
        const float* ip[2] = {x.data() + p, x.data() + p};
        float* op[2] = {ol.data(), orr.data()};
        e.process(ip, 2, op, 2, 1024);
    }
    e.runAnalyzerForTests();
    CHECK(e.analyzer().droppedFrames(analysis::Source::Pre) > 0);
    // Then normal running: frames keep counting into the same capture.
    for (size_t p = static_cast<size_t>(2 * fs); p + 1024 <= x.size(); p += 1024) {
        const float* ip[2] = {x.data() + p, x.data() + p};
        float* op[2] = {ol.data(), orr.data()};
        e.process(ip, 2, op, 2, 1024);
        e.runAnalyzerForTests();
    }
    m.updateNowForTests();
    engine::MatchSnapshot s;
    REQUIRE(m.latest(s));
    CHECK(s.inputSeconds > 1.5);
    CHECK(s.inputReady);
}
