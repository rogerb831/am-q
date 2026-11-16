#include <doctest/doctest.h>

#include "amq/dsp/analog_prototype.hpp"
#include "amq/dsp/conventions.hpp"

#include <cmath>
#include <initializer_list>
#include <complex>

using namespace amq::dsp;
using cd = std::complex<double>;

namespace {
double magDb(const AnalogPrototype& p, double omega) { return linearToDb(std::abs(p.evaluate(omega))); }

const Slope kAllSlopes[] = {Slope::Db6, Slope::Db12, Slope::Db18, Slope::Db24, Slope::Db30,
                            Slope::Db36, Slope::Db48, Slope::Db72, Slope::Db96};
}  // namespace

TEST_CASE("order = slope / 6, minimum 2 for Bell, Band Pass, Notch (R10)") {
    CHECK(filterOrder(Shape::LowCut, Slope::Db6) == 1);
    CHECK(filterOrder(Shape::LowCut, Slope::Db96) == 16);
    CHECK(filterOrder(Shape::HighShelf, Slope::Db18) == 3);
    CHECK(filterOrder(Shape::Bell, Slope::Db6) == 2);
    CHECK(filterOrder(Shape::Notch, Slope::Db6) == 2);
    CHECK(filterOrder(Shape::BandPass, Slope::Db6) == 2);
    CHECK(filterOrder(Shape::Bell, Slope::Db48) == 8);
    CHECK(slopeAllowed(Shape::LowCut, Slope::Brickwall));
    CHECK(slopeAllowed(Shape::HighCut, Slope::Brickwall));
    CHECK_FALSE(slopeAllowed(Shape::Bell, Slope::Brickwall));
    CHECK(designAnalogPrototype({Shape::LowCut, 1000.0, 0.0, 1.0, Slope::Brickwall}).supported);
    CHECK(designAnalogPrototype({Shape::LowCut, 1000.0, 0.0, 1.0, Slope::Brickwall}).brickwall);
    CHECK(designAnalogPrototype({Shape::LowCut, 1000.0, 0.0, 1.0, Slope::Brickwall}).order == kBrickwallOrder);
    CHECK_FALSE(designAnalogPrototype({Shape::Bell, 1000.0, 0.0, 1.0, Slope::Brickwall}).supported);
}

TEST_CASE("cascade order: sections carry exactly 'order' poles") {
    for (Slope sl : kAllSlopes) {
        for (Shape sh : {Shape::LowCut, Shape::HighCut, Shape::LowShelf, Shape::HighShelf, Shape::TiltShelf, Shape::AllPass}) {
            const auto p = designAnalogPrototype({sh, 1000.0, 6.0, 1.0, sl});
            int poles = 0;
            for (const auto& s : p.sections) poles += s.a2 != 0.0 ? 2 : (s.a1 != 0.0 ? 1 : 0);
            CHECK(poles == filterOrder(sh, sl));
        }
        for (Shape sh : {Shape::Bell, Shape::Notch, Shape::BandPass}) {
            const auto p = designAnalogPrototype({sh, 1000.0, 6.0, 1.0, sl});
            int poles = 0;
            for (const auto& s : p.sections) poles += s.a2 != 0.0 ? 2 : (s.a1 != 0.0 ? 1 : 0);
            CHECK(poles == 2 * (filterOrder(sh, sl) / 2));  // band-pass mapping of order/2
        }
    }
}

TEST_CASE("Bell: gain at centre, unity far away, matches the 2nd-order analog bell") {
    for (Slope sl : kAllSlopes) {
        for (double g : {-30.0, -6.0, 6.0, 30.0}) {
            const auto p = designAnalogPrototype({Shape::Bell, 1000.0, g, 2.0, sl});
            CHECK(magDb(p, 1.0) == doctest::Approx(g).epsilon(1e-9));
            CHECK(std::abs(magDb(p, 1e-4)) < 1e-3);
            CHECK(std::abs(magDb(p, 1e4)) < 1e-3);
        }
    }
    // Independent reference: H(s) = (s^2 + s*A/Q + 1) / (s^2 + s/(A*Q) + 1), A = 10^(g/40)
    // (RBJ cookbook analog peaking prototype).
    const double g = 9.0, q = 1.7, A = std::pow(10.0, g / 40.0);
    const auto p = designAnalogPrototype({Shape::Bell, 1000.0, g, q, Slope::Db12});
    for (double w = 0.01; w < 100.0; w *= 1.13) {
        const cd s{0.0, w};
        const cd ref = (s * s + s * (A / q) + 1.0) / (s * s + s / (A * q) + 1.0);
        CHECK(std::abs(p.evaluate(w) - ref) / std::abs(ref) < 1e-9);  // area 04: <= 1e-9 relative
    }
}

