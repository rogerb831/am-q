// Natural Phase mode in the engine (area 02 R9, R12, R13; area 03 R14; area 04 R9, R10, R13;
// area 05 R2): the latched mode and its latency, the exact identity when every band is flat,
// accuracy against the analog targets with the real designs and convolvers (L/R and the 2x2
// M/S case, stereo and mono), and the kernel scheduler.
#include "amq/dsp/analog_prototype.hpp"
#include "amq/engine/band_response.hpp"
#include "amq/engine/engine.hpp"
#include "amq/params/mappings.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <chrono>
#include <complex>
#include <cstdio>
#include <memory>
#include <vector>

using namespace amq;
using params::BandField;
using params::GlobalParam;
using params::ParamSource;
using cd = std::complex<double>;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;
constexpr uint32_t kMaxFrames = 512;
enum Placement { Stereo = 0, Left = 1, Right = 2, Mid = 3, Side = 4 };

void setMode(params::ParameterTable& t, dsp::ProcessingMode m) {
    t.set(params::globalParam(GlobalParam::Mode), static_cast<int>(m), ParamSource::Preset);
}

void useBand(params::ParameterTable& t, int b, dsp::Shape shape, double hz, double gainDb, double q, int placement,
             dsp::Slope slope = dsp::Slope::Db12) {
    t.set(params::bandParam(b, BandField::Used), 1, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Shape), static_cast<int>(shape), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Freq), std::log2(hz), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Gain), gainDb, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Q), params::positionFromQ(q), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Placement), placement, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Slope), static_cast<int>(slope), ParamSource::Preset);
}

// Silence through the engine, in 64-sample pieces.
void run(engine::Engine& e, size_t samples, uint32_t channels = 2) {
    std::vector<float> z(64, 0.0f), o0(64), o1(64);
    const float* ip[2] = {z.data(), z.data()};
    float* op[2] = {o0.data(), o1.data()};
    for (size_t done = 0; done < samples; done += 64) e.process(ip, channels, op, channels, 64);
}

// Lets every band settle on its refined design and the correction follow it (Manual mode).
void settle(engine::Engine& e, uint32_t channels = 2) {
    for (int i = 0; i < 4; ++i) {
        run(e, 9600, channels);  // past the refine swap delay and glide
        e.runDesignJobsForTests();
        run(e, 9600, channels);
        e.runKernelJobsForTests();
        run(e, 2048, channels);  // pick-up at a partition boundary, then the crossfade
    }
}

// Impulse responses: out[c] for an impulse on input channel `in`.
std::array<std::vector<double>, 2> impulse(engine::Engine& e, int in, size_t n, uint32_t channels = 2) {
    std::vector<float> x[2] = {std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f)};
    std::vector<float> y[2] = {std::vector<float>(n), std::vector<float>(n)};
    x[in][0] = 1.0f;
    for (size_t p = 0; p < n; p += 64) {
        const float* ip[2] = {x[0].data() + p, x[1].data() + p};
        float* op[2] = {y[0].data() + p, y[1].data() + p};
        e.process(ip, channels, op, channels, 64);
    }
    return {std::vector<double>(y[0].begin(), y[0].end()), std::vector<double>(y[1].begin(), y[1].end())};
}

cd dft(const std::vector<double>& h, double hz) {
    cd s = 0.0;
    const double w = 2.0 * kPi * hz / kFs;
    for (size_t i = 0; i < h.size(); ++i) s += h[i] * std::polar(1.0, -w * static_cast<double>(i));
    return s;
}

cd target(const params::ParameterTable& t, int b, double hz) {
    const auto proto = dsp::designAnalogPrototype(engine::bandSpecFrom(engine::bandDesignInputs(t, b, kFs)));
    return proto.evaluateHz(hz);
}

cd delay(double hz, int samples) { return std::polar(1.0, -2.0 * kPi * hz / kFs * samples); }

std::vector<double> testFreqs() {
    std::vector<double> f;
    for (int i = 0; i < 120; ++i) f.push_back(30.0 * std::pow(0.9 * kFs / 2.0 / 30.0, i / 119.0));
    return f;
}

}  // namespace

