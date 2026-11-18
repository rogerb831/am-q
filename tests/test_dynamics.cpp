// Dynamic EQ timing harness (area 06 task "Build a measurement harness that applies dB steps to
// the side chain and reports 90 % settling times") and the area 06 / 07 / 08 acceptance rows that
// concern the dynamics core: attack and release ranges, frequency scaling, program-dependent
// release, sample-rate independence, range and offset caps, the knee, and the auto-threshold
// reference envelope (instant attack, release, warm-up, floor, independence from the knobs).
#include "amq/dsp/dynamics.hpp"
#include "amq/dsp/helper_filters.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace amq::dsp;

namespace {

// Feeds `before` dB of target until settled, then `after`, and returns the 90 % time of the move
// in seconds (crossing interpolated between samples).
double settle90(BandDynamics& d, double fs, double before, double after, double holdBeforeS = 3.0) {
    d.reset();
    const auto hold = static_cast<size_t>(holdBeforeS * fs);
    for (size_t i = 0; i < hold; ++i) d.process(before);
    const double start = d.value();
    const double goal = start + 0.9 * (after - start);
    double prev = start;
    for (size_t n = 1; n < static_cast<size_t>(20.0 * fs); ++n) {
        const double v = d.process(after);
        const bool crossed = after > start ? v >= goal : v <= goal;
        if (crossed) return (static_cast<double>(n - 1) + (goal - prev) / (v - prev)) / fs;
        prev = v;
    }
    return -1.0;
}

double attackTime(double knobA, double knobR, double hz, double fs) {
    BandDynamics d;
    d.prepare(fs);
    d.setTimes(attackSeconds(knobA, hz), releaseSeconds(knobR));
    return settle90(d, fs, 0.0, 20.0, 0.0);
}

double releaseTime(double knobA, double knobR, double hz, double fs) {
    BandDynamics d;
    d.prepare(fs);
    d.setTimes(attackSeconds(knobA, hz), releaseSeconds(knobR));
    return settle90(d, fs, 20.0, 0.0, 3.0);  // after sustained reduction
}

}  // namespace

TEST_CASE("Timing spec: tapers, frequency scaling and minimum cycles") {
    CHECK(knobTaperMs(0, kAttackMinMs, kAttackMidMs, kAttackMaxMs) == doctest::Approx(0.25));
    CHECK(knobTaperMs(50, kAttackMinMs, kAttackMidMs, kAttackMaxMs) == doctest::Approx(10.0));
    CHECK(knobTaperMs(100, kAttackMinMs, kAttackMidMs, kAttackMaxMs) == doctest::Approx(250.0));
    CHECK(releaseSeconds(0) == doctest::Approx(0.005));
    CHECK(releaseSeconds(50) == doctest::Approx(0.25));
    CHECK(releaseSeconds(100) == doctest::Approx(1.0));
    double prevA = 0.0, prevR = 0.0;  // monotonic (R1)
    for (int k = 0; k <= 100; ++k) {
        CHECK(attackSeconds(k, 20000) > prevA);  // at 20 kHz the minimum-cycles rule never binds
        CHECK(attackSeconds(k, 1000) >= attackSeconds(k - 1 < 0 ? 0 : k - 1, 1000));
        CHECK(releaseSeconds(k) > prevR);
        prevA = attackSeconds(k, 20000), prevR = releaseSeconds(k);
    }
    CHECK(attackSeconds(0, 5000) <= 0.0003);    // acceptance: knob 0 <= 0.3 ms at 5 kHz
    CHECK(attackSeconds(100, 100) >= 0.25);     // knob 100 >= 250 ms at 100 Hz
    CHECK(attackSeconds(0, 30) == doctest::Approx(1.0 / 30.0));  // minimum-cycles rule binds on bass
}

TEST_CASE("Attack and release ranges within 5 % of the spec at 44.1, 48, 96 and 192 kHz") {
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0})
        for (double hz : {100.0, 1000.0, 5000.0})
            for (double knob : {0.0, 25.0, 50.0, 100.0}) {
                INFO("fs " << fs << " band " << hz << " Hz knob " << knob);
                const double a = attackTime(knob, 50, hz, fs);
                CHECK(a == doctest::Approx(attackSeconds(knob, hz)).epsilon(0.05));
                const double r = releaseTime(50, knob, hz, fs);
                CHECK(r == doctest::Approx(releaseSeconds(knob)).epsilon(0.05));
            }
    CHECK(releaseTime(50, 100, 1000, 48000) >= 0.95);  // maximum about 1 s
}

