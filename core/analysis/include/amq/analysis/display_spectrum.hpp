#pragma once
// First display consumer of the analyzer (area 19 R8, area 12 R10): maps each source's latest
// frame onto the shared 512-point log grid in dBFS and publishes the pre, post and external
// spectra through the publisher's spectrum triple buffer, at most once per analyzer pass.
//
// Display mapping (a display stage, never fed to EQ Match): a grid point that covers one or
// more FFT bins takes the largest of them, so a sine reads its level; a point narrower than a
// bin interpolates the power linearly between the two nearest bins. Amplitude scaling: a sine
// of peak amplitude A centred on a bin reads 20 log10(A) dBFS.
//
// Display follower (area 19 task "display follower with millisecond rise and fall, freeze, peak-hold
// and tilt"; a display stage, never fed to EQ Match). Each grid point follows its new value in dB
// with a one-pole rise and fall (90 % times from Analyzer Speed, below; the coefficient comes from
// each frame's real interval, so it is independent of FFT size and overlap). Tilt adds
// dB-per-octave about 1 kHz. Freeze stops updating. The post spectrum also keeps a peak hold that
// falls at kPeakFallDbPerSecond. All PROVISIONAL until the Product designer tunes them.
//   Speed           Very Slow  Slow   Medium  Fast   Very Fast
//   rise (ms)       60         40     25      12     6
//   fall (ms)       3000       1500   800     400    200

#include "amq/analysis/analyzer.hpp"
#include "amq/analysis/publisher.hpp"
#include "amq/dsp/log_grid.hpp"

#include <array>
#include <atomic>

namespace amq::analysis {

class DisplaySpectrum final : public FrameConsumer {
public:
    // Main thread, with the analyzer worker stopped. fftSize: the analyzer's display size.
    void prepare(SnapshotPublisher* publisher, double sampleRate, int fftSize);

    void onFrame(const SpectrumFrame& f) noexcept override;
    void onPassEnd() noexcept override;

    // Any thread (the engine, every block): Analyzer Speed (0-4), Freeze, Tilt in dB per octave.
    void setSettings(int speed, bool freeze, double tiltDbPerOctave) noexcept {
        speed_.store(speed, std::memory_order_relaxed);
        freeze_.store(freeze, std::memory_order_relaxed);
        tilt_.store(tiltDbPerOctave, std::memory_order_relaxed);
    }
    static constexpr std::array<double, 5> kRiseMs = {60, 40, 25, 12, 6};
    static constexpr std::array<double, 5> kFallMs = {3000, 1500, 800, 400, 200};
    static constexpr double kPeakFallDbPerSecond = 6.0;

    // Worker thread (tests): the latest values of a source, in dBFS per grid point.
    [[nodiscard]] const std::array<float, dsp::kLogGridPoints>& latest(Source s) const noexcept {
        return latest_[static_cast<size_t>(s)];
    }

private:
    struct Point {
        int lo = 0, hi = -1;  // covered bins, inclusive; hi < lo means interpolate
        int k0 = 0;           // interpolation: bins k0 and k0 + 1
        float frac = 0.0f;
    };
    SnapshotPublisher* publisher_ = nullptr;
    int fftSize_ = 0;
    std::array<Point, dsp::kLogGridPoints> map_{};
    std::array<std::array<float, dsp::kLogGridPoints>, kSourceCount> latest_{};   // followed, tilted
    std::array<double, dsp::kLogGridPoints> tiltOf_{};                             // log2(f / 1 kHz)
    std::array<float, dsp::kLogGridPoints> peak_{};
    std::array<uint64_t, kSourceCount> lastPos_{};
    double sampleRate_ = 48000.0;
    std::atomic<int> speed_{2};
    std::atomic<bool> freeze_{false};
    std::atomic<double> tilt_{0.0};
    bool dirty_ = false;
    uint64_t position_ = 0;
};

}  // namespace amq::analysis
