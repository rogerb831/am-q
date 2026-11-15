#pragma once
// dB-domain one-pole attack/release smoother (area 06 R9, R11; design "Coefficients").
//
// A textbook one-pole smoother on a dB value, with separate coefficients for the two
// directions (Giannoulis, Massberg and Reiss, "Digital Dynamic Range Compressor Design",
// JAES 2012, the smooth decoupled form). The value moves with the attack coefficient while
// the target is above it and with the release coefficient while the target is below it. The
// dynamics feed it gain reduction in dB (0 or more), so "above" means more reduction.
//
// Times follow the shared convention (conventions.hpp, area 01 R20): a time is the time to
// cover 90 % of a step, exactly, at any sample rate. A time of 0 is instant: the value equals
// the target on the same sample. Coefficients are recomputed only when a time or the sample
// rate changes. Non-finite targets are ignored, and a value within 1e-9 dB of its target
// snaps onto it, so a settled smoother never carries a denormal tail.
//
// Real-time safe: no allocation, no locks, no system calls.

#include "amq/dsp/conventions.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace amq::dsp {

class DbAttackRelease {
public:
    static constexpr double kSnapDb = 1e-9;

    void prepare(double sampleRate) noexcept {
        if (sampleRate != sampleRate_) {
            sampleRate_ = sampleRate;
            updateCoefficients();
        }
    }

    // Seconds; 0 (or negative) is instant. Safe to call every block: no work unless changed.
    void setTimes(double attackSeconds, double releaseSeconds) noexcept {
        if (attackSeconds == attack_ && releaseSeconds == release_) return;
        attack_ = attackSeconds;
        release_ = releaseSeconds;
        updateCoefficients();
    }

    void reset(double valueDb = 0.0) noexcept { y_ = std::isfinite(valueDb) ? valueDb : 0.0; }

    double process(double targetDb) noexcept {
        if (!std::isfinite(targetDb)) return y_;  // hold on garbage input (fuzz)
        const double d = targetDb - y_;
        if (std::abs(d) <= kSnapDb) {
            y_ = targetDb;
            return y_;
        }
        y_ += (d > 0.0 ? ca_ : cr_) * d;
        return y_;
    }

    void processBlock(const double* targetDb, double* outDb, size_t n) noexcept {
        for (size_t i = 0; i < n; ++i) outDb[i] = process(targetDb[i]);
    }

    [[nodiscard]] double value() const noexcept { return y_; }
    [[nodiscard]] double attackCoefficient() const noexcept { return ca_; }
    [[nodiscard]] double releaseCoefficient() const noexcept { return cr_; }
    [[nodiscard]] uint64_t coefficientUpdates() const noexcept { return updates_; }

private:
    void updateCoefficients() noexcept {
        ca_ = onePoleCoefficientFor90PercentTime(attack_, sampleRate_);
        cr_ = onePoleCoefficientFor90PercentTime(release_, sampleRate_);
        ++updates_;
    }

    double sampleRate_ = 48000.0;
    double attack_ = 0.0, release_ = 0.0;
    double ca_ = 1.0, cr_ = 1.0;
    double y_ = 0.0;
    uint64_t updates_ = 0;
};

}  // namespace amq::dsp
