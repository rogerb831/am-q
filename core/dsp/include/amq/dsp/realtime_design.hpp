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

enum class RealtimeDesignResult : uint8_t { Matched, BilinearFallback, Unsupported };

[[nodiscard]] RealtimeDesignResult designBandRealtime(const BandSpec& spec, double sampleRate,
                                                      SvfCascade& out) noexcept;

}  // namespace amq::dsp
