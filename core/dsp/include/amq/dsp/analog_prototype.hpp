#pragma once
// Analog prototypes for the ten Am-Q shapes (plan area 01, R9, R10, R12, R14; area 04 R9).
//
// Every band is a cascade of second-order s-domain sections (a first-order section is
// stored as a second-order one with zero s^2 terms). The variable s is normalised to the
// band's centre frequency: s = j*Omega with Omega = f / f0. evaluate(Omega) returns the
// complex H(j*Omega).
//
// Public basis (see docs/dsp/phase0-design-notes.md for the full list):
//  - Butterworth pole placement: any filter design text (e.g. Oppenheim & Schafer,
//    Discrete-Time Signal Processing, ch. 7; Parks & Burrus, Digital Filter Design).
//  - Second-order bell / shelf: R. Bristow-Johnson, "Cookbook formulae for audio EQ
//    biquad filter coefficients" (analog prototypes).
//  - High-order Butterworth shelf and the low-pass to band-pass mapping used for
//    higher-order Bell / Notch / Band Pass: S. J. Orfanidis, "High-Order Digital
//    Parametric Equalizer Design", J. Audio Eng. Soc. 53(11), 1026-1046, Nov 2005.
//
// PROVISIONAL: the displayed-Q to section-Q mapping per shape (R14) is an Am-Q
// decision still open (Phase 0 task "Define and document the displayed-Q to section-Q
// mapping"). The mapping used here is a documented placeholder; see qMappingNotes().

#include "amq/dsp/fixed_vector.hpp"

#include <complex>
#include <string_view>
#include <vector>

namespace amq::dsp {

// Stable shape ids (area 15 R7: ids never renumber). Order matches R9.
enum class Shape : int {
    Bell = 0,
    LowShelf = 1,
    LowCut = 2,
    HighShelf = 3,
    HighCut = 4,
    Notch = 5,
    BandPass = 6,
    TiltShelf = 7,
    FlatTilt = 8,
    AllPass = 9,
};
inline constexpr int kShapeCount = 10;

enum class Slope : int {
    Db6 = 0, Db12, Db18, Db24, Db30, Db36, Db48, Db72, Db96,
    Brickwall,  // Low Cut / High Cut only; Phase 2 design, not implemented in Phase 0
};
inline constexpr int kSlopeCount = 10;

[[nodiscard]] std::string_view shapeName(Shape) noexcept;
[[nodiscard]] std::string_view slopeName(Slope) noexcept;
[[nodiscard]] int slopeDbPerOctave(Slope) noexcept;  // 0 for Brickwall

[[nodiscard]] bool shapeHasGain(Shape) noexcept;
[[nodiscard]] bool shapeUsesQ(Shape) noexcept;
[[nodiscard]] bool shapeUsesSlope(Shape) noexcept;
[[nodiscard]] bool slopeAllowed(Shape, Slope) noexcept;

// R10: order = slope / 6; Bell, Band Pass and Notch have a minimum order of 2.
[[nodiscard]] int filterOrder(Shape, Slope) noexcept;

inline constexpr double kMinQ = 0.025;  // R8
inline constexpr double kMaxQ = 40.0;   // R8

struct BandSpec {
    Shape shape = Shape::Bell;
    double frequencyHz = 1000.0;
    double gainDb = 0.0;
    double q = 1.0;
    Slope slope = Slope::Db12;
};

// One s-domain section: (b0 + b1 s + b2 s^2) / (a0 + a1 s + a2 s^2), s normalised to f0.
struct AnalogSection {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a0 = 1.0, a1 = 0.0, a2 = 0.0;

    [[nodiscard]] bool isFirstOrder() const noexcept { return a2 == 0.0 && b2 == 0.0; }
    [[nodiscard]] std::complex<double> evaluateS(std::complex<double> s) const noexcept;
    [[nodiscard]] std::complex<double> evaluate(double omega) const noexcept {
        return evaluateS({0.0, omega});
    }
};

// At most 10 sections today (Flat Tilt: 20 real poles); 16 leaves headroom.
inline constexpr std::size_t kMaxAnalogSections = 16;

struct AnalogPrototype {
    Shape shape = Shape::Bell;
    double centreHz = 1000.0;  // the normalisation frequency f0
    int order = 0;              // nominal order from R10 (0 if unsupported)
    double q = 1.0;             // displayed Q after clamping (R8), for tolerance rules
    bool supported = false;     // false for Brickwall (Phase 2)
    // The overall gain is folded into sections[0]. Fixed capacity: no allocation (area 01 R19).
    FixedVector<AnalogSection, kMaxAnalogSections> sections;

    // H(j*Omega), Omega = f / f0.
    [[nodiscard]] std::complex<double> evaluate(double omega) const noexcept;
    [[nodiscard]] std::complex<double> evaluateHz(double hz) const noexcept {
        return evaluate(hz / centreHz);
    }
};

// Builds the prototype. Gain is clamped to +/-30 dB (R13), Q to [0.025, 40] (R8).
[[nodiscard]] AnalogPrototype designAnalogPrototype(const BandSpec& spec);

// Human-readable description of the provisional Q convention (for reports/docs).
[[nodiscard]] std::string_view qMappingNotes() noexcept;

}  // namespace amq::dsp
