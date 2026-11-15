#pragma once
// Audio-side parameter ramps (area 09 R13/R14, area 10 R3, area 01 R3/R20).
//
// Linear per-sample ramps that retarget from their current value. The ramp time depends on
// the size of the move: small moves get the long time, larger moves a shorter one, and moves
// at or above the jump threshold are instant (area 09 design). Times are in ms, independent of
// the sample rate, and mean the time to cover 90 % of the step (area 01 R20).

#include <cmath>
#include <cstdint>

namespace amq::engine {

struct RampPolicy {
    double longMs = 20.0;   // time for the smallest moves
    double shortMs = 5.0;   // time just below the jump threshold
    double jump = 1.0;      // |move| at or above this is instant (plain units of the field)
    bool neverInstant = false;  // e.g. polarity flips always ramp
};

// Ramp length in samples for a move of size |delta| (0 = instant).
[[nodiscard]] inline uint32_t rampSamples(const RampPolicy& p, double delta, double sampleRate) noexcept {
    const double d = std::abs(delta);
    if (d == 0.0) return 0;
    if (d >= p.jump && !p.neverInstant) return 0;
    const double f = std::fmin(d / p.jump, 1.0);
    const double ms90 = p.longMs - (p.longMs - p.shortMs) * f;
    // A linear ramp of length L covers 90 % of the step at 0.9 L.
    const double samples = (ms90 / 0.9) * 1e-3 * sampleRate;
    return samples < 1.0 ? 0u : static_cast<uint32_t>(std::lround(samples));
}

// The value is closed-form, start + step * count, so it is identical however the advance is
// chunked (per sample, per read, per piece). That keeps output independent of host block
// sizes.
class LinearRamp {
public:
    void snap(double v) noexcept {
        start_ = tgt_ = v;
        inc_ = 0.0;
        total_ = done_ = 0;
    }
    void retarget(double target, uint32_t samples) noexcept {
        const double from = current();
        if (samples == 0) {
            snap(target);
            return;
        }
        start_ = from;
        tgt_ = target;
        inc_ = (tgt_ - start_) / static_cast<double>(samples);
        total_ = samples;
        done_ = 0;
    }
    // Value for the next sample. A ramp started at sample k already moves at sample k.
    double next() noexcept {
        if (done_ < total_) ++done_;
        return current();
    }
    void advance(uint64_t n) noexcept {
        const uint64_t left = total_ - done_;
        done_ += static_cast<uint32_t>(n < left ? n : left);
    }
    [[nodiscard]] double current() const noexcept {
        return done_ >= total_ ? tgt_ : start_ + inc_ * static_cast<double>(done_);
    }
    // Value n samples after the current one (ramps are deterministic, so designs can look ahead).
    [[nodiscard]] double valueAhead(uint64_t n) const noexcept {
        const uint64_t d = done_ + n;
        return d >= total_ ? tgt_ : start_ + inc_ * static_cast<double>(d);
    }
    [[nodiscard]] double target() const noexcept { return tgt_; }
    [[nodiscard]] bool active() const noexcept { return done_ < total_; }

private:
    double start_ = 0.0, tgt_ = 0.0, inc_ = 0.0;
    uint32_t total_ = 0, done_ = 0;
};

}  // namespace amq::engine
