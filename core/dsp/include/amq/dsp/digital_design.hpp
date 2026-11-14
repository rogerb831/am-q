#pragma once
// Analog-to-digital design (plan area 01, "Filter design"; R12).
//
//  - designMatched: magnitude-matched design. Each analog section keeps its poles,
//    mapped exactly with z = exp(s*T) (impulse-invariant pole mapping), and its
//    numerator is chosen so the digital magnitude equals the analog magnitude at DC,
//    at Nyquist and at the section's natural frequency. This follows
//    M. Vicanek, "Matched Second Order Digital Filters", 14 Feb 2016
//    (https://www.vicanek.de/articles/BiquadFits.pdf), generalised from his per-type
//    formulas (low pass, high pass, band pass, peaking) to an arbitrary second-order
//    section through the same |H|^2 = sum(B_i * phi_i) representation. The goal of
//    tracking the analog magnitude up to Nyquist without cramping is also the theme of
//    S. J. Orfanidis, "Digital Parametric Equalizer Design With Prescribed
//    Nyquist-Frequency Gain", J. Audio Eng. Soc. 45(6), 444-455, June 1997.
//
//  - designBilinear: bilinear transform with frequency pre-warp at a chosen frequency
//    (any DSP text, e.g. Oppenheim & Schafer, Discrete-Time Signal Processing, 7.1.2).
//    The fallback design, and the All Pass design (pre-warped at the centre frequency,
//    so the phase at the centre is exact).
//
// Output is cascaded biquads in direct form (a0 = 1). The real-time kernel (TPT/ZDF
// SVF, R18) is a Phase 1 task and PROVISIONAL pending counsel review; these biquads are
// the design-and-measurement representation only.

#include "amq/dsp/analog_prototype.hpp"

#include <complex>
#include <vector>

namespace amq::dsp {

struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0;

    // H(e^{j*omega}), omega in radians per sample.
    [[nodiscard]] std::complex<double> response(double omega) const noexcept;
};

struct DigitalFilter {
    std::vector<Biquad> sections;
    double sampleRate = 48000.0;
    int clampedSections = 0;  // matched design: sections whose 3-point target was unrealisable

    [[nodiscard]] std::complex<double> response(double omega) const noexcept;
    [[nodiscard]] std::complex<double> responseHz(double hz) const noexcept;
};

enum class DesignMethod {
    // Vicanek (2016) three-point match per section: exact at DC, Nyquist and the
    // section's natural frequency. The published baseline.
    MatchedThreePoint,
    // Am-Q extension (default): same exact pole mapping and the same |H|^2 phi basis,
    // but the three numerator terms (B0, B1, B2) are fitted to the analog magnitude
    // over a dense log grid up to Nyquist by weighted least squares with Lawson
    // (IRLS) re-weighting towards minimax relative error. Analog zeros on the j*Omega
    // axis (notches) map to unit-circle zeros at the exact frequency. Poles whose
    // frequency would alias above Nyquist are mapped with the bilinear map instead.
    MatchedLeastSquares,
    BilinearPrewarp,  // fallback, pre-warped at the band centre
    // Default. MatchedLeastSquares as the starting point, then a joint refinement of
    // all sections together: Levenberg-Marquardt on the dB error against the analog
    // prototype over a fit grid below limitFraction * Nyquist, with Lawson
    // re-weighting towards the minimax (Chebyshev) error. This is the classic
    // "minimum p-error" magnitude design of cascaded biquads (A. G. Deczky, IEEE
    // Trans. Audio Electroacoust. AU-20(4), 1972) applied to an analog target.
    // Section count and per-section order are unchanged; numerators are made minimum
    // phase afterwards. Runs only when the starting design misses the target.
    MatchedRefined,
    MagnitudeMatched = MatchedRefined,  // the area 01 default
};

// Options for the joint refinement (also used for its stop criterion).
struct RefineOptions {
    double limitFraction = 0.9;   // R12 region: below 0.9 * Nyquist
    double floorDb = -60.0;       // levels below this only need to stay below it
    double targetDb = 0.1;        // R12
    double stopDb = 0.05;         // stop refining once the fit-grid error is below this
    double startDb = 0.07;        // refine only if the starting design exceeds this
    int maxOuter = 10;            // Lawson re-weighting passes
    int maxInner = 40;            // Levenberg-Marquardt steps per pass
    // Maximum number of correction biquads appended to the cascade (start as identity).
    // With extraOnlyWhenNeeded (the default), they are added only when the order-N
    // design misses the band's revised R12 tolerance (r12ToleranceDb) on the dense
    // check grid, with a margin of extraTriggerFraction; and kept only if they help.
    // Section count then changes with the settings, so Phase 1's off-thread redesign
    // must treat a section-count change like a slope change (R17 crossfade).
    int extraSections = 1;
    bool extraOnlyWhenNeeded = true;
    double extraTriggerFraction = 0.9;
};

// Revised area 01 R12 tolerance for one band (dB, compared below 0.9 * Nyquist with
// levels below -60 dB floored): 0.1 dB for f0 <= 0.9 * Nyquist; 1.1 dB for
// f0 > 0.9 * Nyquist with Q <= 3; NaN (TBD until R14) for f0 > 0.9 * Nyquist with Q > 3.
[[nodiscard]] double r12ToleranceDb(double centreHz, double q, double sampleRate) noexcept;

// Tolerance used to decide whether to add the correction section. Equals r12ToleranceDb,
// except that the TBD region uses 1.1 dB (assumption until R14 is settled).
[[nodiscard]] double r12DesignTargetDb(double centreHz, double q, double sampleRate) noexcept;

// The default design with explicit refinement options: matched least-squares start,
// then joint refinement from several starts (see MatchedRefined).
[[nodiscard]] DigitalFilter designRefined(const AnalogPrototype& proto, double sampleRate,
                                          const RefineOptions& options);

[[nodiscard]] DigitalFilter designMatched(const AnalogPrototype& proto, double sampleRate,
                                          DesignMethod method = DesignMethod::MatchedRefined);

// Joint minimax refinement of an existing digital design against its prototype.
// Returns the better (by max dB error on the fit grid) of the input and the result.
[[nodiscard]] DigitalFilter refineJoint(const AnalogPrototype& proto, const DigitalFilter& start,
                                        const RefineOptions& options = {});

// Max dB error of a design against its prototype on the refinement's fit grid
// (below limitFraction * Nyquist, levels floored at floorDb).
// Max dB error on an independent, denser check grid (offset from the fit grid). Used to
// choose between refinement candidates so a spike between fit points cannot win.
[[nodiscard]] double denseCheckMaxErrorDb(const AnalogPrototype& proto, const DigitalFilter& d,
                                          const RefineOptions& options = {});

[[nodiscard]] double fitGridMaxErrorDb(const AnalogPrototype& proto, const DigitalFilter& d,
                                       const RefineOptions& options = {});

// Pre-warp frequency defaults to the band centre. If it is at or above Nyquist it is
// clamped to 0.95 * Nyquist (the centre cannot be mapped exactly there).
[[nodiscard]] DigitalFilter designBilinear(const AnalogPrototype& proto, double sampleRate,
                                           double prewarpHz = -1.0);

// Area 01 default routing: All Pass always uses the pre-warped bilinear design; every
// other shape uses the requested method.
[[nodiscard]] DigitalFilter designDigital(const AnalogPrototype& proto, double sampleRate,
                                          DesignMethod method = DesignMethod::MagnitudeMatched);

}  // namespace amq::dsp