TEST_CASE("Natural Phase: the mode is latched at prepare and sets the latency (area 02 R9, R13)") {
    params::ParameterTable t;
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, kMaxFrames));
    CHECK(e.activeMode() == dsp::ProcessingMode::ZeroLatency);
    CHECK(e.latencySamples() == 0);
    CHECK_FALSE(e.restartNeeded());

    setMode(t, dsp::ProcessingMode::NaturalPhase);
    CHECK(e.restartNeeded());               // the host must restart the plugin
    CHECK(e.latencySamples() == 0);         // until then the old mode runs and is reported
    REQUIRE(e.prepare(kFs, kMaxFrames));
    CHECK(e.activeMode() == dsp::ProcessingMode::NaturalPhase);
    CHECK(e.latencySamples() == 320);
    CHECK(e.latencySamples() == dsp::latencySamples(dsp::ProcessingMode::NaturalPhase, 1, kFs, false));
    CHECK_FALSE(e.restartNeeded());

    setMode(t, dsp::ProcessingMode::LinearPhase);  // Phase 2: runs as Zero Latency for now
    CHECK(e.requestedMode() == dsp::ProcessingMode::ZeroLatency);
    CHECK(e.restartNeeded());
    REQUIRE(e.prepare(96000.0, kMaxFrames));
    CHECK(e.latencySamples() == 0);
    e.release();
    CHECK_FALSE(e.restartNeeded());
}

TEST_CASE("Natural Phase with every band flat: an impulse comes out exactly 320 samples late") {
    params::ParameterTable t;
    setMode(t, dsp::ProcessingMode::NaturalPhase);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, kMaxFrames));
    REQUIRE(e.latencySamples() == 320);
    run(e, 4096);
    for (int in = 0; in < 2; ++in) {
        const auto y = impulse(e, in, 2048);
        double worst = 0.0;
        for (size_t i = 0; i < 2048; ++i)
            for (int c = 0; c < 2; ++c) {
                const double want = (c == in && i == 320) ? 1.0 : 0.0;
                worst = std::max(worst, std::abs(y[static_cast<size_t>(c)][i] - want));
            }
        INFO("input channel " << in << ": worst deviation " << worst);
        CHECK(worst < 1e-6);  // float convolver rounding (area 05: the exact delay is open)
    }
}

TEST_CASE("Natural Phase accuracy through the engine: L/R bands match the analog target, delayed by 320") {
    params::ParameterTable t;
    setMode(t, dsp::ProcessingMode::NaturalPhase);
    useBand(t, 0, dsp::Shape::Bell, 1000, 6, 1, Stereo);
    useBand(t, 1, dsp::Shape::HighShelf, 8000, 6, 0.7, Stereo);
    useBand(t, 2, dsp::Shape::LowCut, 60, 0, 0.7, Stereo);
    useBand(t, 3, dsp::Shape::Bell, 12000, 9, 2, Left);
    useBand(t, 4, dsp::Shape::Bell, 300, -6, 2, Right);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, kMaxFrames));
    settle(e);
    CHECK(e.kernelSwaps() > 0);
    for (int ch = 0; ch < 2; ++ch) {
        const auto y = impulse(e, ch, 16384);
        double worstDb = 0, worstDeg = 0, worstCross = 0;
        for (double hz : testFreqs()) {
            cd want = delay(hz, 320);
            for (int b = 0; b < 5; ++b) {
                if (b == 3 && ch != 0) continue;
                if (b == 4 && ch != 1) continue;
                want *= target(t, b, hz);
            }
            if (std::abs(want) < 0.01) continue;
            const cd r = dft(y[static_cast<size_t>(ch)], hz) / want;
            worstDb = std::max(worstDb, std::abs(20.0 * std::log10(std::abs(r))));
            worstDeg = std::max(worstDeg, std::abs(std::arg(r)) * 180.0 / kPi);
            worstCross = std::max(worstCross, std::abs(dft(y[static_cast<size_t>(1 - ch)], hz)));
        }
        INFO("channel " << ch << ": " << worstDb << " dB, " << worstDeg << " deg, cross " << worstCross);
        CHECK(worstDb <= 0.05);
        CHECK(worstDeg <= 2.0);
        CHECK(worstCross < 1e-6);  // no M/S band: no cross terms
    }
}

TEST_CASE("Natural Phase accuracy with Mid and Side bands: the 2x2 correction (area 03 R14)") {
    params::ParameterTable t;
    setMode(t, dsp::ProcessingMode::NaturalPhase);
    useBand(t, 0, dsp::Shape::Bell, 300, 4, 1, Left);
    useBand(t, 1, dsp::Shape::Bell, 1000, 6, 1, Mid);
    useBand(t, 2, dsp::Shape::HighShelf, 6000, -6, 0.7, Side);
    useBand(t, 3, dsp::Shape::Bell, 14000, 6, 2, Stereo);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, kMaxFrames));
    settle(e);
    const auto fromL = impulse(e, 0, 16384);
    run(e, 4096);
    const auto fromR = impulse(e, 1, 16384);
    double worst = 0.0;
    for (double hz : testFreqs()) {
        // T = Dec * diag(Mid, Side) * Enc * diag(L/R stage), times the delay.
        const cd st = target(t, 3, hz);
        const cd l = target(t, 0, hz) * st, r = st;
        const cd m = target(t, 1, hz), s = target(t, 2, hz);
        const cd d = delay(hz, 320);
        const cd want[2][2] = {{0.5 * (m + s) * l * d, 0.5 * (m - s) * r * d}, {0.5 * (m - s) * l * d, 0.5 * (m + s) * r * d}};
        const cd got[2][2] = {{dft(fromL[0], hz), dft(fromR[0], hz)}, {dft(fromL[1], hz), dft(fromR[1], hz)}};
        double scale = 0.0;
        for (auto& row : want) for (const cd& v : row) scale = std::max(scale, std::abs(v));
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) worst = std::max(worst, std::abs(got[i][j] - want[i][j]) / scale);
    }
    INFO("worst complex error relative to the largest entry: " << worst);
    CHECK(worst < 0.006);  // about 0.05 dB or 0.35 degrees
}

