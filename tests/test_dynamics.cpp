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
