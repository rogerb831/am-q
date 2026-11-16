// Filter kernel and band chain tests (area 01 R3/R7/R15/R18/R19, area 02 R5/R6).
#include "amq/dsp/realtime_design.hpp"
#include "amq/engine/engine.hpp"
#include "amq/params/mappings.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <new>
#include <random>
#include <vector>

#include "alloc_counter.hpp"

namespace {
auto& gCountAllocs = amq_test::countAllocs;
auto& gAllocs = amq_test::allocs;
}  // namespace

using namespace amq;
using params::BandField;
using params::GlobalParam;
using params::ParamSource;

namespace {

std::vector<float> noise(size_t n, unsigned seed, float amp = 0.5f) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-amp, amp);
    std::vector<float> v(n);
    for (auto& x : v) x = u(rng);
    return v;
}

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(size_t n, float v = 0.0f) : l(n, v), r(n, v) {}
};

void piece(engine::Engine& e, const Stereo& in, Stereo& out, size_t from, size_t to) {
    const float* ip[2] = {in.l.data() + from, in.r.data() + from};
    float* op[2] = {out.l.data() + from, out.r.data() + from};
    e.process(ip, 2, op, 2, static_cast<uint32_t>(to - from));
}

void useBand(params::ParameterTable& t, int b, dsp::Shape shape, double hz, double gainDb, double q,
             dsp::Slope slope = dsp::Slope::Db12, int placement = 0) {
    t.set(params::bandParam(b, BandField::Used), 1, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Shape), static_cast<int>(shape), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Slope), static_cast<int>(slope), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Freq), std::log2(hz), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Gain), gainDb, ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Q), params::positionFromQ(q), ParamSource::Preset);
    t.set(params::bandParam(b, BandField::Placement), placement, ParamSource::Preset);
}

double rmsDb(const std::vector<float>& v, size_t from) {
    double s = 0;
    for (size_t i = from; i < v.size(); ++i) s += double(v[i]) * v[i];
    return 10.0 * std::log10(s / double(v.size() - from));
}

int firstDifference(const std::vector<float>& a, const std::vector<float>& b) {
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return static_cast<int>(i);
    return -1;
}

}  // namespace

TEST_CASE("SVF realises a stable biquad exactly") {
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    for (int trial = 0; trial < 500; ++trial) {
        const double r = 0.05 + 0.949 * u(rng), th = 3.1 * u(rng);
        dsp::Biquad b;
        b.a1 = -2.0 * r * std::cos(th);
        b.a2 = r * r;
        b.b0 = u(rng) * 2 - 1;
        b.b1 = u(rng) * 2 - 1;
        b.b2 = u(rng) * 2 - 1;
        dsp::SvfParams prm;
        REQUIRE(dsp::svfFromBiquad(b, prm));
        const dsp::SvfCoeffs c = dsp::svfCoeffs(prm);
        dsp::SvfState s;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0, maxErr = 0, maxY = 1e-12;
        for (int n = 0; n < 4000; ++n) {
            const double x = n == 0 ? 1.0 : (n % 97 == 0 ? -0.5 : 0.0);
            const double yd = b.b0 * x + b.b1 * x1 + b.b2 * x2 - b.a1 * y1 - b.a2 * y2;
            x2 = x1, x1 = x, y2 = y1, y1 = yd;
            const double ys = dsp::svfTick(c, s, x);
            maxErr = std::max(maxErr, std::abs(ys - yd));
            maxY = std::max(maxY, std::abs(yd));
        }
        CHECK(maxErr / maxY < 1e-9);
    }
    dsp::Biquad unstable;
    unstable.a1 = 0.0;
    unstable.a2 = 1.01;
    dsp::SvfParams c;
    CHECK_FALSE(dsp::svfFromBiquad(unstable, c));
}

