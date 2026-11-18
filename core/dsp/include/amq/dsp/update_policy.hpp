#pragma once
// Coefficient update and structure-change policy (plan area 01, R15 and R17).
//
// The values below (4-sample parameter read, 32-sample minimum redesign interval, 100 ms
// section-count crossfade) come from area 01 R15/R17. Their counsel review was marked complete
// on 2026-11-18 (CLEAN_ROOM.md). They live behind this interface so the schedule and the values
// can change without touching DSP code.
// Nothing in the Phase 0 sandbox consumes this yet; the redesign scheduler and the
// crossfade are Phase 1/2 tasks.
//
// R18 (TPT/ZDF SVF kernel details) is likewise PROVISIONAL and not implemented here.

namespace amq::dsp {

struct UpdatePolicy {
    virtual ~UpdatePolicy() = default;

    // How often (in samples) parameter ramps are read.          PROVISIONAL (R15)
    [[nodiscard]] virtual int parameterReadIntervalSamples() const noexcept = 0;

    // Minimum interval (in samples) between coefficient redesigns. PROVISIONAL (R15)
    [[nodiscard]] virtual int minimumRedesignIntervalSamples() const noexcept = 0;

    // Frequency jump (octaves) that forces an immediate redesign.  PROVISIONAL (R15)
    [[nodiscard]] virtual double immediateRedesignJumpOctaves() const noexcept = 0;

    // Crossfade length (seconds) when a band's section count changes. PROVISIONAL (R17)
    [[nodiscard]] virtual double sectionCountCrossfadeSeconds() const noexcept = 0;
};

// Defaults taken from the plan text (area 01 R15/R17).
struct DefaultUpdatePolicy final : UpdatePolicy {
    [[nodiscard]] int parameterReadIntervalSamples() const noexcept override { return 4; }
    [[nodiscard]] int minimumRedesignIntervalSamples() const noexcept override { return 32; }
    // The plan says "a large frequency jump" without a number; 1 octave is a
    // placeholder for the Phase 1 zipper tests to tune.
    [[nodiscard]] double immediateRedesignJumpOctaves() const noexcept override { return 1.0; }
    [[nodiscard]] double sectionCountCrossfadeSeconds() const noexcept override { return 0.100; }
};

}  // namespace amq::dsp
