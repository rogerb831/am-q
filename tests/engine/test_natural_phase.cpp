// Natural Phase accuracy of the kernel builder with the real band designs (area 05 R7, area 03
// acceptance "Natural Phase accuracy"). The correction here is the plain ratio of the analog
// target to the settled digital response at the bin centres (area 03 R14, area 04 R9/R10); the
// regularisation and the Nyquist phase taper (area 03 R15) are Phase 2 and not applied.
#include "amq/dsp/analog_prototype.hpp"
#include "amq/dsp/natural_phase_kernel.hpp"
#include "amq/engine/band_response.hpp"
#include "amq/params/mappings.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

using namespace amq;
using K = dsp::NaturalPhaseKernelBuilder;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Band {
    const char* name;
    dsp::Shape shape;
    double hz, gainDb, q;
    dsp::Slope slope = dsp::Slope::Db12;
};

const Band kBands[] = {
    {"Bell 1 kHz +6 Q1", dsp::Shape::Bell, 1000, 6, 1},
    {"Bell 100 Hz -9 Q2", dsp::Shape::Bell, 100, -9, 2},
    {"Bell 6 kHz +12 Q4", dsp::Shape::Bell, 6000, 12, 4},
    {"Bell 12 kHz +9 Q1", dsp::Shape::Bell, 12000, 9, 1},
    {"Bell 18 kHz -6 Q2", dsp::Shape::Bell, 18000, -6, 2},
    {"Low Shelf 120 Hz +6", dsp::Shape::LowShelf, 120, 6, 0.7},
    {"High Shelf 8 kHz +6", dsp::Shape::HighShelf, 8000, 6, 0.7},
    {"High Shelf 15 kHz -9", dsp::Shape::HighShelf, 15000, -9, 0.7},
    {"Low Cut 60 Hz 12", dsp::Shape::LowCut, 60, 0, 0.7, dsp::Slope::Db12},
    {"High Cut 12 kHz 24", dsp::Shape::HighCut, 12000, 0, 0.7, dsp::Slope::Db24},
    {"Bell 19 kHz +12 Q8", dsp::Shape::Bell, 19000, 12, 8},
    {"Bell 3 kHz -18 Q20", dsp::Shape::Bell, 3000, -18, 20},
    {"Notch 15 kHz Q4", dsp::Shape::Notch, 15000, 0, 4},
    {"High Cut 8 kHz 96", dsp::Shape::HighCut, 8000, 0, 0.7, dsp::Slope::Db96},
    {"Low Cut 40 Hz 48", dsp::Shape::LowCut, 40, 0, 0.7, dsp::Slope::Db48},
    {"Tilt Shelf 1 kHz +9", dsp::Shape::TiltShelf, 1000, 9, 0.7},
};

struct Errors {
    double db = 0.0, deg = 0.0, dbHz = 0.0, degHz = 0.0;
    double rawDb = 0.0, rawDeg = 0.0;  // the IIR alone against the target, for scale
};

