// Global bypass and the mode-change fade (area 01 R6, area 02 R2 and R13, area 09 R7): the
// bypass null against the input delayed by the reported latency, the exact shape of the
// crossfade, the fade out before a restart and back in after it, and the restart timeout.
#include "amq/engine/engine.hpp"
#include "amq/params/mappings.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <vector>

using namespace amq;
using params::BandField;
using params::GlobalParam;
using params::ParamSource;

namespace {

constexpr double kFs = 48000.0;
constexpr uint32_t kMaxFrames = 512;

struct Io {
    std::vector<float> l, r;
};

void setMode(params::ParameterTable& t, dsp::ProcessingMode m, ParamSource src = ParamSource::Preset) {
    t.set(params::globalParam(GlobalParam::Mode), static_cast<int>(m), src);
}
void setBypass(params::ParameterTable& t, bool on, ParamSource src = ParamSource::Preset) {
    t.set(params::globalParam(GlobalParam::Bypass), on ? 1 : 0, src);
}

// A few bands that change the signal clearly, on both stages.
void useBands(params::ParameterTable& t) {
    auto band = [&](int b, dsp::Shape s, double hz, double db, double q, int placement) {
        t.set(params::bandParam(b, BandField::Used), 1, ParamSource::Preset);
        t.set(params::bandParam(b, BandField::Shape), static_cast<int>(s), ParamSource::Preset);
        t.set(params::bandParam(b, BandField::Freq), std::log2(hz), ParamSource::Preset);
        t.set(params::bandParam(b, BandField::Gain), db, ParamSource::Preset);
        t.set(params::bandParam(b, BandField::Q), params::positionFromQ(q), ParamSource::Preset);
        t.set(params::bandParam(b, BandField::Placement), placement, ParamSource::Preset);
    };
    band(0, dsp::Shape::Bell, 1000, 12, 1, 0);
    band(1, dsp::Shape::HighShelf, 6000, -6, 0.7, 0);
    band(2, dsp::Shape::Bell, 300, 6, 2, 3);  // Mid
}

Io noise(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    Io x{std::vector<float>(n), std::vector<float>(n)};
    for (size_t i = 0; i < n; ++i) x.l[i] = u(rng), x.r[i] = u(rng);
    return x;
}

// Renders in 64-sample pieces; `at(sample)` may change the table before a piece.
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

uint32_t rampLength(double ms) { return static_cast<uint32_t>(std::lround(ms / 0.9 * 1e-3 * kFs)); }

}  // namespace

TEST_CASE("Bypass null: fully bypassed output is the input delayed by the reported latency, bit for bit") {
    for (auto mode : {dsp::ProcessingMode::ZeroLatency, dsp::ProcessingMode::NaturalPhase}) {
        params::ParameterTable t;
        useBands(t);
        setMode(t, mode);
        setBypass(t, true);
        engine::Engine e(t);
        REQUIRE(e.prepare(kFs, kMaxFrames));
        const uint32_t lat = e.latencySamples();
        const auto x = noise(8192, 3);
        const auto y = render(e, x);
        bool exact = true;
        for (size_t i = 0; i < x.l.size(); ++i) {
            const float wl = i >= lat ? x.l[i - lat] : 0.0f, wr = i >= lat ? x.r[i - lat] : 0.0f;
            exact &= y.l[i] == wl && y.r[i] == wr;
        }
        INFO("mode " << static_cast<int>(mode) << ", latency " << lat);
        CHECK(exact);
    }
}

TEST_CASE("Bypass toggles crossfade linearly between the processed and the delayed dry signal") {
    for (auto mode : {dsp::ProcessingMode::ZeroLatency, dsp::ProcessingMode::NaturalPhase}) {
        const auto x = noise(16384, 9);
        const size_t onAt = 4096, offAt = 10240;
        // References: always processed, and the dry input delayed by the latency.
        params::ParameterTable tw;
        useBands(tw);
        setMode(tw, mode);
        engine::Engine ew(tw);
        REQUIRE(ew.prepare(kFs, kMaxFrames));
        const auto wet = render(ew, x);
        const uint32_t lat = ew.latencySamples();

        params::ParameterTable t;
        useBands(t);
        setMode(t, mode);
        engine::Engine e(t);
        REQUIRE(e.prepare(kFs, kMaxFrames));
        const auto y = render(e, x, [&](size_t p) {
            if (p == onAt) setBypass(t, true, ParamSource::Host);
            if (p == offAt) setBypass(t, false, ParamSource::Host);
        });
        const uint32_t len = rampLength(engine::kBypassFadeMs);
        double worst = 0.0;
        for (size_t i = 0; i < x.l.size(); ++i) {
            double m = 0.0;  // the ramp moves at the event's sample (LinearRamp::next)
            if (i >= onAt) m = std::min(1.0, double(i - onAt + 1) / len);
            if (i >= offAt) m = std::max(0.0, 1.0 - double(i - offAt + 1) / len);
            const double dl = i >= lat ? x.l[i - lat] : 0.0, dr = i >= lat ? x.r[i - lat] : 0.0;
            worst = std::max(worst, std::abs(y.l[i] - (wet.l[i] + m * (dl - wet.l[i]))));
            worst = std::max(worst, std::abs(y.r[i] - (wet.r[i] + m * (dr - wet.r[i]))));
        }
        INFO("mode " << static_cast<int>(mode) << ": worst deviation from the ideal crossfade " << worst);
        CHECK(worst < 1e-6);
        CHECK(e.bypassMix() == 0.0);
    }
}

