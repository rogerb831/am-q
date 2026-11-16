// Natural Phase accuracy of the kernel builder with the real band designs (area 05 R7, area 03
// acceptance "Natural Phase accuracy"). Kernels come from the engine's own correction builder,
// engine::buildCorrectionKernels (area 03 R14, area 04 R9/R10), with its regularisation and
// Nyquist phase taper (area 03 R15); the reports compare the settings and the windows.
#include "amq/dsp/analog_prototype.hpp"
#include "amq/dsp/natural_phase_kernel.hpp"
#include "amq/engine/band_response.hpp"
#include "amq/engine/natural_phase.hpp"
#include "amq/params/mappings.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <memory>
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

// Harder cases for the regularisation report: zeros on and off bins, deep cuts, bands near
// Nyquist, and an all-pass.
const Band kStress[] = {
    {"Notch 15 kHz Q4", dsp::Shape::Notch, 15000, 0, 4},
    {"Notch 1 kHz Q10", dsp::Shape::Notch, 1000, 0, 10},
    {"Notch 18 kHz Q2", dsp::Shape::Notch, 18000, 0, 2},
    {"Notch 6 kHz Q0.7", dsp::Shape::Notch, 6000, 0, 0.7},
    {"Notch 12345 Hz Q3", dsp::Shape::Notch, 12345, 0, 3},
    {"Bell 3 kHz -30 Q8", dsp::Shape::Bell, 3000, -30, 8},
    {"Bell 20 kHz +12 Q2", dsp::Shape::Bell, 20000, 12, 2},
    {"High Shelf 19 kHz +9", dsp::Shape::HighShelf, 19000, 9, 0.7},
    {"High Cut 18 kHz 48", dsp::Shape::HighCut, 18000, 0, 0.7, dsp::Slope::Db48},
    {"All Pass 10 kHz Q2", dsp::Shape::AllPass, 10000, 0, 2},
    {"Bell 20.5 kHz +18 Q16", dsp::Shape::Bell, 20500, 18, 16},
    {"Bell 40 Hz +12 Q10", dsp::Shape::Bell, 40, 12, 10},
};

// Builds the kernel for one band with the engine's correction builder and measures |error| of
// digital * kernel against the analog target from 20 Hz to 0.95 Nyquist, where the target is
// above -40 dB.
bool measure(const Band& b, double fs, K::Window w, Errors& e, const engine::CorrectionOptions& opt = {},
             double fromHz = 20.0, double toNyquist = 0.95, double kaiserBeta = K::kDefaultKaiserBeta) {
    const engine::DesignInputs in{fs, std::log2(b.hz), b.gainDb, params::positionFromQ(b.q),
                                  static_cast<int>(b.shape), static_cast<int>(b.slope)};
    dsp::SvfCascade cascade;
    if (!engine::designBandCascade(in, engine::DesignTier::Refined, cascade)) return false;
    const auto proto = dsp::designAnalogPrototype(engine::bandSpecFrom(in));
    auto req = std::make_unique<engine::CorrectionRequest>();
    req->sampleRate = fs;
    req->channels = 1;
    req->bands[0] = engine::CorrectionBand{true, 0, 1, in, cascade};
    auto kernels = std::make_unique<engine::CorrectionKernels>();
    K builder(w, kaiserBeta);
    engine::buildCorrectionKernels(*req, builder, *kernels, opt);
    const auto& h = kernels->taps[0];
    const int points = 600;
    const double lo = std::log(fromHz), hi = std::log(toNyquist * fs / 2.0);
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
    // With the engine's regularisation (area 03 R15), over the accuracy bands and the stress
    // cases; also the top region (0.95 to 0.995 Nyquist), which has no target but must stay tame.
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        struct Choice {
            K::Window w;
            double beta;
            const char* name;
        };
        const Choice choices[] = {{K::Window::Rectangular, 0, "Rectangular"}, {K::Window::Hann, 0, "Hann"},
                                  {K::Window::BlackmanHarris, 0, "Blackman-Harris"}, {K::Window::Kaiser, 2, "Kaiser b=2"},
                                  {K::Window::Kaiser, 3, "Kaiser b=3"}, {K::Window::Kaiser, 4, "Kaiser b=4"},
                                  {K::Window::Kaiser, 6, "Kaiser b=6"}, {K::Window::Kaiser, 8, "Kaiser b=8"}};
        for (const auto& ch : choices) {
            const K::Window w = ch.w;
            Errors worst, top;
            std::string worstDb, worstDeg, topDb;
            auto each = [&](const Band& b) {
                Errors e, t;
                if (!measure(b, fs, w, e, {}, 20.0, 0.95, ch.beta)) return;
                measure(b, fs, w, t, {}, 0.95 * fs / 2.0, 0.995, ch.beta);
                if (e.db > worst.db) worst.db = e.db, worstDb = b.name;
                if (e.deg > worst.deg) worst.deg = e.deg, worstDeg = b.name;
                if (t.db > top.db) top.db = t.db, topDb = b.name;
            };
            for (const auto& b : kBands) each(b);
            for (const auto& b : kStress) each(b);
            std::printf("%6.0f Hz %-16s worst %.4f dB (%s), %.3f deg (%s); top %.3f dB (%s)\n", fs, ch.name,
                        worst.db, worstDb.c_str(), worst.deg, worstDeg.c_str(), top.db, topDb.c_str());
        }
        std::printf("\n");
    }
}

