#pragma once
// Band design for the audio thread (area 01 R15, R19): allocation-free and about 1 us per band.
//
// Phase 1 tiering (docs/plan/areas/01, "Filter design"):
//   - Audio thread: the three-point matched design (Vicanek 2016), redesigned at most every
//     32 samples while parameters move. All Pass uses bilinear pre-warped at f0 (area 02 R15).
//   - Worker thread (next task): the joint-refined design that meets R12, swapped in once a
//     band's parameters settle. Until then the audio uses the three-point design.
//
// Every section must map to a stable SVF. If one does not (for example f0 far above Nyquist),
// the band falls back to bilinear pre-warped at min(f0, 0.45 fs); if that also fails the
// function returns false and the band is bypassed.

#include "amq/dsp/svf.hpp"

namespace amq::dsp {

struct SvfCascade {
    FixedVector<SvfParams, kMaxDigitalSections> sections;
};

// Exact response of the cascade the audio runs, at hz for sample rate fs (area 04 R10).
[[nodiscard]] inline std::complex<double> svfCascadeResponseHz(const SvfCascade& c, double hz, double fs) noexcept {
    const double w = 2.0 * 3.14159265358979323846 * hz / fs;
    const std::complex<double> z{std::cos(w), std::sin(w)};
    std::complex<double> h{1.0, 0.0};
    for (const auto& p : c.sections) h *= svfResponse(p, z);
    return h;
}

enum class RealtimeDesignResult : uint8_t { Matched, BilinearFallback, Unsupported };

[[nodiscard]] RealtimeDesignResult designBandRealtime(const BandSpec& spec, double sampleRate,
                                                      SvfCascade& out) noexcept;

}  // namespace amq::dsp