// Builds the kernel for one band and measures |error| of digital * kernel against the analog
// target from 20 Hz to 0.95 Nyquist, where the target is above -40 dB.
bool measure(const Band& b, double fs, K::Window w, Errors& e) {
    const engine::DesignInputs in{fs, std::log2(b.hz), b.gainDb, params::positionFromQ(b.q),
                                  static_cast<int>(b.shape), static_cast<int>(b.slope)};
    dsp::SvfCascade cascade;
    if (!engine::designBandCascade(in, engine::DesignTier::Refined, cascade)) return false;
    const auto proto = dsp::designAnalogPrototype(engine::bandSpecFrom(in));
    std::vector<std::complex<double>> c(K::kBins);
    for (int k = 0; k < K::kBins; ++k) {
        const double hz = std::max(K::binHz(k, fs), 1e-3);  // DC: the limit of the ratio
        c[static_cast<size_t>(k)] = proto.evaluateHz(hz) / dsp::svfCascadeResponseHz(cascade, hz, fs);
    }
    K builder(w);
    std::vector<float> h(K::kTaps);
    builder.build(c.data(), h.data());
    const int points = 600;
    const double lo = std::log(20.0), hi = std::log(0.95 * fs / 2.0);
    for (int i = 0; i < points; ++i) {
        const double hz = std::exp(lo + (hi - lo) * i / (points - 1));
        const auto target = proto.evaluateHz(hz);
        if (std::abs(target) < 0.01) continue;
        const double om = 2.0 * kPi * hz / fs;
        std::complex<double> kern = 0.0;
        for (int n = 0; n < K::kTaps; ++n) kern += static_cast<double>(h[static_cast<size_t>(n)]) * std::polar(1.0, -om * (n - K::kCentre));
        const auto got = dsp::svfCascadeResponseHz(cascade, hz, fs) * kern;
        const auto ratio = got / target;
        const double db = std::abs(20.0 * std::log10(std::abs(ratio)));
        const double deg = std::abs(std::arg(ratio)) * 180.0 / kPi;
        if (db > e.db) e.db = db, e.dbHz = hz;
        if (deg > e.deg) e.deg = deg, e.degHz = hz;
        const auto raw = dsp::svfCascadeResponseHz(cascade, hz, fs) / target;
        e.rawDb = std::max(e.rawDb, std::abs(20.0 * std::log10(std::abs(raw))));
        e.rawDeg = std::max(e.rawDeg, std::abs(std::arg(raw)) * 180.0 / kPi);
    }
    return true;
}

const char* windowName(K::Window w) {
    switch (w) {
        case K::Window::Rectangular: return "Rectangular";
        case K::Window::Hann: return "Hann";
        case K::Window::BlackmanHarris: return "Blackman-Harris";
        case K::Window::Kaiser: return "Kaiser b=6";
    }
    return "?";
}

}  // namespace

// Report: every window candidate against the target (0.05 dB, 2 degrees). Run with
//   amq_engine_tests -tc="Natural Phase window evaluation*" --no-skip
TEST_CASE("Natural Phase window evaluation (report)" * doctest::skip()) {
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        for (auto w : {K::Window::Rectangular, K::Window::Hann, K::Window::BlackmanHarris, K::Window::Kaiser}) {
            Errors worst;
            std::string worstDb, worstDeg;
            for (const auto& b : kBands) {
                Errors e;
                if (!measure(b, fs, w, e)) continue;
                std::printf("  %6.0f Hz  %-16s %-22s %7.4f dB @ %6.0f Hz   %6.3f deg @ %6.0f Hz   (IIR alone %6.3f dB, %6.2f deg)\n",
                            fs, windowName(w), b.name, e.db, e.dbHz, e.deg, e.degHz, e.rawDb, e.rawDeg);
                if (e.db > worst.db) worst.db = e.db, worstDb = b.name;
                if (e.deg > worst.deg) worst.deg = e.deg, worstDeg = b.name;
            }
            std::printf("%6.0f Hz %-16s worst %.4f dB (%s), %.3f deg (%s)\n\n", fs, windowName(w), worst.db,
                        worstDb.c_str(), worst.deg, worstDeg.c_str());
        }
    }
}

TEST_CASE("Natural Phase accuracy with the default window: <= 0.05 dB and <= 2 degrees to 0.95 Nyquist") {
    // The notch is left out: next to its zero the plain ratio has a spike 512 taps cannot
    // resolve (0.43 dB at 48 kHz). That is the near-zero case the area 03 R15 regularisation
    // covers (Phase 2).
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        for (const auto& b : kBands) {
            if (b.shape == dsp::Shape::Notch) continue;
            Errors e;
            REQUIRE(measure(b, fs, K::kDefaultWindow, e));
            INFO(b.name << " at " << fs << " Hz: " << e.db << " dB, " << e.deg << " deg");
            CHECK(e.db <= 0.05);
            CHECK(e.deg <= 2.0);
        }
    }
}
