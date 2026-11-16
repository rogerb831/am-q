#pragma once
// Dynamic EQ core (area 06; area 07 R7-R11; area 08 R1, R2). The structure follows Giannoulis,
// Massberg and Reiss, "Digital Dynamic Range Compressor Design" (JAES 2012): a level detector in
// dB, a soft-knee gain computer, and smoothing in the dB domain. Everything here is ours: the
// tapers, the frequency scaling, the dual release and every constant, which are PROVISIONAL until
// the area 06 listening tests freeze them.
//
// Timing spec (area 06 task "Write the timing spec"):
//   - Every time is a 90 % time (area 01 R20): the time to cover 90 % of a dB step, at any rate.
//   - Knob tapers: Attack and Release (0-100) map to time on a piecewise log curve through three
//     anchors (knob 0, 50, 100). Attack 0.25 / 10 / 250 ms, Release 5 / 250 / 1000 ms. Log time
//     makes the knob perceptually even; the midpoint gives the musical defaults (area 06 design).
//   - Frequency-aware attack (R4): the attack anchors are for a band at 1 kHz; a band at f scales
//     them by (1 kHz / f)^0.25 (100 Hz: x1.78; 10 kHz: x0.56; ratio 100 Hz : 10 kHz = 3.16). The
//     attack is never shorter than one period of the band centre (minimum-cycles rule), which
//     keeps the gain from moving at waveform rate on bass.
//   - Program-dependent release (R5), dual release: a fast path (the attack, then a 40 ms release,
//     or the Release time if shorter) and a slow path (an attack of at least 150 ms, then the
//     Release time) run in parallel, and the larger gain reduction wins. A short peak barely
//     moves the slow path, so it recovers at the fast release; sustained reduction fills the slow
//     path, so it recovers at the Release time. A final attack/release stage then shapes the
//     result; it is the identity (both times 0) until listening asks for more.
//   - Dyn Auto uses the knob-50 timing; the stored knob values are kept (R6).
//
// Gain computer: gain reduction (0 or more dB) grows one dB per dB the side chain is over the
// threshold (the band holds the side-chain level at the threshold until the range is used up),
// with a quadratic soft knee of width W centred on the threshold, and is capped at |range| (R7).
// The sign of the range then makes it a cut or a boost. Knee width (area 07 R11): 3 dB plus
// 1.5 dB per octave of band width plus 0.2 dB per dB of |range|.
//
// Level detector (area 06 open question "peak or short-RMS", PROVISIONAL): a peak follower with an
// instant attack and a release of 4 periods of the band frequency (at least 1 ms), on the
// side-chain envelope. For a band-pass side chain the envelope is the SVF quadrature envelope
// (svf.hpp, svfBandEnvelope), which has no ripple on a tone; otherwise it is |x|.
//
// Auto threshold (area 07 R7-R10, area 08 R1, R2): a reference envelope of the side-chain level
// with an instant attack and a 10 s release (90 % time), independent of the Attack and Release
// knobs. For the first 0.5 s after a reset it releases in 0.25 s instead, so it settles quickly.
// It never falls below -100 dBFS, and the detector never reports below -150 dBFS, so silence and
// very low input give no gain change. The threshold is the reference minus |range|, with the
// offset capped at 30 dB (R8).

#include "amq/dsp/conventions.hpp"
#include "amq/dsp/db_smoother.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace amq::dsp {

inline constexpr double kAttackMinMs = 0.25, kAttackMidMs = 10.0, kAttackMaxMs = 250.0;
inline constexpr double kReleaseMinMs = 5.0, kReleaseMidMs = 250.0, kReleaseMaxMs = 1000.0;
inline constexpr double kAttackRefHz = 1000.0;
inline constexpr double kAttackFreqExponent = 0.25;
inline constexpr double kAttackMinCycles = 1.0;
inline constexpr double kFastReleaseMs = 40.0;
inline constexpr double kSlowAttackMs = 150.0;
inline constexpr double kFinalAttackMs = 0.0, kFinalReleaseMs = 0.0;
inline constexpr double kDefaultTimingKnob = 50.0;

inline constexpr double kKneeBaseDb = 3.0, kKneePerOctaveDb = 1.5, kKneePerRangeDb = 0.2;
inline constexpr double kShelfBandwidthOctaves = 2.0;  // nominal width of shelves and tilts for the knee

inline constexpr double kDetectorReleaseCycles = 4.0;
inline constexpr double kDetectorMinReleaseMs = 1.0;
inline constexpr double kLevelFloorDb = -150.0;
inline constexpr double kRefFloorDb = -100.0;
inline constexpr double kRefReleaseSeconds = 10.0;
inline constexpr double kRefWarmupSeconds = 0.5;
inline constexpr double kRefWarmupReleaseSeconds = 0.25;
inline constexpr double kAutoOffsetCapDb = 30.0;

// Piecewise log taper through (0, lo), (50, mid), (100, hi), in ms.
[[nodiscard]] inline double knobTaperMs(double knob, double lo, double mid, double hi) noexcept {
    const double k = std::clamp(knob, 0.0, 100.0);
    return k <= 50.0 ? lo * std::pow(mid / lo, k / 50.0) : mid * std::pow(hi / mid, (k - 50.0) / 50.0);
}