TEST_CASE("Natural Phase in mono: Stereo bands only, matched to the target") {
    params::ParameterTable t;
    setMode(t, dsp::ProcessingMode::NaturalPhase);
    useBand(t, 0, dsp::Shape::Bell, 2000, 8, 1.5, Stereo);
    useBand(t, 1, dsp::Shape::Bell, 500, 8, 1.5, Mid);  // not on a mono channel (area 02 R5)
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, kMaxFrames));
    settle(e, 1);
    const auto y = impulse(e, 0, 16384, 1);
    double worstDb = 0, worstDeg = 0;
    for (double hz : testFreqs()) {
        const cd r = dft(y[0], hz) / (target(t, 0, hz) * delay(hz, 320));
        worstDb = std::max(worstDb, std::abs(20.0 * std::log10(std::abs(r))));
        worstDeg = std::max(worstDeg, std::abs(std::arg(r)) * 180.0 / kPi);
    }
    INFO(worstDb << " dB, " << worstDeg << " deg");
    CHECK(worstDb <= 0.05);
    CHECK(worstDeg <= 2.0);
}

TEST_CASE("Natural Phase kernel scheduling: a change posts at once, automation is rate-limited (area 04 R13)") {
    params::ParameterTable t;
    setMode(t, dsp::ProcessingMode::NaturalPhase);
    useBand(t, 0, dsp::Shape::Bell, 1000, 6, 1, Stereo);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, kMaxFrames));
    settle(e);
    const auto requests = e.stats().kernelRequests;
    const auto swaps = e.kernelSwaps();
    run(e, 4800);
    CHECK(e.stats().kernelRequests == requests);  // nothing changes, nothing is posted

    // A step: posted in the first piece; built; in place well within 20 ms at 48 kHz.
    t.set(params::bandParam(0, BandField::Gain), -6.0, ParamSource::Host);
    run(e, 64);
    CHECK(e.stats().kernelRequests == requests + 1);
    e.runKernelJobsForTests();
    run(e, 128);  // the next partition boundary picks the new kernels up
    CHECK(e.kernelSwaps() == swaps + 1);

    // Continuous automation for 200 ms: at most one request per 10 ms (plus the first).
    const auto before = e.stats().kernelRequests;
    for (int i = 0; i < 150; ++i) {
        t.set(params::bandParam(0, BandField::Freq), std::log2(1000.0 + 20.0 * i), ParamSource::Host);
        run(e, 64);
    }
    const auto posted = e.stats().kernelRequests - before;
    INFO(posted << " requests in 200 ms");
    CHECK(posted >= 15);
    CHECK(posted <= 21);
}

namespace {

// 24 bands across shapes and all five placements, designed as the engine settles on them.
void use24(params::ParameterTable& t) {
    const dsp::Shape shapes[] = {dsp::Shape::Bell, dsp::Shape::LowShelf, dsp::Shape::HighShelf, dsp::Shape::Notch,
                                 dsp::Shape::Bell, dsp::Shape::LowCut, dsp::Shape::HighCut, dsp::Shape::TiltShelf};
    for (int b = 0; b < 24; ++b)
        useBand(t, b, shapes[b % 8], 40.0 * std::pow(2.0, b * 0.4), (b % 2 ? 1.0 : -1.0) * (3.0 + b % 5), 0.7 + 0.3 * (b % 4),
                b % 5);
}

std::unique_ptr<engine::CorrectionRequest> request24(const params::ParameterTable& t) {
    auto r = std::make_unique<engine::CorrectionRequest>();
    r->sampleRate = kFs;
    r->channels = 2;
    const uint8_t masks[] = {3, 1, 2, 1, 2};  // Stereo, Left, Right, Mid, Side
    for (int b = 0; b < 24; ++b) {
        auto& cb = r->bands[static_cast<size_t>(b)];
        const int placement = static_cast<int>(t.value(params::bandParam(b, BandField::Placement)));
        cb.active = true;
        cb.stage = placement >= Mid ? 1 : 0;
        cb.mask = masks[placement];
        cb.in = engine::bandDesignInputs(t, b, kFs);
        REQUIRE(engine::designBandCascade(cb.in, engine::DesignTier::Refined, cb.cascade));
    }
    return r;
}

}  // namespace