TEST_CASE("Realtime design: every shape, slope, frequency and rate gives a stable cascade") {
    int fallbacks = 0, total = 0;
    for (int sh = 0; sh < dsp::kShapeCount; ++sh)
        for (int sl = 0; sl < dsp::kSlopeCount; ++sl)
            for (double fs : {44100.0, 48000.0, 96000.0, 192000.0, 384000.0})
                for (int fi = 0; fi < 24; ++fi)
                    for (double g : {-30.0, -6.0, 0.0, 12.0, 30.0})
                        for (double q : {0.025, 1.0, 40.0}) {
                            dsp::BandSpec s{static_cast<dsp::Shape>(sh), 10.0 * std::pow(3000.0, fi / 23.0), g, q,
                                            static_cast<dsp::Slope>(sl)};
                            dsp::SvfCascade c;
                            const auto res = dsp::designBandRealtime(s, fs, c);
                            const bool expectSupported = dsp::slopeAllowed(s.shape, s.slope) && s.slope != dsp::Slope::Brickwall;
                            INFO("shape " << sh << " slope " << sl << " fs " << fs << " f " << s.frequencyHz);
                            if (!expectSupported) {
                                CHECK(res == dsp::RealtimeDesignResult::Unsupported);
                                continue;
                            }
                            ++total;
                            REQUIRE(res != dsp::RealtimeDesignResult::Unsupported);
                            fallbacks += res == dsp::RealtimeDesignResult::BilinearFallback;
                            CHECK_FALSE(c.sections.overflowed());
                            for (const auto& sec : c.sections) CHECK(std::isfinite(sec.m0 + sec.m1 + sec.m2 + sec.g + sec.k));
                        }
    MESSAGE("bilinear fallbacks: " << fallbacks << " of " << total << " designs");
}

TEST_CASE("A Bell band applies its designed gain") {
    params::ParameterTable t;
    useBand(t, 0, dsp::Shape::Bell, 1000.0, 6.0, 1.0);
    engine::Engine e(t);
    e.prepare(48000.0, 4800);
    const size_t n = 48000;
    Stereo in(n), out(n);
    for (size_t i = 0; i < n; ++i) in.l[i] = in.r[i] = 0.5f * static_cast<float>(std::sin(2 * 3.14159265358979 * 1000.0 * i / 48000.0));
    for (size_t p = 0; p < n; p += 4800) piece(e, in, out, p, p + 4800);
    const double gain = rmsDb(out.l, 4800) - rmsDb(in.l, 4800);
    // Reference: the same three-point design evaluated directly.
    const auto proto = dsp::designAnalogPrototype({dsp::Shape::Bell, 1000.0, 6.0, 1.0, dsp::Slope::Db12});
    const auto d = dsp::designMatched(proto, 48000.0, dsp::DesignMethod::MatchedThreePoint);
    CHECK(gain == doctest::Approx(20.0 * std::log10(std::abs(d.responseHz(1000.0)))).epsilon(0.002));
    CHECK(gain == doctest::Approx(6.0).epsilon(0.02));  // and close to the analog +6 dB
    CHECK(rmsDb(out.r, 4800) == doctest::Approx(rmsDb(out.l, 4800)));
}

TEST_CASE("Disabled and unused bands are bit-transparent; enabling crossfades") {
    params::ParameterTable t;
    useBand(t, 3, dsp::Shape::Bell, 2000.0, 12.0, 2.0);
    t.set(params::bandParam(3, BandField::Enabled), 0, ParamSource::Preset);
    engine::Engine e(t);
    e.prepare(48000.0, 4096);
    Stereo in(4096), out(4096);
    in.l = noise(4096, 1);
    in.r = noise(4096, 2);
    piece(e, in, out, 0, 4096);
    CHECK(out.l == in.l);
    CHECK(out.r == in.r);
    // Enable: crossfade over ~10 ms, no step at the switch.
    Stereo sine(4096), o2(4096);
    for (size_t i = 0; i < 4096; ++i) sine.l[i] = sine.r[i] = 0.5f * static_cast<float>(std::sin(i * 0.26));
    t.set(params::bandParam(3, BandField::Enabled), 1, ParamSource::Host);
    piece(e, sine, o2, 0, 4096);
    double maxStep = 0;
    for (size_t i = 1; i < 4096; ++i) maxStep = std::max(maxStep, std::abs(double(o2.l[i]) - o2.l[i - 1]));
    CHECK(maxStep < 0.6);  // the sine's own step is about 0.13; a hard switch to +12 dB would be far more
    t.set(params::bandParam(3, BandField::Enabled), 0, ParamSource::Host);
    piece(e, in, out, 0, 4096);  // fade out over 10 ms
    piece(e, in, out, 0, 4096);
    CHECK(out.l == in.l);  // fully off again: bit-transparent
}