TEST_CASE("Frequency scaling: the 100 Hz attack is the documented ratio longer than at 10 kHz") {
    for (double knob : {50.0, 80.0, 100.0}) {  // where the minimum-cycles rule does not bind at 100 Hz (knob >= 42)
        const double ratio = attackTime(knob, 50, 100, 48000) / attackTime(knob, 50, 10000, 48000);
        INFO("knob " << knob << " ratio " << ratio);
        CHECK(ratio == doctest::Approx(std::pow(100.0, kAttackFreqExponent)).epsilon(0.05));
    }
}

TEST_CASE("Program-dependent release: a 5 ms burst recovers fast, a 2 s tone at the Release time") {
    const double fs = 48000;
    auto recovery = [&](double burstS) {
        BandDynamics d;
        d.prepare(fs);
        d.setTimes(attackSeconds(10, 1000), releaseSeconds(70));  // 1.9 ms attack, 437 ms release
        for (size_t i = 0; i < static_cast<size_t>(burstS * fs); ++i) d.process(20.0);
        const double peak = d.value();
        for (size_t n = 1; n < static_cast<size_t>(10 * fs); ++n)
            if (d.process(0.0) <= 0.1 * peak) return double(n) / fs;
        return -1.0;
    };
    const double burst = recovery(0.005), held = recovery(2.0);
    INFO("burst recovers in " << burst * 1e3 << " ms, sustained in " << held * 1e3 << " ms");
    CHECK(burst == doctest::Approx(kFastReleaseMs * 1e-3).epsilon(0.1));
    CHECK(held == doctest::Approx(releaseSeconds(70)).epsilon(0.05));
}

TEST_CASE("Gain computer: soft knee, one dB per dB over, capped at the range") {
    const double knee = 6.0;
    CHECK(gainComputerDb(-10, knee, 30) == 0.0);
    CHECK(gainComputerDb(-3, knee, 30) == doctest::Approx(0.0));
    CHECK(gainComputerDb(0, knee, 30) == doctest::Approx(0.75));  // (W/2)^2 / 2W = W/8
    CHECK(gainComputerDb(3, knee, 30) == doctest::Approx(3.0));   // joins the line at the knee edge
    CHECK(gainComputerDb(10, knee, 30) == doctest::Approx(10.0));
    CHECK(gainComputerDb(60, knee, 30) == 30.0);  // range cap (R7)
    CHECK(gainComputerDb(60, 0.0, 12) == 12.0);
    double prev = 0.0;  // continuous and monotonic
    for (double o = -20; o <= 40; o += 0.01) {
        const double g = gainComputerDb(o, knee, 30);
        CHECK(g >= prev - 1e-12);
        CHECK(g - prev < 0.011);
        prev = g;
    }
    CHECK(kneeWidthDb(1.0, 6.0) == doctest::Approx(3.0 + 1.5 + 1.2));
}

TEST_CASE("Auto threshold: offset is |range|, capped at 30 dB") {
    CHECK(autoThresholdDb(-10, -6) == -16.0);
    CHECK(autoThresholdDb(-10, 6) == -16.0);
    CHECK(autoThresholdDb(-10, -45) == -40.0);  // a range scaled past 30 dB (Gain Scale)
}