TEST_CASE("Shelves: DC/HF gains and half gain at the centre") {
    for (Slope sl : kAllSlopes) {
        for (double g : {-24.0, -3.0, 3.0, 24.0}) {
            const auto ls = designAnalogPrototype({Shape::LowShelf, 500.0, g, 1.0, sl});
            CHECK(magDb(ls, 1e-5) == doctest::Approx(g).epsilon(1e-6));
            CHECK(std::abs(magDb(ls, 1e5)) < 1e-6);
            CHECK(magDb(ls, 1.0) == doctest::Approx(g / 2).epsilon(1e-9));
            const auto hs = designAnalogPrototype({Shape::HighShelf, 500.0, g, 1.0, sl});
            CHECK(magDb(hs, 1e5) == doctest::Approx(g).epsilon(1e-6));
            CHECK(std::abs(magDb(hs, 1e-5)) < 1e-6);
            CHECK(magDb(hs, 1.0) == doctest::Approx(g / 2).epsilon(1e-9));
            const auto ts = designAnalogPrototype({Shape::TiltShelf, 500.0, g, 1.0, sl});
            CHECK(magDb(ts, 1e-5) == doctest::Approx(-g / 2).epsilon(1e-6));
            CHECK(magDb(ts, 1e5) == doctest::Approx(g / 2).epsilon(1e-6));
            CHECK(std::abs(magDb(ts, 1.0)) < 1e-9);
        }
    }
}

TEST_CASE("Cuts: Butterworth at Q = 1, -3 dB at f0, slope = order * 6 dB/oct") {
    for (Slope sl : kAllSlopes) {
        const int n = filterOrder(Shape::LowCut, sl);
        const auto hp = designAnalogPrototype({Shape::LowCut, 100.0, 0.0, 1.0, sl});
        const auto lp = designAnalogPrototype({Shape::HighCut, 100.0, 0.0, 1.0, sl});
        CHECK(magDb(hp, 1.0) == doctest::Approx(-10.0 * std::log10(2.0)).epsilon(1e-9));
        CHECK(magDb(lp, 1.0) == doctest::Approx(-10.0 * std::log10(2.0)).epsilon(1e-9));
        CHECK(std::abs(magDb(hp, 1e4)) < 1e-6);
        CHECK(std::abs(magDb(lp, 1e-4)) < 1e-6);
        // Asymptotic slope over one octave, far into the stop band.
        const double slope = magDb(lp, 2000.0) - magDb(lp, 1000.0);
        CHECK(slope == doctest::Approx(-20.0 * n * std::log10(2.0)).epsilon(1e-4));
    }
}

TEST_CASE("Notch, Band Pass, All Pass") {
    for (Slope sl : kAllSlopes) {
        const auto notch = designAnalogPrototype({Shape::Notch, 1000.0, 0.0, 3.0, sl});
        CHECK(std::abs(notch.evaluate(1.0)) < 1e-9);
        CHECK(std::abs(magDb(notch, 1e-4)) < 1e-6);
        const auto bp = designAnalogPrototype({Shape::BandPass, 1000.0, 0.0, 3.0, sl});
        CHECK(std::abs(magDb(bp, 1.0)) < 1e-9);
        const auto ap = designAnalogPrototype({Shape::AllPass, 1000.0, 0.0, 0.7, sl});
        for (double w = 0.01; w < 100.0; w *= 1.5) CHECK(std::abs(std::abs(ap.evaluate(w)) - 1.0) < 1e-12);
    }
}

TEST_CASE("Flat Tilt: pivot at f0 is 0 dB and slope is roughly constant") {
    const auto ft = designAnalogPrototype({Shape::FlatTilt, 1000.0, 20.0, 1.0, Slope::Db12});
    CHECK(std::abs(magDb(ft, 1.0)) < 1e-9);
    const double perOct = 20.0 / std::log2(1000.0);
    // Away from the range edges, each octave rises by about perOct dB (ripple < 0.1 dB).
    for (double f = 100.0; f <= 4000.0; f *= 2.0) {
        const double rise = magDb(ft, 2.0 * f / 1000.0) - magDb(ft, f / 1000.0);
        CHECK(rise == doctest::Approx(perOct).epsilon(0.05));
    }
    // Gain at the 1 kHz reference stays within +/-30 dB.
    const auto ftLow = designAnalogPrototype({Shape::FlatTilt, 10.0, 30.0, 1.0, Slope::Db12});
    CHECK(std::abs(magDb(ftLow, 1000.0 / 10.0)) <= 30.0 + 1e-9);
}

TEST_CASE("Gain and Q are clamped to the R8/R13 ranges") {
    const auto p = designAnalogPrototype({Shape::Bell, 1000.0, 45.0, 1.0, Slope::Db12});
    CHECK(magDb(p, 1.0) == doctest::Approx(30.0));
    const auto q = designAnalogPrototype({Shape::Bell, 1000.0, 6.0, 100.0, Slope::Db12});
    const auto q40 = designAnalogPrototype({Shape::Bell, 1000.0, 6.0, 40.0, Slope::Db12});
    CHECK(std::abs(q.evaluate(1.01) - q40.evaluate(1.01)) < 1e-12);
}
