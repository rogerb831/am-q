#include <doctest/doctest.h>

#include "amq/dsp/analog_prototype.hpp"
#include "amq/dsp/conventions.hpp"
#include "amq/dsp/digital_design.hpp"

#include <cmath>
#include <initializer_list>
#include <algorithm>
#include <complex>

using namespace amq::dsp;

namespace {
double dB(std::complex<double> h) { return linearToDb(std::abs(h)); }
}  // namespace

TEST_CASE("Bilinear with pre-warp is exact at the pre-warp frequency") {
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        for (double f0 : {50.0, 1000.0, 15000.0}) {
            const auto p = designAnalogPrototype({Shape::Bell, f0, 9.0, 2.0, Slope::Db12});
            const auto d = designBilinear(p, fs);
            const auto ha = p.evaluateHz(f0);
            const auto hd = d.responseHz(f0);
            CHECK(std::abs(hd - ha) < 1e-9);
        }
    }
}

TEST_CASE("All Pass phase at the centre is exact (R12, within 2 degrees)") {
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        for (double f0 : {20.0, 200.0, 2000.0, 12000.0, 19000.0}) {
            for (Slope sl : {Slope::Db6, Slope::Db12, Slope::Db24, Slope::Db96}) {
                for (double q : {0.1, 1.0, 10.0}) {
                    const auto p = designAnalogPrototype({Shape::AllPass, f0, 0.0, q, sl});
                    const auto d = designDigital(p, fs);
                    const double err = std::remainder(std::arg(d.responseHz(f0)) - std::arg(p.evaluateHz(f0)), kTwoPi);
                    CHECK(std::abs(err) * 180.0 / kPi < 2.0);
                    CHECK(std::abs(std::abs(d.responseHz(0.3 * fs)) - 1.0) < 1e-9);
                }
            }
        }
    }
}

TEST_CASE("Three-point matched design is exact at DC and Nyquist for every section") {
    for (double fs : {44100.0, 192000.0}) {
        for (Shape sh : {Shape::Bell, Shape::LowShelf, Shape::HighCut, Shape::LowCut, Shape::BandPass}) {
            const auto p = designAnalogPrototype({sh, 3000.0, 12.0, 2.0, Slope::Db24});
            const auto d = designMatched(p, fs, DesignMethod::MatchedThreePoint);
            const double nyq = 0.5 * fs;
            CHECK(dB(d.responseHz(nyq)) == doctest::Approx(dB(p.evaluateHz(nyq))).epsilon(1e-6));
            if (sh != Shape::LowCut && sh != Shape::BandPass)
                CHECK(dB(d.responseHz(1e-3)) == doctest::Approx(dB(p.evaluateHz(1e-3))).epsilon(1e-6));
        }
    }
}

// Both matched variants meet R12 for a 2nd-order Bell with f0 below ~0.05 x Nyquist.
// At f0 = 2 kHz, fs = 44.1/48 kHz (about 0.09 Nyquist) wide bells (Q 0.5) already miss.
// Above that they do not (see the accuracy report); this test pins the region that passes.
TEST_CASE("Matched 2nd-order Bell meets 0.1 dB for f0 < 0.05 Nyquist") {
    for (auto method : {DesignMethod::MatchedLeastSquares, DesignMethod::MatchedThreePoint}) {
        for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
            for (double f0 : {20.0, 100.0, 1000.0}) {
                for (double g : {-12.0, 12.0}) {
                    for (double q : {0.5, 1.0, 4.0}) {
                        const auto p = designAnalogPrototype({Shape::Bell, f0, g, q, Slope::Db12});
                        const auto d = designMatched(p, fs, method);
                        double maxErr = 0.0;
                        for (double f = 10.0; f < 0.9 * 0.5 * fs; f *= 1.01)
                            maxErr = std::max(maxErr, std::abs(dB(d.responseHz(f)) - dB(p.evaluateHz(f))));
                        INFO("method " << static_cast<int>(method) << " fs " << fs << " f0 " << f0 << " g " << g << " q " << q);
                        CHECK(maxErr < 0.1);
                    }
                }
            }
        }
    }
}

TEST_CASE("Least-squares matched design keeps DC exact") {
    for (double fs : {44100.0, 192000.0}) {
        for (Shape sh : {Shape::Bell, Shape::LowShelf, Shape::HighShelf, Shape::HighCut, Shape::Notch}) {
            for (double f0 : {100.0, 3000.0, 15000.0}) {
                const auto p = designAnalogPrototype({sh, f0, 12.0, 2.0, Slope::Db24});
                const auto d = designMatched(p, fs, DesignMethod::MatchedLeastSquares);
                CHECK(dB(d.responseHz(1e-3)) == doctest::Approx(dB(p.evaluateHz(1e-3))).epsilon(1e-6));
            }
        }
    }
}

TEST_CASE("Matched design avoids cramping where bilinear cramps (high-frequency Bell)") {
    const double fs = 44100.0;
    const auto p = designAnalogPrototype({Shape::Bell, 16000.0, 12.0, 1.0, Slope::Db12});
    const auto dm = designMatched(p, fs);
    const auto db = designBilinear(p, fs);
    double errM = 0.0, errB = 0.0;
    for (double f = 10.0; f < 0.9 * 0.5 * fs; f *= 1.01) {
        errM = std::max(errM, std::abs(dB(dm.responseHz(f)) - dB(p.evaluateHz(f))));
        errB = std::max(errB, std::abs(dB(db.responseHz(f)) - dB(p.evaluateHz(f))));
    }
    MESSAGE("16 kHz bell at 44.1 kHz: matched max err " << errM << " dB, bilinear " << errB << " dB");
    CHECK(errM < errB);
    CHECK(errB > 1.0);  // bilinear cramping is real and large here
}

TEST_CASE("Designs are stable (all poles inside the unit circle)") {
    for (double fs : {44100.0, 192000.0}) {
        for (int s = 0; s < kShapeCount; ++s) {
            for (double f0 : {10.0, 1000.0, 20000.0, 30000.0}) {
                for (double q : {0.025, 1.0, 40.0}) {
                    const auto p = designAnalogPrototype({static_cast<Shape>(s), f0, 12.0, q, Slope::Db48});
                    for (auto method : {DesignMethod::MagnitudeMatched, DesignMethod::BilinearPrewarp}) {
                        const auto d = designDigital(p, fs, method);
                        for (const auto& b : d.sections) {
                            // Jury conditions for 1 + a1 z^-1 + a2 z^-2.
                            CHECK(std::abs(b.a2) < 1.0);
                            CHECK(std::abs(b.a1) < 1.0 + b.a2);
                        }
                    }
                }
            }
        }
    }
}
