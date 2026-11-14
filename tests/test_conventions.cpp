#include <doctest/doctest.h>

#include "amq/dsp/conventions.hpp"

#include <cmath>
#include <initializer_list>

using namespace amq::dsp;

TEST_CASE("dB <-> linear uses 10^(dB/20) (R13)") {
    CHECK(dbToLinear(0.0) == doctest::Approx(1.0));
    CHECK(dbToLinear(20.0) == doctest::Approx(10.0));
    CHECK(dbToLinear(-6.0) == doctest::Approx(0.501187233627).epsilon(1e-12));
    for (double db = -90.0; db <= 30.0; db += 0.37) CHECK(linearToDb(dbToLinear(db)) == doctest::Approx(db).epsilon(1e-12));
    CHECK(linearToDbFloored(0.0, -60.0) == doctest::Approx(-60.0));
    CHECK(clampBandGainDb(45.0) == 30.0);
    CHECK(clampBandGainDb(-45.0) == -30.0);
    CHECK(clampBandGainDb(12.5) == 12.5);
}

TEST_CASE("one-pole coefficient covers 90 % of a step in the stated time (R20)") {
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        for (long n : {48L, 480L, 4800L, 48000L}) {  // whole sample counts
            const double t = static_cast<double>(n) / fs;
            const double c = onePoleCoefficientFor90PercentTime(t, fs);
            double y = 0.0;
            for (long i = 0; i < n; ++i) y += c * (1.0 - y);
            CHECK(y == doctest::Approx(0.9).epsilon(1e-6));
            CHECK(ninetyPercentTimeOfOnePole(c, fs) == doctest::Approx(t).epsilon(1e-9));
        }
    }
    CHECK(onePoleCoefficientFor90PercentTime(0.0, 48000.0) == 1.0);
    CHECK(linearRampLengthFor90PercentTime(0.09) == doctest::Approx(0.1));
}

TEST_CASE("log2-Hz frequency encoding") {
    CHECK(hzToLog2(1000.0) == doctest::Approx(std::log2(1000.0)));
    CHECK(log2ToHz(hzToLog2(1234.5)) == doctest::Approx(1234.5));
    CHECK(normalisedToFrequency(0.0) == doctest::Approx(10.0));
    CHECK(normalisedToFrequency(1.0) == doctest::Approx(30000.0));
    CHECK(frequencyToNormalised(normalisedToFrequency(0.42)) == doctest::Approx(0.42));
    CHECK(hzToRadiansPerSample(24000.0, 48000.0) == doctest::Approx(kPi));
    CHECK(log2HzToRadiansPerSample(hzToLog2(12000.0), 48000.0) == doctest::Approx(kPi / 2));
}