TEST_CASE("Reference envelope: instant attack, 10 s release, 0.5 s warm-up, floor") {
    const double fs = 48000;
    ReferenceEnvelope r;
    r.prepare(fs);
    // Floor: silence and very low levels hold -100 dB.
    for (int i = 0; i < 1000; ++i) r.process(detectorLevelDb(0.0));
    CHECK(r.value() == kRefFloorDb);
    for (int i = 0; i < 1000; ++i) r.process(detectorLevelDb(1e-6));  // -120 dBFS
    CHECK(r.value() == kRefFloorDb);
    // Instant attack: a +20 dB step lands on the first sample.
    r.process(-40.0);
    r.process(-20.0);
    CHECK(r.value() == -20.0);
    // Warm-up: settles to a lower level within 0.5 s.
    r.reset();
    r.process(-10.0);
    size_t n = 0;
    while (r.process(-40.0) > -40.0 + 0.1 * 30.0 && n < 100000) ++n;
    CHECK(double(n) / fs < 0.5);
    // Release after warm-up: a -20 dB step after 5 s of signal falls in 10 s (90 %), +-5 %.
    r.reset();
    for (size_t i = 0; i < static_cast<size_t>(5 * fs); ++i) r.process(-10.0);
    CHECK_FALSE(r.warmingUp());
    n = 0;
    while (r.process(-30.0) > -10.0 - 0.9 * 20.0 && n < static_cast<size_t>(30 * fs)) ++n;
    CHECK(double(n) / fs == doctest::Approx(kRefReleaseSeconds).epsilon(0.05));
}

TEST_CASE("Detector level floor and finite results on garbage") {
    CHECK(detectorLevelDb(0.0) == kLevelFloorDb);
    CHECK(detectorLevelDb(1.0) == doctest::Approx(0.0));
    BandDynamics d;
    d.prepare(48000);
    d.setTimes(0.001, 0.1);
    d.process(5.0);
    CHECK(std::isfinite(d.process(std::nan(""))));
    ReferenceEnvelope r;
    r.prepare(48000);
    CHECK(std::isfinite(r.process(std::nan(""))));
}

TEST_CASE("Quadrature envelope of the band-pass side chain: flat on a tone at the centre") {
    const double fs = 48000.0, pi = 3.14159265358979323846;
    for (double hz : {60.0, 1000.0, 12000.0})
        for (double q : {0.5, 2.0, 10.0}) {
            const SvfCoeffs c = svfCoeffs(helperBandPass(hz, q, fs));
            SvfState s;
            double lo = 1e9, hi = 0.0;
            const auto n = static_cast<size_t>(fs);
            for (size_t i = 0; i < n; ++i) {
                const double e = svfBandEnvelope(c, s, 0.5 * std::sin(2.0 * pi * hz * double(i) / fs));
                if (i > n / 2) lo = std::min(lo, e), hi = std::max(hi, e);
            }
            INFO(hz << " Hz, Q " << q << ": envelope " << lo << " .. " << hi);
            CHECK(lo == doctest::Approx(0.5).epsilon(1e-4));  // 0.001 dB (a Q 10 band at 60 Hz still rings out its start)
            CHECK(hi == doctest::Approx(0.5).epsilon(1e-4));
        }
}

#include "amq/dsp/dynamics_lanes.hpp"

#include <random>
#include <vector>

TEST_CASE("fastLog2 matches std::log2 to a few ulps, and powers of two exactly") {
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> u(-60.0, 60.0);
    double worst = 0.0;
    for (int i = 0; i < 200000; ++i) {
        const double x = std::exp2(u(rng));
        const double ref = std::log2(x);
        worst = std::max(worst, std::abs(amq::dsp::fastLog2(x) - ref) / std::max(std::abs(ref), 1.0));
    }
    CHECK(worst < 1e-15);
    for (int k = -40; k <= 40; ++k) CHECK(amq::dsp::fastLog2(std::ldexp(1.0, k)) == static_cast<double>(k));
    CHECK(amq::dsp::detectorLevelDb(0.0) == amq::dsp::kLevelFloorDb);
    CHECK(amq::dsp::detectorLevelDb(1.0) == 0.0);
}

namespace {
amq::dsp::GainLane makeLane(std::mt19937& rng, bool autoThr, double knee, double range) {
    using namespace amq::dsp;
    ReferenceEnvelope r;
    r.prepare(48000.0);
    BandDynamics d;
    d.prepare(48000.0);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    d.setTimes(attackSeconds(100.0 * u(rng), 1000.0), releaseSeconds(100.0 * u(rng)));
    GainLane L;
    L.load(r, d);
    L.autoThr = autoThr, L.thresholdDb = -30.0 * u(rng), L.offsetDb = std::min(std::abs(range), kAutoOffsetCapDb);
    L.kneeDb = knee, L.capDb = std::abs(range), L.inv = 1.0 / std::abs(range);
    L.refCount = static_cast<uint64_t>(30000 * u(rng));  // some lanes still warming up
    return L;
}
std::vector<double> envelope(std::mt19937& rng, uint32_t n) {
    std::uniform_real_distribution<double> u(-120.0, 6.0);
    std::vector<double> e(n);
    for (auto& v : e) v = std::pow(10.0, u(rng) / 20.0);
    e[n / 3] = 0.0;  // below the detector floor
    return e;
}
}  // namespace

