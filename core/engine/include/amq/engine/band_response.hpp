#pragma once
// Band magnitude responses from the exact filters the audio runs (area 04 R10, area 16 R4,
// area 12 R12). One conversion from parameter values to a band spec and one design call per
// tier are shared by the engine, the design worker and every response user (GUI curve tests,
// Auto Gain, EQ Match), so a response can never drift from what is heard.

#include "amq/dsp/realtime_design.hpp"
#include "amq/engine/design_worker.hpp"
#include "amq/params/parameter_table.hpp"

#include <cstddef>

namespace amq::engine {

// Which design the response comes from.
enum class DesignTier : uint8_t {
    Fast,     // the three-point design the audio thread uses while parameters move
    Refined,  // the refined design the audio settles on (area 01 R12)
};

// Parameter-domain inputs (log2 Hz, dB after Gain Scale, Q position) to a design spec.
[[nodiscard]] dsp::BandSpec bandSpecFrom(const DesignInputs& in) noexcept;

// The settled design inputs of one band, read from the table as the engine reads them:
// Gain Scale applied and the result clamped to +/-30 dB (area 01 R13).
[[nodiscard]] DesignInputs bandDesignInputs(const params::ParameterTable& t, int band, double sampleRate) noexcept;

// Designs the band as the audio path does. False if the band has no usable design (the engine
// then bypasses it).
[[nodiscard]] bool designBandCascade(const DesignInputs& in, DesignTier tier, dsp::SvfCascade& out);

// Magnitude in dB of the band at n frequencies. False (outDb untouched) if no design.
[[nodiscard]] bool bandMagnitudeDb(const DesignInputs& in, DesignTier tier, const double* hz, size_t n,
                                   double* outDb);

}  // namespace amq::engine