TEST_CASE("Mode change: the output fades out, the restart is due once silent, and the next activation fades in") {
    params::ParameterTable t;
    useBands(t);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, kMaxFrames));
    const auto x = noise(8192, 5);
    const size_t at = 2048;
    const auto y = render(e, x, [&](size_t p) {
        if (p == at) setMode(t, dsp::ProcessingMode::NaturalPhase, ParamSource::Host);
        if (p == at) CHECK_FALSE(e.restartReady());
    });
    const uint32_t len = rampLength(engine::kModeFadeMs);
    CHECK(e.restartNeeded());
    CHECK(e.restartReady());
    CHECK(e.latencySamples() == 0);  // the old mode runs until the restart
    // Silent after the fade, and the fade itself is a linear ramp on the processed signal.
    bool silentAfter = true;
    for (size_t i = at + len; i < x.l.size(); ++i) silentAfter &= y.l[i] == 0.0f && y.r[i] == 0.0f;
    CHECK(silentAfter);

    // Reference without the change: the fade must be ref * (1 - (i - at + 1) / len).
    params::ParameterTable tr;
    useBands(tr);
    engine::Engine er(tr);
    REQUIRE(er.prepare(kFs, kMaxFrames));
    const auto ref = render(er, x);
    double worst = 0.0;
    for (size_t i = 0; i < at + len; ++i) {
        const double g = i < at ? 1.0 : 1.0 - double(i - at + 1) / len;
        worst = std::max(worst, std::abs(y.l[i] - ref.l[i] * g));
    }
    INFO("fade-out deviation " << worst);
    CHECK(worst < 1e-6);

    // The host restarts: Natural Phase, and the output fades in from silence.
    REQUIRE(e.prepare(kFs, kMaxFrames));
    CHECK(e.latencySamples() == 320);
    CHECK_FALSE(e.restartNeeded());
    CHECK(e.modeFadeGain() == 0.0);
    setBypass(t, true);  // bypassed: the output is the delayed input, so the fade-in is visible exactly
    e.snapToTable();
    const auto z = render(e, x);
    double worstIn = 0.0;
    for (size_t i = 0; i < 2 * len; ++i) {
        const double g = std::min(1.0, double(i + 1) / len);
        const double want = i >= 320 ? x.l[i - 320] * g : 0.0;
        worstIn = std::max(worstIn, std::abs(z.l[i] - want));
    }
    INFO("fade-in deviation " << worstIn);
    CHECK(worstIn < 1e-6);
    CHECK(e.modeFadeGain() == 1.0);
}

TEST_CASE("Mode change undone before the restart: the output fades back in") {
    params::ParameterTable t;
    useBands(t);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, kMaxFrames));
    const auto x = noise(8192, 6);
    render(e, x, [&](size_t p) {
        if (p == 1024) setMode(t, dsp::ProcessingMode::NaturalPhase, ParamSource::Host);
        if (p == 4096) setMode(t, dsp::ProcessingMode::ZeroLatency, ParamSource::Host);
    });
    CHECK_FALSE(e.restartNeeded());
    CHECK_FALSE(e.restartReady());
    CHECK(e.modeFadeGain() == 1.0);
}

TEST_CASE("A host that never restarts: after the wait the old mode fades back in") {
    params::ParameterTable t;
    useBands(t);
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, kMaxFrames));
    const auto x = noise(static_cast<size_t>(kFs * 1.5), 7);
    const auto y = render(e, x, [&](size_t p) {
        if (p == 0) setMode(t, dsp::ProcessingMode::NaturalPhase, ParamSource::Host);
    });
    CHECK(e.restartNeeded());          // still wanted: the plugin's request stays outstanding
    CHECK_FALSE(e.restartReady());     // but the output plays again
    CHECK(e.modeFadeGain() == 1.0);
    const size_t back = static_cast<size_t>(kFs * 1.2);
    double tail = 0.0;
    for (size_t i = back; i < y.l.size(); ++i) tail = std::max(tail, double(std::abs(y.l[i])));
    CHECK(tail > 0.1);
}

TEST_CASE("Other activations start at full level: no fade without a mode change") {
    params::ParameterTable t;
    engine::Engine e(t);
    REQUIRE(e.prepare(kFs, kMaxFrames));
    CHECK(e.modeFadeGain() == 1.0);
    const auto x = noise(512, 8);
    const auto y = render(e, x);
    CHECK(y.l == x.l);
    CHECK(y.r == x.r);
}