TEST_CASE("A band gain step at offset k changes the output at sample k") {
    for (uint32_t k : {1u, 37u, 255u}) {
        auto run = [&](bool change) {
            params::ParameterTable t;
            useBand(t, 0, dsp::Shape::Bell, 3000.0, 3.0, 1.0);
            engine::Engine e(t);
            e.prepare(48000.0, 512);
            Stereo in(512), out(512);
            in.l = noise(512, 3);
            in.r = noise(512, 4);
            piece(e, in, out, 0, k);
            if (change) t.set(params::bandParam(0, BandField::Gain), 9.0, ParamSource::Host);
            piece(e, in, out, k, 512);
            return out.l;
        };
        INFO("offset " << k);
        CHECK(firstDifference(run(false), run(true)) == static_cast<int>(k));
    }
}

TEST_CASE("Block-size robustness with active bands and automation") {
    const size_t n = 9000;
    auto run = [&](const std::vector<uint32_t>& sizes) {
        params::ParameterTable t;
        useBand(t, 0, dsp::Shape::LowShelf, 200.0, 4.0, 0.7);
        useBand(t, 1, dsp::Shape::Bell, 2500.0, -6.0, 3.0, dsp::Slope::Db24);
        useBand(t, 2, dsp::Shape::HighCut, 12000.0, 0.0, 0.7, dsp::Slope::Db48);
        engine::Engine e(t);
        e.prepare(48000.0, 4096);
        Stereo in(n), out(n);
        in.l = noise(n, 7);
        in.r = noise(n, 8);
        const std::vector<size_t> eventsAt = {1000, 1003, 5000};
        size_t pos = 0, k = 0, ev = 0;
        while (pos < n) {
            if (ev < eventsAt.size() && pos == eventsAt[ev]) {
                t.set(params::bandParam(1, BandField::Freq), std::log2(2500.0 + 300.0 * (ev + 1)), ParamSource::Host);
                ++ev;
            }
            size_t end = std::min<size_t>(pos + sizes[k++ % sizes.size()], n);
            if (ev < eventsAt.size() && end > eventsAt[ev]) end = eventsAt[ev];  // split at events
            piece(e, in, out, pos, end);
            pos = end;
        }
        return out.l;
    };
    const auto ref = run({64});
    CHECK(run({1}) == ref);
    CHECK(run({3}) == ref);
    CHECK(run({4096}) == ref);
    CHECK(run({17, 1, 300, 5, 64, 999}) == ref);
}

TEST_CASE("Placement: Left and Right bands touch one channel") {
    params::ParameterTable t;
    useBand(t, 0, dsp::Shape::Bell, 1000.0, 12.0, 1.0, dsp::Slope::Db12, 1);  // Left
    engine::Engine e(t);
    e.prepare(48000.0, 4096);
    Stereo in(4096), out(4096);
    in.l = noise(4096, 9);
    in.r = noise(4096, 10);
    piece(e, in, out, 0, 4096);
    CHECK(out.l != in.l);
    CHECK(out.r == in.r);
}

TEST_CASE("process() never allocates, including redesigns and shape changes") {
    params::ParameterTable t;
    for (int b = 0; b < params::kBandCount; ++b)
        useBand(t, b, static_cast<dsp::Shape>(b % 9), 30.0 * std::pow(2.0, b * 0.4), (b % 7) - 3.0, 1.0,
                static_cast<dsp::Slope>(b % 9));
    engine::Engine e(t);
    e.prepare(96000.0, 256);
    Stereo in(256), out(256);
    in.l = noise(256, 12);
    in.r = noise(256, 13);
    gAllocs = 0;
    gCountAllocs = true;
    for (int blk = 0; blk < 200; ++blk) {
        const int b = blk % params::kBandCount;
        t.set(params::bandParam(b, BandField::Freq), std::log2(50.0 + 97.0 * blk), ParamSource::Host);
        t.set(params::bandParam(b, BandField::Shape), (blk / 3) % 10, ParamSource::Host);
        t.set(params::bandParam(b, BandField::Slope), (blk / 5) % 9, ParamSource::Host);
        piece(e, in, out, 0, 256);
    }
    gCountAllocs = false;
    CHECK(gAllocs.load() == 0);
    CHECK(e.stats().designs > 200);
}