TEST_CASE("Dynamics lanes: the SSE2 pair gives the same bits as two scalar lanes") {
    std::mt19937 rng(21);
    for (int trial = 0; trial < 40; ++trial) {
        const uint32_t n = 1 + static_cast<uint32_t>(trial * 37 % 500);
        auto A = makeLane(rng, trial % 2 == 0, trial % 3 ? 6.0 : 0.0, trial % 2 ? -6.0 : 12.0);
        auto B = makeLane(rng, trial % 4 < 2, 4.5, -18.0);
        auto ea = envelope(rng, n), eb = envelope(rng, n);
        auto sa = A, sb = B;
        auto xa = ea, xb = eb;
        amq::dsp::gainLanePair(A, B, ea.data(), eb.data(), n);
        amq::dsp::gainLaneScalar(sa, xa.data(), n);
        amq::dsp::gainLaneScalar(sb, xb.data(), n);
        CHECK(ea == xa);
        CHECK(eb == xb);
        CHECK((A.ref == sa.ref && A.refCount == sa.refCount && A.fy == sa.fy && A.sy == sa.sy && A.zy == sa.zy));
        CHECK((B.ref == sb.ref && B.refCount == sb.refCount && B.zy == sb.zy));
    }
}

TEST_CASE("Dynamics lanes: the AVX2 quad gives the same bits as four scalar lanes") {
    if (!amq::dsp::cpuHasAvx2()) {
        MESSAGE("no AVX2 on this CPU: skipped");
        return;
    }
    std::mt19937 rng(31);
    for (int trial = 0; trial < 20; ++trial) {
        const uint32_t n = 1 + static_cast<uint32_t>(trial * 53 % 600);
        amq::dsp::GainLane q[4], s1[4];
        std::vector<double> e[4], x[4];
        for (int k = 0; k < 4; ++k) {
            q[k] = s1[k] = makeLane(rng, (trial + k) % 2 == 0, k == 3 ? 0.0 : 4.0 + k, -3.0 - 4.0 * k);
            e[k] = x[k] = envelope(rng, n);
        }
        e[1][0] = x[1][0] = std::numeric_limits<double>::infinity();  // an infinite envelope sample
        amq::dsp::GainLane* const L[4] = {&q[0], &q[1], &q[2], &q[3]};
        double* const B[4] = {e[0].data(), e[1].data(), e[2].data(), e[3].data()};
        amq::dsp::gainLaneQuadAvx2(L, B, n);
        for (int k = 0; k < 4; ++k) {
            amq::dsp::gainLaneScalar(s1[k], x[k].data(), n);
            CHECK(e[k] == x[k]);
            CHECK((q[k].ref == s1[k].ref && q[k].refCount == s1[k].refCount && q[k].zy == s1[k].zy));
        }
    }
}

TEST_CASE("Dynamics lanes: the scalar lane equals the object chain it replaces") {
    using namespace amq::dsp;
    std::mt19937 rng(5);
    for (bool autoThr : {true, false}) {
        ReferenceEnvelope r;
        r.prepare(48000.0);
        BandDynamics d;
        d.prepare(48000.0);
        d.setTimes(attackSeconds(30.0, 500.0), releaseSeconds(70.0));
        const double range = -9.0, knee = kneeWidthDb(1.0, range), thrDb = -24.0;
        GainLane L;
        L.load(r, d);
        L.autoThr = autoThr, L.thresholdDb = thrDb, L.offsetDb = std::min(std::abs(range), kAutoOffsetCapDb);
        L.kneeDb = knee, L.capDb = std::abs(range), L.inv = 1.0 / std::abs(range);
        auto e = envelope(rng, 3000);
        std::vector<double> expect(e.size());
        for (size_t i = 0; i < e.size(); ++i) {
            const double level = detectorLevelDb(e[i]);
            const double thr = autoThr ? autoThresholdDb(r.process(level), range) : thrDb;
            expect[i] = std::min(d.process(gainComputerDb(level - thr, knee, std::abs(range))) * (1.0 / std::abs(range)), 1.0);
        }
        gainLaneScalar(L, e.data(), static_cast<uint32_t>(e.size()));
        CHECK(e == expect);
        CHECK(L.ref == r.value());
        CHECK(L.zy == d.value());
    }
}

