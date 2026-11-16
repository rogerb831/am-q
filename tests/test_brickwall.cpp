// Brickwall cuts (area 01 task "Design and tune a brickwall prototype by measurement"). The
// measured properties of the order-16 inverse Chebyshev prototype and its bilinear design.
// PROVISIONAL targets (the tolerance is an open question in area 01): -3 dB at f0, passband flat
// within 0.01 dB below half (Low Cut: above twice) f0, at least 80 dB down from 0.26 octave past f0.
#include "amq/dsp/realtime_design.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>

using namespace amq::dsp;

namespace {
double db(double m) { return 20.0 * std::log10(std::max(m, 1e-300)); }
const double kStopEdge = std::cosh(std::acosh(std::sqrt(std::pow(10.0, 8.0) - 1.0)) / 16.0);  // 1.198
}  // namespace

TEST_CASE("Brickwall analog prototype: -3 dB at f0, flat passband, 80 dB equiripple stopband") {
    for (auto shape : {Shape::HighCut, Shape::LowCut}) {
        const auto p = designAnalogPrototype({shape, 1000.0, 0.0, 1.0, Slope::Brickwall});
        REQUIRE(p.supported);
        REQUIRE(p.sections.size() == 8);
        CHECK(db(std::abs(p.evaluate(1.0))) == doctest::Approx(-3.0103).epsilon(1e-6));
        const bool lp = shape == Shape::HighCut;
        double passDev = 0.0, stopMax = -1e9;
        for (int i = 0; i <= 2000; ++i) {
            const double w = std::pow(10.0, -3.0 + 6.0 * i / 2000.0);
            const double m = db(std::abs(p.evaluate(w)));
            const bool pass = lp ? w <= 0.5 : w >= 2.0;
            const bool stop = lp ? w >= kStopEdge : w <= 1.0 / kStopEdge;
            if (pass) passDev = std::max(passDev, std::abs(m));
            if (stop) stopMax = std::max(stopMax, m);
        }
        INFO((lp ? "High Cut" : "Low Cut") << ": passband deviation " << passDev << " dB, stopband max " << stopMax << " dB");
        CHECK(passDev < 0.01);
        CHECK(stopMax <= -79.99);
        CHECK(stopMax > -80.5);  // equiripple: the stopband reaches its depth
    }
    CHECK(kStopEdge == doctest::Approx(1.198).epsilon(1e-3));  // 0.26 octave
}

TEST_CASE("Brickwall digital design: bilinear at f0 keeps -3 dB at f0 and the stopband depth") {
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0})
        for (double f0 : {30.0, 200.0, 1000.0, 8000.0, 16000.0})
            for (auto shape : {Shape::HighCut, Shape::LowCut}) {
                SvfCascade c;
                REQUIRE(designBandRealtime({shape, f0, 0.0, 1.0, Slope::Brickwall}, fs, c) == RealtimeDesignResult::Matched);
                const bool lp = shape == Shape::HighCut;
                INFO("fs " << fs << " f0 " << f0 << (lp ? " High Cut" : " Low Cut"));
                CHECK(db(std::abs(svfCascadeResponseHz(c, f0, fs))) == doctest::Approx(-3.0103).epsilon(1e-4));
                double passDev = 0.0, stopMax = -1e9;
                for (int i = 0; i <= 1500; ++i) {
                    const double hz = 5.0 * std::pow(0.49 * fs / 5.0, i / 1500.0);
                    const double m = db(std::abs(svfCascadeResponseHz(c, hz, fs)));
                    if (lp ? hz <= 0.5 * f0 : hz >= 2.0 * f0) passDev = std::max(passDev, std::abs(m));
                    if (lp ? hz >= f0 * kStopEdge : hz <= f0 / kStopEdge) stopMax = std::max(stopMax, m);
                }
                CHECK(passDev < 0.01);
                CHECK(stopMax <= -79.9);
            }
}