[[nodiscard]] inline double attackSeconds(double knob, double bandHz) noexcept {
    const double hz = std::max(bandHz, 1.0);
    const double t = knobTaperMs(knob, kAttackMinMs, kAttackMidMs, kAttackMaxMs) * 1e-3 *
                     std::pow(kAttackRefHz / hz, kAttackFreqExponent);
    return std::max(t, kAttackMinCycles / hz);
}

[[nodiscard]] inline double releaseSeconds(double knob) noexcept {
    return knobTaperMs(knob, kReleaseMinMs, kReleaseMidMs, kReleaseMaxMs) * 1e-3;
}

[[nodiscard]] inline double kneeWidthDb(double bandwidthOctaves, double rangeDb) noexcept {
    return kKneeBaseDb + kKneePerOctaveDb * std::max(bandwidthOctaves, 0.0) + kKneePerRangeDb * std::abs(rangeDb);
}

// Gain reduction in dB (>= 0) for a side-chain level overDb above the threshold.
[[nodiscard]] inline double gainComputerDb(double overDb, double kneeDb, double capDb) noexcept {
    double gr;
    if (kneeDb > 0.0 && std::abs(overDb) < 0.5 * kneeDb) {
        const double u = overDb + 0.5 * kneeDb;
        gr = u * u / (2.0 * kneeDb);
    } else {
        gr = overDb > 0.0 ? overDb : 0.0;
    }
    return std::min(gr, std::max(capDb, 0.0));
}

// Detector level in dB of an absolute sample value, floored.
[[nodiscard]] inline double detectorLevelDb(double absValue) noexcept {
    // 20 log10(x) = (20 log10 2) log2(x); log2 is the cheaper call.
    return absValue > 3.1622776601683795e-8 ? 6.020599913279624 * std::log2(absValue) : kLevelFloorDb;  // -150 dB
}

[[nodiscard]] inline double autoThresholdDb(double referenceDb, double rangeDb) noexcept {
    return referenceDb - std::min(std::abs(rangeDb), kAutoOffsetCapDb);
}

// Peak follower on a linear envelope: instant attack, one-pole release (90 % time).
class PeakDetector {
public:
    void setRelease(double seconds, double sampleRate) noexcept {
        c_ = onePoleCoefficientFor90PercentTime(seconds, sampleRate);
    }
    void reset() noexcept { env_ = 0.0; }
    double process(double envelope) noexcept {
        if (!(envelope == envelope)) return env_;  // NaN: hold
        env_ = envelope >= env_ ? envelope : env_ + c_ * (envelope - env_);
        return env_;
    }
    [[nodiscard]] double value() const noexcept { return env_; }

private:
    double env_ = 0.0, c_ = 1.0;
};

[[nodiscard]] inline double detectorReleaseSeconds(double bandHz) noexcept {
    return std::max(kDetectorReleaseCycles / std::max(bandHz, 1.0), kDetectorMinReleaseMs * 1e-3);
}

// Auto-threshold reference envelope (instant attack, slow release, warm-up, floor).
class ReferenceEnvelope {
public:
    void prepare(double sampleRate) noexcept {
        cSlow_ = onePoleCoefficientFor90PercentTime(kRefReleaseSeconds, sampleRate);
        cWarm_ = onePoleCoefficientFor90PercentTime(kRefWarmupReleaseSeconds, sampleRate);
        warmSamples_ = static_cast<uint64_t>(std::llround(kRefWarmupSeconds * sampleRate));
        reset();
    }
    // Starts the warm-up again (reset, transport start, Auto switched on: area 07 R9).
    void reset() noexcept {
        env_ = kRefFloorDb;
        count_ = 0;
    }
    double process(double levelDb) noexcept {
        if (!std::isfinite(levelDb)) return env_;
        if (levelDb > env_) env_ = levelDb;
        else env_ += (count_ < warmSamples_ ? cWarm_ : cSlow_) * (levelDb - env_);
        if (env_ < kRefFloorDb) env_ = kRefFloorDb;
        if (count_ < warmSamples_) ++count_;
        return env_;
    }
    [[nodiscard]] double value() const noexcept { return env_; }
    [[nodiscard]] bool warmingUp() const noexcept { return count_ < warmSamples_; }

private:
    double env_ = kRefFloorDb;
    double cSlow_ = 0.0, cWarm_ = 0.0;
    uint64_t warmSamples_ = 0, count_ = 0;
};

// One band's gain-reduction smoothing: the dual release and the final stage.
class BandDynamics {
public:
    void prepare(double sampleRate) noexcept {
        fast_.prepare(sampleRate);
        slow_.prepare(sampleRate);
        final_.prepare(sampleRate);
        final_.setTimes(kFinalAttackMs * 1e-3, kFinalReleaseMs * 1e-3);
        reset();
    }
    void setTimes(double attackS, double releaseS) noexcept {
        fast_.setTimes(attackS, std::min(kFastReleaseMs * 1e-3, releaseS));
        slow_.setTimes(std::max(kSlowAttackMs * 1e-3, attackS), releaseS);
    }
    void reset() noexcept {
        fast_.reset(0.0);
        slow_.reset(0.0);
        final_.reset(0.0);
    }
    // Target gain reduction (dB, >= 0) in, smoothed gain reduction out.
    double process(double targetDb) noexcept {
        const double a = fast_.process(targetDb);
        const double b = slow_.process(targetDb);
        return final_.process(std::max(a, b));
    }
    [[nodiscard]] double value() const noexcept { return final_.value(); }

private:
    DbAttackRelease fast_, slow_, final_;
};

}  // namespace amq::dsp