TEST_CASE("Extreme settings stay finite and bounded") {
    for (double fs : {44100.0, 384000.0})
        for (int sh = 0; sh < dsp::kShapeCount; ++sh)
            for (int sl : {0, 8})
                for (double hz : {10.0, 21000.0, 30000.0}) {
                    params::ParameterTable t;
                    useBand(t, 0, static_cast<dsp::Shape>(sh), hz, 30.0, 40.0, static_cast<dsp::Slope>(sl));
                    engine::Engine e(t);
                    e.prepare(fs, 2048);
                    Stereo in(2048), out(2048);
                    in.l = noise(2048, 14);
                    in.r = noise(2048, 15);
                    for (int rep = 0; rep < 4; ++rep) piece(e, in, out, 0, 2048);
                    float peak = 0;
                    bool finite = true;
                    for (float v : out.l) finite &= std::isfinite(v), peak = std::max(peak, std::abs(v));
                    INFO("fs " << fs << " shape " << sh << " slope " << sl << " f " << hz);
                    CHECK(finite);
                    CHECK(peak < 200.0f);  // +30 dB at Q 40 on noise, with headroom
                }
}

TEST_CASE("Zipper: production redesign schedule versus per-sample design during a fast sweep") {
    const size_t n = 48000;
    auto run = [&](uint32_t interval) {
        params::ParameterTable t;
        useBand(t, 0, dsp::Shape::Bell, 100.0, 12.0, 2.0);
        engine::Engine e(t);
        e.setRedesignIntervalForTests(interval);
        if (interval == 1) e.setReadIntervalForTests(1);  // offline ideal: read and design every sample
        e.prepare(48000.0, 64);
        Stereo in(n), out(n);
        for (size_t i = 0; i < n; ++i) in.l[i] = in.r[i] = 0.25f * static_cast<float>(std::sin(2 * 3.14159265358979 * 1000.0 * i / 48000.0));
        for (size_t p = 0; p < n; p += 64) {  // host automation every 64 samples: 100 Hz -> 10 kHz in 0.5 s
            const double frac = std::min(1.0, p / 24000.0);
            t.set(params::bandParam(0, BandField::Freq), std::log2(100.0) + frac * std::log2(100.0), ParamSource::Host);
            piece(e, in, out, p, p + 64);
        }
        return out.l;
    };
    const auto coarse = run(engine::kRedesignInterval), fine = run(1);
    double err = 0;
    for (size_t i = 0; i < n; ++i) err = std::max(err, std::abs(double(coarse[i]) - fine[i]));
    const double residualDb = 20.0 * std::log10(std::max(err, 1e-12));
    MESSAGE("zipper residual (production vs per-sample design), peak: " << residualDb << " dBFS");
    CHECK(residualDb < -90.0);  // area 01 R15 / area 09 zipper criterion
}


TEST_CASE("CPU: 24 bands, 48 kHz, 64-sample blocks (report only)") {
    const dsp::Shape shapes[] = {dsp::Shape::Bell, dsp::Shape::LowShelf, dsp::Shape::HighShelf, dsp::Shape::Notch};
    auto measure = [&](bool refined) {
        params::ParameterTable t;
        for (int b = 0; b < params::kBandCount; ++b)
            useBand(t, b, shapes[b % 4], 40.0 * std::pow(2.0, b * 0.35), (b % 5) - 2.0, 1.5, dsp::Slope::Db12);
        engine::Engine e(t);
        e.prepare(48000.0, 64);
        Stereo in(64), out(64);
        in.l = noise(64, 21);
        in.r = noise(64, 22);
        for (int i = 0; i < 100; ++i) {  // settle; optionally swap in the refined designs
            piece(e, in, out, 0, 64);
            if (refined) e.runDesignJobsForTests();
        }
        CHECK(e.stats().refinedSwaps == (refined ? 24u : 0u));
        const int blocks = 48000 / 64 * 5;  // 5 s of audio
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < blocks; ++i) piece(e, in, out, 0, 64);
        return 100.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 5.0;
    };
    double fast = 1e9, refined = 1e9;
    for (int rep = 0; rep < 3; ++rep) {  // best of three, alternating, to damp machine noise
        fast = std::min(fast, measure(false));
        refined = std::min(refined, measure(true));
    }
    MESSAGE("24 static 12 dB/oct bands, stereo, % of one core (best of 3; target <= 3 %, area 01): fast design "
            << fast << ", refined design " << refined);
}
