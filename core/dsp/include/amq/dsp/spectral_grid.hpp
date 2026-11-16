#pragma once
// Spectral dynamics analysis grid (area 07 R1-R4, R6; area 08 R3; spec in
// docs/dsp/spectral-dynamics.md). All constants are PROVISIONAL until the area 07 listening tests.
//
// Density (0-100) sets the analysis band width: 1/4 octave at Density 0 to 1/16 octave at 100
// (log-interpolated), measured at 1 kHz; a band at f is (f / 1 kHz)^-0.1 times that, so bands are
// finer at high frequencies (R2). Bands are never narrower than two FFT bins. Centres run from the
// span's lower edge to its upper edge (R1); the count is bounded (R3). Weights are triangular in
// log frequency between neighbouring centres and sum to 1 at every bin (a partition of unity), so
// interpolating band gains back to bins is exact for a constant gain.
//
// Frequency-time scaling (R4): each analysis band's attack is the band attack at its centre
// (dynamics.hpp, attackSeconds), and its release is the Release time times (1 kHz / f)^0.25,
// limited to x0.5 .. x2.
//
// Spectral Tilt (R6): the threshold of each analysis band moves by kTiltDbPerOctave per octave
// from the band centre. The value is stored as a float-capable toggle (area 07 design).

#include "amq/dsp/dynamics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace amq::dsp {

inline constexpr int kSpectralMinBands = 3;
inline constexpr int kSpectralMaxBands = 96;
inline constexpr double kSpectralWidthMaxOct = 0.25;    // Density 0, at 1 kHz
inline constexpr double kSpectralWidthMinOct = 0.0625;  // Density 100, at 1 kHz
inline constexpr double kSpectralWidthFreqExponent = -0.1;
inline constexpr double kSpectralReleaseExponent = 0.25;
inline constexpr double kTiltDbPerOctave = -3.0;        // higher bands get a lower threshold (PROVISIONAL)
inline constexpr double kSpectralSpanMinHz = 20.0, kSpectralSpanMaxHz = 20000.0;

[[nodiscard]] inline double spectralWidthOctaves(double density, double hz) noexcept {
    const double d = std::clamp(density, 0.0, 100.0) / 100.0;
    const double w1k = kSpectralWidthMaxOct * std::pow(kSpectralWidthMinOct / kSpectralWidthMaxOct, d);
    return w1k * std::pow(std::max(hz, 1.0) / 1000.0, kSpectralWidthFreqExponent);
}

[[nodiscard]] inline double spectralReleaseSeconds(double releaseKnob, double hz) noexcept {
    const double s = std::clamp(std::pow(1000.0 / std::max(hz, 1.0), kSpectralReleaseExponent), 0.5, 2.0);
    return releaseSeconds(releaseKnob) * s;
}

// Log-spaced analysis bands over [lo, hi] with triangular bin weights.
struct SpectralGrid {
    int count = 0;
    std::array<double, kSpectralMaxBands> centreHz{};
    std::array<double, kSpectralMaxBands> widthOct{};

    // Builds the centres. binHz = fs / N (the FFT bin spacing). No allocation.
    void build(double lo, double hi, double density, double binHz) noexcept {
        lo = std::clamp(lo, 1.0, hi);
        count = 0;
        double c = lo * std::exp2(0.5 * spectralWidthOctaves(density, lo));
        auto next = [&](double f) {
            const double w = spectralWidthOctaves(density, f);
            return std::max(f * std::exp2(w), f + 2.0 * binHz);
        };
        // Count first, then widen uniformly if the count exceeds the bound.
        int n = 0;
        for (double f = c; f <= hi && n < 100000; f = next(f)) ++n;
        double stretch = 1.0;
        if (n > kSpectralMaxBands) stretch = static_cast<double>(n) / kSpectralMaxBands;
        for (double f = c; f <= hi && count < kSpectralMaxBands;) {
            centreHz[static_cast<size_t>(count)] = f;
            widthOct[static_cast<size_t>(count)] = spectralWidthOctaves(density, f) * stretch;
            ++count;
            f = std::max(f * std::exp2(spectralWidthOctaves(density, f) * stretch), f + 2.0 * binHz);
        }
        while (count < kSpectralMinBands) {  // a narrow span: spread the minimum count over it
            count = kSpectralMinBands;
            for (int i = 0; i < count; ++i)
                centreHz[static_cast<size_t>(i)] = lo * std::pow(hi / lo, (i + 0.5) / count);
            const double w = std::log2(hi / lo) / count;
            for (int i = 0; i < count; ++i) widthOct[static_cast<size_t>(i)] = w;
        }
    }

    // The two bands that weight a bin at hz, and the weight of the first (the second gets 1 - w).
    // Below the first centre and above the last, the end band takes it all.
    void weights(double hz, int& a, int& b, double& wa) const noexcept {
        if (count <= 0) {
            a = b = 0, wa = 1.0;
            return;
        }
        if (hz <= centreHz[0]) {
            a = b = 0, wa = 1.0;
            return;
        }
        const int last = count - 1;
        if (hz >= centreHz[static_cast<size_t>(last)]) {
            a = b = last, wa = 1.0;
            return;
        }
        int i = 0;
        while (i + 1 < count && centreHz[static_cast<size_t>(i + 1)] < hz) ++i;
        a = i, b = i + 1;
        const double la = std::log2(centreHz[static_cast<size_t>(a)]), lb = std::log2(centreHz[static_cast<size_t>(b)]);
        wa = (lb - std::log2(hz)) / (lb - la);
    }
};

}  // namespace amq::dsp
