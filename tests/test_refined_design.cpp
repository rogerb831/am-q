#include <doctest/doctest.h>

#include "amq/dsp/analog_prototype.hpp"
#include "amq/dsp/conventions.hpp"
#include "amq/dsp/digital_design.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <initializer_list>

using namespace amq::dsp;

namespace {
// Independent fine scan: 20000 log points from 10 Hz to 0.9 Nyquist, floor -60 dB.
double fineScanMaxErrorDb(const AnalogPrototype& p, const DigitalFilter& d) {
    const double lim = 0.45 * d.sampleRate;
    double worst = 0.0;
    for (int k = 0; k <= 20000; ++k) {
        const double hz = 10.0 * std::pow(lim / 10.0, k / 20000.0);
        const double a = linearToDbFloored(std::abs(p.evaluateHz(hz)), -60.0);
        const double g = linearToDbFloored(std::abs(d.responseHz(hz)), -60.0);
        worst = std::max(worst, std::abs(a - g));
    }
    return worst;
}
}  // namespace

TEST_CASE("Joint refinement fixes cases the per-section designs miss (R12)") {
    struct Case { Shape shape; Slope slope; double f0, g, q, fs; };
    const Case cases[] = {
        {Shape::LowShelf, Slope::Db72, 18000.0, -12.0, 4.0, 44100.0},  // per-section LS: 11 dB
        {Shape::HighShelf, Slope::Db96, 10240.0, 30.0, 10.0, 96000.0},
        {Shape::Bell, Slope::Db24, 10000.0, -12.0, 0.707, 44100.0},
        {Shape::LowCut, Slope::Db48, 3000.0, 0.0, 3.0, 44100.0},
        {Shape::HighCut, Slope::Db24, 1000.0, 0.0, 1.0, 48000.0},
    };
    for (const auto& c : cases) {
        const auto p = designAnalogPrototype({c.shape, c.f0, c.g, c.q, c.slope});
        const auto before = designMatched(p, c.fs, DesignMethod::MatchedLeastSquares);
        const auto after = designDigital(p, c.fs);
        const double eb = fineScanMaxErrorDb(p, before), ea = fineScanMaxErrorDb(p, after);
        INFO(shapeName(c.shape) << " " << slopeName(c.slope) << " f0 " << c.f0 << ": before " << eb << " after " << ea);
        CHECK(ea <= 0.1);
        CHECK(ea <= eb);
    }
}

TEST_CASE("Refined notch keeps its zero exactly at f0") {
    for (double fs : {44100.0, 96000.0}) {
        for (double f0 : {100.0, 5000.0, 15000.0}) {
            for (Slope sl : {Slope::Db12, Slope::Db48}) {
                const auto p = designAnalogPrototype({Shape::Notch, f0, 0.0, 2.0, sl});
                const auto d = designDigital(p, fs);
                CHECK(std::abs(d.responseHz(f0)) < 1e-6);
            }
        }
    }
}

TEST_CASE("Refined design is stable and minimum phase") {
    for (double fs : {44100.0, 192000.0}) {
        for (int s = 0; s < kShapeCount; ++s) {
            if (static_cast<Shape>(s) == Shape::AllPass) continue;
            for (double f0 : {30.0, 3000.0, 18000.0}) {
                const auto p = designAnalogPrototype({static_cast<Shape>(s), f0, 12.0, 4.0, Slope::Db24});
                const auto d = designDigital(p, fs);
                for (const auto& b : d.sections) {
                    CHECK(std::abs(b.a2) < 1.0);
                    CHECK(std::abs(b.a1) < 1.0 + b.a2);
                    // Numerator zeros on or inside the unit circle.
                    if (b.b2 != 0.0) {
                        const std::complex<double> disc = std::sqrt(std::complex<double>(b.b1 * b.b1 - 4 * b.b0 * b.b2));
                        CHECK(std::abs((-b.b1 + disc) / (2 * b.b0)) <= 1.0 + 1e-9);
                        CHECK(std::abs((-b.b1 - disc) / (2 * b.b0)) <= 1.0 + 1e-9);
                    } else if (b.b0 != 0.0) {
                        CHECK(std::abs(b.b1) <= std::abs(b.b0) + 1e-12);
                    }
                }
            }
        }
    }
}

TEST_CASE("Default design adds the correction section only when order N misses R12") {
    // A 12 dB/oct Band Pass at 30 Hz, fs 96 kHz: one biquad cannot follow the analog
    // 6 dB/oct skirt towards Nyquist at the -60 dB floor, so the default adds a section.
    const auto p = designAnalogPrototype({Shape::BandPass, 30.0, 0.0, 0.707, Slope::Db12});
    RefineOptions orderN;
    orderN.extraSections = 0;
    const auto n = designRefined(p, 96000.0, orderN);
    const auto d = designDigital(p, 96000.0);  // default: conditional correction section
    const double e = fineScanMaxErrorDb(p, n), e2 = fineScanMaxErrorDb(p, d);
    MESSAGE("Band Pass 12 dB/oct, 30 Hz, 96 kHz: order N " << e << " dB, default " << e2 << " dB");
    CHECK(e > 0.1);  // documents the order-N limit (see phase0-design-notes.md)
    CHECK(e2 <= 0.1);
    CHECK(d.sections.size() == n.sections.size() + 1);

    // A band that meets R12 at order N keeps order N.
    const auto bell = designAnalogPrototype({Shape::Bell, 1000.0, 6.0, 1.0, Slope::Db24});
    CHECK(designDigital(bell, 48000.0).sections.size() == bell.sections.size());
}

TEST_CASE("Revised R12 tolerance regions") {
    CHECK(r12ToleranceDb(1000.0, 40.0, 48000.0) == doctest::Approx(0.1));
    CHECK(r12ToleranceDb(21600.0, 40.0, 48000.0) == doctest::Approx(0.1));   // exactly 0.9 Nyquist
    CHECK(r12ToleranceDb(22000.0, 3.0, 48000.0) == doctest::Approx(1.1));
    CHECK(std::isnan(r12ToleranceDb(22000.0, 3.01, 48000.0)));              // TBD until R14
    CHECK(r12DesignTargetDb(22000.0, 10.0, 48000.0) == doctest::Approx(1.1));
}