#include <chrono>
#include <cstdio>

TEST_CASE("Dynamics lanes: cost report (24 bands, 1 s at 48 kHz)" * doctest::skip()) {
    std::mt19937 rng(3);
    std::vector<amq::dsp::GainLane> lanes;
    for (int b = 0; b < 24; ++b) lanes.push_back(makeLane(rng, true, 6.0, -6.0));
    std::vector<std::vector<double>> env;
    for (int b = 0; b < 24; ++b) env.push_back(envelope(rng, 64));
    auto run = [&](bool pairs) {
        auto L = lanes;
        std::vector<double> buf(64);
        const auto t0 = std::chrono::steady_clock::now();
        for (int blk = 0; blk < 750; ++blk)
            for (int b = 0; b < 24; b += pairs ? 2 : 1) {
                auto a = env[b];
                if (pairs) {
                    auto c = env[b + 1];
                    amq::dsp::gainLanePair(L[b], L[b + 1], a.data(), c.data(), 64);
                } else {
                    amq::dsp::gainLaneScalar(L[b], a.data(), 64);
                }
            }
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    auto runAvx = [&] {
        auto L = lanes;
        const auto t0 = std::chrono::steady_clock::now();
        for (int blk = 0; blk < 750; ++blk)
            for (int b = 0; b < 24; b += 4) {
                auto a = env[b], c = env[b + 1], d = env[b + 2], f = env[b + 3];
                amq::dsp::GainLane* const P[4] = {&L[b], &L[b + 1], &L[b + 2], &L[b + 3]};
                double* const B[4] = {a.data(), c.data(), d.data(), f.data()};
                amq::dsp::gainLaneQuadAvx2(P, B, 64);
            }
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    std::printf("  dB chain, scalar: %.2f %% of one core; SSE2 pairs: %.2f %%; AVX2 quads: %.2f %%%s\n", 100.0 * run(false),
                100.0 * run(true), 100.0 * runAvx(), amq::dsp::cpuHasAvx2() ? "" : " (no AVX2: scalar fallback)");
}

TEST_CASE("Dynamics lanes: a lane set step gives the same bits as each lane through the scalar kernel") {
    std::mt19937 rng(77);
    for (int trial = 0; trial < 12; ++trial) {
        const size_t count = 1 + static_cast<size_t>(trial * 7 % 23);  // odd counts exercise the scalar tail
        amq::dsp::GainLaneSet set;
        set.resize(count);
        std::vector<amq::dsp::GainLane> ref;
        for (size_t i = 0; i < count; ++i) {
            auto L = makeLane(rng, (i + trial) % 3 != 0, i % 4 ? 5.5 : 0.0, i % 2 ? -9.0 : 14.0);
            L.tiltDb = i % 3 == 0 ? 0.0 : 1.5 * (static_cast<double>(i) - 4.0);
            set.setLane(i, L);
            ref.push_back(L);
        }
        for (int step = 0; step < 200; ++step) {
            const auto env = envelope(rng, static_cast<uint32_t>(count) + 3);
            amq::dsp::gainLanesStep(set, env.data(), count);
            for (size_t i = 0; i < count; ++i) {
                double x = env[i];
                amq::dsp::gainLaneScalar(ref[i], &x, 1);
                const auto L = set.lane(i);
                REQUIRE((L.zy == ref[i].zy && L.ref == ref[i].ref && L.refCount == ref[i].refCount && L.fy == ref[i].fy &&
                         L.sy == ref[i].sy));
            }
        }
    }
}
