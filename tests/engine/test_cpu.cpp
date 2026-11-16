// CPU budget (area 01 acceptance: 24 bands, 48 kHz, 64-sample blocks, <= 3 % of one core).
// A report, skipped by default: run with -tc="CPU*" -ns. Release builds only give meaningful
// numbers. The automated check (with a generous margin for shared CI machines) is the second
// test case, also skipped by default because timing on shared machines is noisy.
#include "amq/engine/engine.hpp"
#include "amq/params/mappings.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace amq;
using params::BandField;
using params::ParamSource;

namespace {

constexpr double kFs = 48000.0;

void fill(params::ParameterTable& t, dsp::Slope slope, int placementMix) {
    for (int b = 0; b < params::kBandCount; ++b) {
        const auto shape = b % 4 == 0 ? dsp::Shape::LowShelf : b % 4 == 1 ? dsp::Shape::HighCut : dsp::Shape::Bell;
        t.set(params::bandParam(b, BandField::Used), 1, ParamSource::Preset);
        t.set(params::bandParam(b, BandField::Shape), static_cast<int>(shape), ParamSource::Preset);
        t.set(params::bandParam(b, BandField::Freq), std::log2(40.0 * std::pow(400.0, b / 23.0)), ParamSource::Preset);
        t.set(params::bandParam(b, BandField::Gain), b % 2 ? 4.0 : -3.0, ParamSource::Preset);
        t.set(params::bandParam(b, BandField::Q), params::positionFromQ(1.5), ParamSource::Preset);
        t.set(params::bandParam(b, BandField::Slope), static_cast<int>(slope), ParamSource::Preset);
        t.set(params::bandParam(b, BandField::Placement), placementMix ? b % 5 : 0, ParamSource::Preset);
    }
}

// Fraction of one core used to process `seconds` of stereo audio in 64-sample blocks.
double load(dsp::Slope slope, int placementMix, dsp::ProcessingMode mode, bool automate, double seconds = 4.0) {
    params::ParameterTable t;
    fill(t, slope, placementMix);
    t.set(params::globalParam(params::GlobalParam::Mode), static_cast<int>(mode), ParamSource::Preset);
    engine::Engine e(t);
    e.prepare(kFs, 64);
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> u(-0.3f, 0.3f);
    std::vector<float> l(64), r(64), ol(64), or_(64);
    const auto blocks = static_cast<size_t>(seconds * kFs / 64);
    // Warm up: let refined designs settle.
    for (size_t k = 0; k < 2000; ++k) {
        for (auto& v : l) v = u(rng);
        for (auto& v : r) v = u(rng);
        const float* ip[2] = {l.data(), r.data()};
        float* op[2] = {ol.data(), or_.data()};
        e.process(ip, 2, op, 2, 64);
    }
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t k = 0; k < blocks; ++k) {
        if (automate && k % 4 == 0)
            for (int b = 0; b < params::kBandCount; b += 3)
                t.set(params::bandParam(b, BandField::Gain), 6.0 * std::sin(0.01 * double(k) + b), ParamSource::Host);
        for (auto& v : l) v = u(rng);
        for (auto& v : r) v = u(rng);
        const float* ip[2] = {l.data(), r.data()};
        float* op[2] = {ol.data(), or_.data()};
        e.process(ip, 2, op, 2, 64);
    }
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return dt / seconds;
}

}  // namespace

TEST_CASE("CPU report: 24 bands, 48 kHz, 64-sample blocks" * doctest::skip()) {
    struct Row {
        const char* name;
        dsp::Slope slope;
        int mix;
        dsp::ProcessingMode mode;
        bool automate;
    };
    const Row rows[] = {
        {"12 dB/oct, stereo, Zero Latency", dsp::Slope::Db12, 0, dsp::ProcessingMode::ZeroLatency, false},
        {"12 dB/oct, L/R/M/S mix, Zero Latency", dsp::Slope::Db12, 1, dsp::ProcessingMode::ZeroLatency, false},
        {"12 dB/oct, stereo, automated", dsp::Slope::Db12, 0, dsp::ProcessingMode::ZeroLatency, true},
        {"48 dB/oct, stereo, Zero Latency", dsp::Slope::Db48, 0, dsp::ProcessingMode::ZeroLatency, false},
        {"96 dB/oct, stereo, Zero Latency", dsp::Slope::Db96, 0, dsp::ProcessingMode::ZeroLatency, false},
        {"12 dB/oct, stereo, Natural Phase", dsp::Slope::Db12, 0, dsp::ProcessingMode::NaturalPhase, false},
        {"12 dB/oct, stereo, Natural Phase, automated", dsp::Slope::Db12, 0, dsp::ProcessingMode::NaturalPhase, true},
    };
    for (const auto& r : rows) std::printf("  %-48s %6.2f %% of one core\n", r.name, 100.0 * load(r.slope, r.mix, r.mode, r.automate));
}

TEST_CASE("CPU budget: typical 24-band session within 3 % of one core (x2 margin)" * doctest::skip()) {
    CHECK(load(dsp::Slope::Db12, 0, dsp::ProcessingMode::ZeroLatency, false) < 0.06);
}