TEST_CASE("Response caches: a one-band edit recomputes only that band, bit-identical to a fresh build (area 04 R12)") {
    params::ParameterTable t;
    use24(t);
    auto r = request24(t);
    dsp::NaturalPhaseKernelBuilder builder;
    auto cache = std::make_unique<engine::CorrectionCache>();
    auto warm = std::make_unique<engine::CorrectionKernels>(), fresh = std::make_unique<engine::CorrectionKernels>();
    engine::buildCorrectionKernels(*r, builder, *cache, *warm);
    CHECK(cache->recomputed == 24);
    engine::buildCorrectionKernels(*r, builder, *cache, *warm);
    CHECK(cache->recomputed == 24);  // nothing changed: nothing recomputed

    // Edit band 7's gain, then band 12's cascade alone (same inputs, other sections).
    t.set(params::bandParam(7, BandField::Gain), 9.5, ParamSource::Preset);
    r->bands[7].in = engine::bandDesignInputs(t, 7, kFs);
    REQUIRE(engine::designBandCascade(r->bands[7].in, engine::DesignTier::Refined, r->bands[7].cascade));
    engine::buildCorrectionKernels(*r, builder, *cache, *warm);
    CHECK(cache->recomputed == 25);
    REQUIRE(engine::designBandCascade(r->bands[12].in, engine::DesignTier::Fast, r->bands[12].cascade));
    engine::buildCorrectionKernels(*r, builder, *cache, *warm);
    CHECK(cache->recomputed == 26);
    // A band turned off and on again keeps its entry.
    r->bands[3].active = false;
    engine::buildCorrectionKernels(*r, builder, *cache, *warm);
    r->bands[3].active = true;
    engine::buildCorrectionKernels(*r, builder, *cache, *warm);
    CHECK(cache->recomputed == 26);

    engine::buildCorrectionKernels(*r, builder, *fresh);  // a fresh cache
    CHECK(warm->taps == fresh->taps);
}

TEST_CASE("Response caches in the engine: a gain jump on one of 24 bands recomputes one band") {
    params::ParameterTable t;
    setMode(t, dsp::ProcessingMode::NaturalPhase);
    use24(t);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, kMaxFrames));
    settle(e);
    const auto before = e.kernelCacheRecomputes();
    CHECK(before >= 24);
    // A 20 dB step is a jump (no ramp), so exactly one design and one kernel request follow.
    t.set(params::bandParam(5, BandField::Gain), t.value(params::bandParam(5, BandField::Gain)) + 20.0, ParamSource::Host);
    run(e, 64);
    e.runKernelJobsForTests();
    CHECK(e.kernelCacheRecomputes() == before + 1);
}

// Report: kernel build time for 24 bands with a cold cache and after a one-band edit. Run with
//   amq_engine_tests -tc="Response cache timing*" --no-skip
TEST_CASE("Response cache timing (report)" * doctest::skip()) {
    params::ParameterTable t;
    use24(t);
    auto r = request24(t);
    dsp::NaturalPhaseKernelBuilder builder;
    auto kernels = std::make_unique<engine::CorrectionKernels>();
    using clk = std::chrono::steady_clock;
    const int n = 50;
    double cold = 0, edit = 0, same = 0;
    for (int i = 0; i < n; ++i) {
        auto cache = std::make_unique<engine::CorrectionCache>();
        auto t0 = clk::now();
        engine::buildCorrectionKernels(*r, builder, *cache, *kernels);
        auto t1 = clk::now();
        r->bands[7].in.gainDb += 0.01;  // one band's inputs change
        engine::buildCorrectionKernels(*r, builder, *cache, *kernels);
        auto t2 = clk::now();
        engine::buildCorrectionKernels(*r, builder, *cache, *kernels);
        auto t3 = clk::now();
        cold += std::chrono::duration<double, std::milli>(t1 - t0).count();
        edit += std::chrono::duration<double, std::milli>(t2 - t1).count();
        same += std::chrono::duration<double, std::milli>(t3 - t2).count();
    }
    std::printf("24 bands, 48 kHz: cold %.3f ms, one band edited %.3f ms, nothing changed %.3f ms (mean of %d)\n",
                cold / n, edit / n, same / n, n);
}

TEST_CASE("Zero Latency is unchanged: no correction stage, latency 0") {
    params::ParameterTable t;
    useBand(t, 0, dsp::Shape::Bell, 1000, 6, 1, Stereo);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, kMaxFrames));
    settle(e);
    CHECK(e.latencySamples() == 0);
    CHECK(e.stats().kernelRequests == 0);
    CHECK(e.kernelSwaps() == 0);
}
