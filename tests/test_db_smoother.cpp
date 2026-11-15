// dB-domain attack/release smoother (area 06 R9, R11; acceptance "Sample-rate independence",
// "Fuzz").
#include "amq/dsp/db_smoother.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <random>
#include <vector>

using namespace amq::dsp;

namespace {

// Time (s) for the smoother to cover 90 % of a step from `from` to `to`, with the crossing
// interpolated between samples (the step is applied at t = 0; sample n is at t = (n+1)/fs).
double measured90(DbAttackRelease s, double fs, double from, double to) {
    s.reset(from);
    const double mark = from + 0.9 * (to - from);
    double prev = from;
    for (long n = 0; n < static_cast<long>(20.0 * fs); ++n) {
        const double y = s.process(to);
        const bool crossed = to > from ? y >= mark : y <= mark;
        if (crossed) {
            const double frac = (mark - prev) / (y - prev);
            return (static_cast<double>(n) + frac) / fs;
        }
        prev = y;
    }
    return -1.0;
}

}  // namespace

TEST_CASE("Attack and release cover 90 % of a dB step in the stated time at every rate (R9, R11)") {
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        for (double t : {0.00025, 0.001, 0.010, 0.250}) {
            DbAttackRelease s;
            s.prepare(fs);
            s.setTimes(t, 0.5);
            const double up = measured90(s, fs, 0.0, 20.0);  // more gain reduction: attack
            CHECK(up == doctest::Approx(t).epsilon(0.05));
        }
        for (double t : {0.005, 0.100, 1.0}) {
            DbAttackRelease s;
            s.prepare(fs);
            s.setTimes(0.001, t);
            const double down = measured90(s, fs, 20.0, 0.0);  // recovering: release
            CHECK(down == doctest::Approx(t).epsilon(0.05));
        }
    }
    // The discrete smoother lands exactly on 90 % at a whole number of samples.
    DbAttackRelease s;
    s.prepare(48000.0);
    s.setTimes(480.0 / 48000.0, 1.0);
    s.reset(0.0);
    double y = 0.0;
    for (int i = 0; i < 480; ++i) y = s.process(10.0);
    CHECK(y == doctest::Approx(9.0).epsilon(1e-9));
}

TEST_CASE("Sample-rate independence: measured times agree with 48 kHz within 5 %") {
    auto at = [](double fs, bool attack) {
        DbAttackRelease s;
        s.prepare(fs);
        s.setTimes(0.003, 0.2);
        return attack ? measured90(s, fs, 0.0, 12.0) : measured90(s, fs, 12.0, 0.0);
    };
    for (bool attack : {true, false}) {
        const double ref = at(48000.0, attack);
        for (double fs : {44100.0, 96000.0, 192000.0}) CHECK(at(fs, attack) == doctest::Approx(ref).epsilon(0.05));
    }
}

TEST_CASE("Time 0 is instant in either direction") {
    DbAttackRelease s;
    s.prepare(48000.0);
    s.setTimes(0.0, 0.0);
    CHECK(s.process(12.0) == 12.0);
    CHECK(s.process(3.0) == 3.0);
    s.setTimes(0.0, 0.1);
    CHECK(s.process(20.0) == 20.0);  // instant attack
    CHECK(s.process(0.0) < 20.0);    // release still smooth
    CHECK(s.value() > 0.0);
    s.setTimes(0.1, 0.0);
    CHECK(s.process(0.0) == 0.0);  // instant release
}

TEST_CASE("Direction picks the coefficient, and changing times does not move the value") {
    DbAttackRelease s;
    s.prepare(48000.0);
    s.setTimes(0.001, 0.5);
    s.reset(0.0);
    const double a = s.process(10.0);
    CHECK(a == doctest::Approx(10.0 * s.attackCoefficient()));
    s.reset(10.0);
    const double r = s.process(0.0);
    CHECK(10.0 - r == doctest::Approx(10.0 * s.releaseCoefficient()));
    CHECK(s.attackCoefficient() > s.releaseCoefficient());
    const double before = s.value();
    s.setTimes(0.02, 0.05);  // moving a knob mid-release: no step in the value (R12)
    CHECK(s.value() == before);
}

TEST_CASE("Coefficients are recomputed only when a time or the rate changes") {
    DbAttackRelease s;
    s.prepare(48000.0);
    s.setTimes(0.01, 0.2);
    const auto n = s.coefficientUpdates();
    for (int i = 0; i < 1000; ++i) s.setTimes(0.01, 0.2);
    s.prepare(48000.0);
    CHECK(s.coefficientUpdates() == n);
    s.setTimes(0.011, 0.2);
    s.prepare(96000.0);
    CHECK(s.coefficientUpdates() == n + 2);
}

TEST_CASE("Block processing equals per-sample processing") {
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> u(0.0, 30.0);
    std::vector<double> in(4096), out(4096);
    for (auto& v : in) v = u(rng);
    DbAttackRelease a, b;
    for (auto* s : {&a, &b}) {
        s->prepare(44100.0);
        s->setTimes(0.002, 0.08);
    }
    a.processBlock(in.data(), out.data(), in.size());
    for (size_t i = 0; i < in.size(); ++i) CHECK(b.process(in[i]) == out[i]);
}

TEST_CASE("Fuzz: garbage targets and extreme times stay finite; settling leaves no denormal tail") {
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    const double specials[] = {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(), 1e300, -1e300, 0.0};
    DbAttackRelease s;
    s.prepare(48000.0);
    bool finite = true;
    for (int i = 0; i < 200000; ++i) {
        if (i % 997 == 0) s.setTimes(std::abs(u(rng)) * 2.0, std::abs(u(rng)) * 2.0);
        if (i % 50000 == 0) s.setTimes(0.0, 1e9);
        const double x = (i % 13 == 0) ? specials[static_cast<size_t>(i / 13) % 6] : u(rng) * 60.0;
        finite &= std::isfinite(s.process(x));
    }
    CHECK(finite);
    // Release back to 0 dB: the value reaches exactly 0 and stays there.
    s.setTimes(0.001, 0.010);
    s.reset(30.0);
    for (int i = 0; i < 48000; ++i) s.process(0.0);
    CHECK(s.value() == 0.0);
    CHECK(std::fpclassify(s.value()) != FP_SUBNORMAL);
}
