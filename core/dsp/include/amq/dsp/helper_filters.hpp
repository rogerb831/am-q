#pragma once
// Design modes (area 02 R15) and the internal helper filters.
//
// Three design modes, chosen by what a filter is for:
//   MagnitudeMatched   user bands (every shape but All Pass): digital_design.hpp, MatchedRefined
//                      off the audio thread, three-point matched on it (realtime_design.hpp)
//   BilinearCentre     All Pass: bilinear pre-warped at the centre, so its phase there is exact
//   BilinearBandwidth  internal helper filters (side-chain filters, solo audition): bilinear with
//                      the centre and the bandwidth pre-warped, below
//
// Helper filters are one second-order section in SVF form (svf.hpp), unity gain in the passband:
//   - Band-pass: g = tan(pi f0 / fs), so the digital peak sits exactly at f0 at 0 dB. The -3 dB
//     edges f1, f2 satisfy tan(pi f1/fs) * tan(pi f2/fs) = g^2 and k = (tan(pi f2/fs) -
//     tan(pi f1/fs)) / g; we pick them so f2 / f1 = 2^N, N the octave bandwidth of an analog
//     second-order band-pass with the same Q: N = (2 / ln 2) asinh(1 / (2 Q)). So the width in
//     octaves is the analog one at every frequency, up to near Nyquist.
//   - Low-pass and high-pass: g = tan(pi fc / fs) (the -3 dB point of a Butterworth section is
//     exactly fc), k = 1 / Q.

#include "amq/dsp/analog_prototype.hpp"
#include "amq/dsp/svf.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace amq::dsp {

enum class DesignMode : uint8_t { MagnitudeMatched, BilinearCentre, BilinearBandwidth };
enum class FilterRole : uint8_t { UserBand, Helper };

[[nodiscard]] inline DesignMode designModeFor(Shape s, FilterRole role, Slope slope = Slope::Db12) noexcept {
    if (role == FilterRole::Helper) return DesignMode::BilinearBandwidth;
    // All Pass and Brickwall cuts: bilinear at the centre (phase exact / equiripple kept).
    return s == Shape::AllPass || slope == Slope::Brickwall ? DesignMode::BilinearCentre : DesignMode::MagnitudeMatched;
}

inline constexpr double kButterworthQ = 0.7071067811865476;

// Octave bandwidth of an analog second-order band-pass with quality factor q.
[[nodiscard]] inline double bandPassOctaves(double q) noexcept {
    return 2.0 / std::log(2.0) * std::asinh(1.0 / (2.0 * q));
}

namespace detail {
inline constexpr double kPi = 3.14159265358979323846;
[[nodiscard]] inline double clampHz(double hz, double fs) noexcept { return std::clamp(hz, 1.0, 0.49 * fs); }
}  // namespace detail

// Band-pass, 0 dB peak at hz, -3 dB width of an analog section with quality q (in octaves).
[[nodiscard]] inline SvfParams helperBandPass(double hz, double q, double fs) noexcept {
    using detail::kPi;
    hz = detail::clampHz(hz, fs);
    SvfParams p;
    p.g = std::tan(kPi * hz / fs);
    const double r = std::exp2(bandPassOctaves(q));
    const double g2 = p.g * p.g;
    const double top = std::min(hz, 0.4999 * fs / r);  // keep f2 = r f1 below Nyquist
    auto h = [&](double f1) { return std::tan(kPi * f1 / fs) * std::tan(kPi * r * f1 / fs) - g2; };
    if (h(top) >= 0.0) {
        double lo = 0.0, hi = top;
        for (int i = 0; i < 60; ++i) {
            const double mid = 0.5 * (lo + hi);
            (h(mid) < 0.0 ? lo : hi) = mid;
        }
        const double f1 = 0.5 * (lo + hi);
        p.k = (std::tan(kPi * r * f1 / fs) - std::tan(kPi * f1 / fs)) / p.g;
    } else {
        p.k = 1.0 / q;  // the upper edge would pass Nyquist: plain bilinear width
    }
    p.m0 = 0.0, p.m1 = p.k, p.m2 = 0.0;  // band output v1 peaks at 1/k: m1 = k gives 0 dB
    return p;
}

[[nodiscard]] inline SvfParams helperLowPass(double hz, double fs, double q = kButterworthQ) noexcept {
    SvfParams p;
    p.g = std::tan(detail::kPi * detail::clampHz(hz, fs) / fs);
    p.k = 1.0 / q;
    p.m0 = 0.0, p.m1 = 0.0, p.m2 = 1.0;
    return p;
}

[[nodiscard]] inline SvfParams helperHighPass(double hz, double fs, double q = kButterworthQ) noexcept {
    SvfParams p;
    p.g = std::tan(detail::kPi * detail::clampHz(hz, fs) / fs);
    p.k = 1.0 / q;
    p.m0 = 1.0, p.m1 = -p.k, p.m2 = -1.0;  // x - k v1 - v2
    return p;
}

}  // namespace amq::dsp
