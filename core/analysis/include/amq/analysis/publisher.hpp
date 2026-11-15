#pragma once
// Snapshot publisher (areas 11, 12, 14). The engine feeds it on the audio thread; the editor
// pulls from it on the GUI thread. One triple buffer per data type (area 11 design).
//
// Peaks are never lost (area 11 R4): each frame's peaks cover the time since the previous
// frame, and if that previous frame is still unread when the next one is written, its peaks
// are max-merged into the new frame, which replaces it. Publishing runs whether or not an
// editor reads, with fixed memory and no allocation (area 14 R14).

#include "amq/analysis/snapshot.hpp"
#include "amq/analysis/triple_buffer.hpp"

#include <cstdint>

namespace amq::analysis {

inline constexpr double kPublishHz = 60.0;  // PROVISIONAL: twice the ~30 Hz editor pull rate
inline constexpr double kRmsMs = 300.0;     // PROVISIONAL RMS integration time
inline constexpr double kHoldMs = 1500.0;   // PROVISIONAL peak-hold time

// Linear magnitude to dB, clamped to the floor for zero, denormal or non-finite input.
float toDb(double linear) noexcept;

// Peak, RMS and peak hold for up to kMaxChannels channels.
class LevelMeter {
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void process(const float* const* x, uint32_t channels, uint32_t frames) noexcept;
    // Writes the frame values and starts a new peak interval. mergePrevious: the previous frame
    // was not read, so its peaks are carried into this one.
    void emit(ChannelLevel* out, bool mergePrevious) noexcept;

private:
    struct Channel {
        double peak = 0.0;     // since the last emit
        double pending = 0.0;  // the peak the last emitted frame carried
        double ms = 0.0;       // smoothed mean square
        double hold = 0.0;
        int64_t holdLeft = 0;
    };
    std::array<Channel, kMaxChannels> ch_{};
    double rmsCoeff_ = 0.0;
    int64_t holdSamples_ = 0;
};

class SnapshotPublisher {
public:
    SnapshotPublisher();

    // ---- Audio thread (or prepare, with no reader running) ----
    void prepare(double sampleRate, uint32_t channels) noexcept;
    void meterInput(const float* const* x, uint32_t channels, uint32_t frames) noexcept { input_.process(x, channels, frames); }
    void meterOutput(const float* const* x, uint32_t channels, uint32_t frames) noexcept {
        channels_ = channels < 1 ? 1 : channels > kMaxChannels ? kMaxChannels : channels;
        output_.process(x, channels, frames);
    }
    // External side chain, when the host feeds it this block (area 01 R2).
    void meterSidechain(const float* const* x, uint32_t channels, uint32_t frames) noexcept {
        scChannels_ = channels < 1 ? 1 : channels > kMaxChannels ? kMaxChannels : channels;
        sidechain_.process(x, channels, frames);
    }
    void setGainChange(int band, float db) noexcept;
    // Advance the clock; publishes when an interval has passed. samplePosition: at block end.
    void advance(uint32_t frames, uint64_t samplePosition) noexcept;
    [[nodiscard]] uint64_t publishedCount() const noexcept { return levelStamp_; }

    // ---- GUI thread (one reader) ----
    // The newest frame, or nullptr if nothing new since the last pull of that type.
    const LevelSnapshot* pullLevels() noexcept { return levels_.acquire() ? &levels_.front() : nullptr; }
    const GainChangeSnapshot* pullGainChange() noexcept { return gain_.acquire() ? &gain_.front() : nullptr; }
    const SpectrumSnapshot* pullSpectrum() noexcept { return spectrum_.acquire() ? &spectrum_.front() : nullptr; }

private:
    void publish(uint64_t samplePosition) noexcept;

    TripleBuffer<LevelSnapshot> levels_;
    TripleBuffer<GainChangeSnapshot> gain_;
    TripleBuffer<SpectrumSnapshot> spectrum_;  // written by the analyzer (area 19)
    LevelMeter input_, output_, sidechain_;
    uint32_t scChannels_ = 0;  // side-chain channels seen since the last publish
    uint32_t channels_ = 2;
    uint32_t interval_ = 800;
    uint32_t sincePublish_ = 0;
    uint64_t levelStamp_ = 0, gainStamp_ = 0;
    std::array<float, kBandCount> gainNow_{};
    std::array<float, kBandCount> gainPeak_{};     // since the last publish
    std::array<float, kBandCount> gainPending_{};  // what the last published frame carried
};

}  // namespace amq::analysis