// Report: the regularisation settings (area 03 R15) over every band and the stress cases, with
// the default window. Run with
//   amq_engine_tests -tc="Natural Phase regularisation evaluation*" --no-skip
TEST_CASE("Natural Phase regularisation evaluation (report)" * doctest::skip()) {
    struct Variant {
        const char* name;
        engine::CorrectionOptions opt;
    };
    const Variant variants[] = {{"plain ratio", {false, 1.0}},          {"limit only", {true, 1.0}},
                                {"limit + taper 0.90", {true, 0.90}},   {"limit + taper 0.95", {true, 0.95}},
                                {"limit + taper 0.97", {true, 0.97}},   {"limit + taper 0.98", {true, 0.98}},
                                {"limit + taper 0.99", {true, 0.99}}};
    // Two ranges: the accuracy range (20 Hz to 0.95 Nyquist) and the top (0.95 to 0.995 Nyquist),
    // which has no target but must stay well-behaved.
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        for (const auto& v : variants) {
            Errors worst, top;
            std::string atDb, atDeg, topDb, topDeg;
            auto each = [&](const Band& b) {
                Errors e, t;
                if (!measure(b, fs, K::kDefaultWindow, e, v.opt)) return;
                measure(b, fs, K::kDefaultWindow, t, v.opt, 0.95 * fs / 2.0, 0.995);
                if (e.db > worst.db) worst.db = e.db, atDb = b.name;
                if (e.deg > worst.deg) worst.deg = e.deg, atDeg = b.name;
                if (t.db > top.db) top.db = t.db, topDb = b.name;
                if (t.deg > top.deg) top.deg = t.deg, topDeg = b.name;
            };
            for (const auto& b : kStress) each(b);
            for (const auto& b : kBands) each(b);
            std::printf("%6.0f Hz %-20s  to 0.95 Nyq: %7.4f dB (%-20s) %7.3f deg (%-20s) | 0.95-0.995: %7.3f dB (%-20s) %8.2f deg (%s)\n",
                        fs, v.name, worst.db, atDb.c_str(), worst.deg, atDeg.c_str(), top.db, topDb.c_str(), top.deg,
                        topDeg.c_str());
        }
        std::printf("\n");
    }
    // Per band, with the engine's default options.
    for (double fs : {44100.0, 48000.0, 96000.0})
        for (const auto& b : kStress) {
            Errors e;
            if (!measure(b, fs, K::kDefaultWindow, e)) continue;
            std::printf("  default %6.0f Hz %-22s %8.4f dB @ %6.0f Hz  %7.3f deg @ %6.0f Hz\n", fs, b.name, e.db, e.dbHz,
                        e.deg, e.degHz);
        }
}

// Report: where the notch correction goes wrong. Prints the analog and digital zero
// frequencies and the plain ratio per bin around the notch. Run with
//   amq_engine_tests -tc="Natural Phase notch diagnosis*" --no-skip
TEST_CASE("Natural Phase notch diagnosis (report)" * doctest::skip()) {
    for (double fs : {48000.0, 96000.0}) {
        const Band& b = kBands[12];
        const engine::DesignInputs in{fs, std::log2(b.hz), b.gainDb, params::positionFromQ(b.q),
                                      static_cast<int>(b.shape), static_cast<int>(b.slope)};
        dsp::SvfCascade cascade;
        REQUIRE(engine::designBandCascade(in, engine::DesignTier::Refined, cascade));
        const auto proto = dsp::designAnalogPrototype(engine::bandSpecFrom(in));
        double dMin = 1e9, dHz = 0, aMin = 1e9, aHz = 0;
        for (double hz = b.hz * 0.97; hz <= b.hz * 1.03; hz += 0.01) {
            const double d = std::abs(dsp::svfCascadeResponseHz(cascade, hz, fs));
            const double a = std::abs(proto.evaluateHz(hz));
            if (d < dMin) dMin = d, dHz = hz;
            if (a < aMin) aMin = a, aHz = hz;
        }
        std::printf("fs %.0f: analog zero %.2f Hz (|H| %.2e), digital zero %.2f Hz (|H| %.2e), %zu sections\n", fs, aHz,
                    aMin, dHz, dMin, cascade.sections.size());
        for (int k = 0; k < K::kBins; ++k) {
            const double hz = K::binHz(k, fs);
            if (hz < b.hz - 6 * fs / 512 || hz > b.hz + 6 * fs / 512) continue;
            const auto a = proto.evaluateHz(hz), d = dsp::svfCascadeResponseHz(cascade, hz, fs);
            const auto r = a / d;
            std::printf("  bin %3d %8.1f Hz  target %8.2f dB  digital %8.2f dB  ratio %8.3f dB %8.2f deg\n", k, hz,
                        20 * std::log10(std::abs(a)), 20 * std::log10(std::abs(d)), 20 * std::log10(std::abs(r)),
                        std::arg(r) * 180 / kPi);
        }
    }
}

TEST_CASE("Natural Phase accuracy with the default window: <= 0.05 dB and <= 2 degrees to 0.95 Nyquist") {
    // Every band, notches included: the removable-singularity limit (area 03 R15) handles a zero
    // that sits on a bin. Above 0.95 Nyquist there is no target; the magnitude stays within 1 dB
    // there (without the Nyquist taper it is off by up to 1.2 dB).
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        auto each = [&](const Band& b) {
            Errors e, top;
            REQUIRE(measure(b, fs, K::kDefaultWindow, e));
            measure(b, fs, K::kDefaultWindow, top, {}, 0.95 * fs / 2.0, 0.995);
            INFO(b.name << " at " << fs << " Hz: " << e.db << " dB, " << e.deg << " deg; top " << top.db << " dB");
            CHECK(e.db <= 0.05);
            CHECK(e.deg <= 2.0);
            CHECK(top.db <= 1.0);
        };
        for (const auto& b : kBands) each(b);
        for (const auto& b : kStress) each(b);
    }
}

TEST_CASE("Natural Phase notch on a bin: the plain ratio fails, the limit fixes it (area 03 R15)") {
    // A 6 kHz notch at 48 kHz has its zero exactly on bin 64: the plain ratio is 0/0 there.
    const Band notch{"Notch 6 kHz Q0.7", dsp::Shape::Notch, 6000, 0, 0.7};
    Errors plain, fixed;
    REQUIRE(measure(notch, 48000.0, K::kDefaultWindow, plain, {false, 1.0}));
    REQUIRE(measure(notch, 48000.0, K::kDefaultWindow, fixed));
    INFO("plain " << plain.db << " dB, regularised " << fixed.db << " dB");
    CHECK(plain.db > 1.0);
    CHECK(fixed.db <= 0.05);
}
