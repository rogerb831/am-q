#include "amq/analysis/publisher.hpp"

#include <algorithm>
#include <cmath>

namespace amq::analysis {

namespace {
// Of two signed gain changes, the one with the larger magnitude.
float larger(float a, float b) noexcept { return std::abs(b) > std::abs(a) ? b : a; }
}  // namespace

float toDb(double linear) noexcept {
    if (!(linear > 1e-30) || !std::isfinite(linear)) return kFloorDb;
    return std::max(kFloorDb, static_cast<float>(20.0 * std::log10(linear)));
}

// ---------------------------------------------------------------------------
// LevelMeter
// ---------------------------------------------------------------------------

void LevelMeter::prepare(double sampleRate) noexcept {
    rmsCoeff_ = 1.0 - std::exp(-1.0 / (kRmsMs * 1e-3 * sampleRate));
    holdSamples_ = static_cast<int64_t>(std::llround(kHoldMs * 1e-3 * sampleRate));
    reset();
}

void LevelMeter::reset() noexcept { ch_ = {}; }

void LevelMeter::process(const float* const* x, uint32_t channels, uint32_t frames) noexcept {
    channels = std::min(channels, kMaxChannels);
    for (uint32_t c = 0; c < channels; ++c) {
        if (!x[c]) continue;
        auto& m = ch_[c];
        double peak = 0.0, ms = m.ms;
        for (uint32_t i = 0; i < frames; ++i) {
            const double v = x[c][i];
            peak = std::max(peak, std::abs(v));
            ms += rmsCoeff_ * (v * v - ms);
        }
        m.ms = ms < 1e-30 ? 0.0 : ms;  // no denormal tail in silence
        m.peak = std::max(m.peak, peak);
        if (peak >= m.hold) {
            m.hold = peak;
            m.holdLeft = holdSamples_;
        } else if ((m.holdLeft -= frames) <= 0) {
            m.hold = peak;
            m.holdLeft = holdSamples_;
        }
    }
}

void LevelMeter::emit(ChannelLevel* out, bool mergePrevious) noexcept {
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        auto& m = ch_[c];
        const double peak = mergePrevious ? std::max(m.peak, m.pending) : m.peak;
        out[c].peakDb = toDb(peak);
        out[c].rmsDb = toDb(std::sqrt(m.ms));
        out[c].holdDb = toDb(m.hold);
        m.pending = peak;
        m.peak = 0.0;
    }
}

// ---------------------------------------------------------------------------
// SnapshotPublisher
// ---------------------------------------------------------------------------

SnapshotPublisher::SnapshotPublisher() { prepare(48000.0, 2); }

void SnapshotPublisher::prepare(double sampleRate, uint32_t channels) noexcept {
    channels_ = std::clamp<uint32_t>(channels, 1, kMaxChannels);
    interval_ = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(sampleRate / kPublishHz)));
    sincePublish_ = 0;
    input_.prepare(sampleRate);
    output_.prepare(sampleRate);
    sidechain_.prepare(sampleRate);
    scChannels_ = 0;
    gainNow_.fill(0.0f);
    gainPeak_.fill(0.0f);
    gainPending_.fill(0.0f);
    // Stamps keep counting across prepare, so a reader never mistakes a new frame for an old one.
}

void SnapshotPublisher::setGainChange(int band, float db) noexcept {
    if (band < 0 || band >= kBandCount) return;
    const auto b = static_cast<size_t>(band);
    gainNow_[b] = db;
    gainPeak_[b] = larger(gainPeak_[b], db);
}

void SnapshotPublisher::advance(uint32_t frames, uint64_t samplePosition) noexcept {
    sincePublish_ += frames;
    if (sincePublish_ < interval_) return;
    // Keep the remainder so the average rate is kPublishHz for any block size; at most one
    // publish per block (a block longer than the interval gets one frame).
    sincePublish_ = std::min(sincePublish_ - interval_, interval_ - 1);
    publish(samplePosition);
}

void SnapshotPublisher::publish(uint64_t samplePosition) noexcept {
    {
        auto& f = levels_.back();
        const bool merge = levels_.unread();
        f.header = {++levelStamp_, samplePosition};
        f.channels = channels_;
        input_.emit(f.input.data(), merge);
        output_.emit(f.output.data(), merge);
        if (scChannels_ == 0) sidechain_.reset();  // disconnected: back to the floor
        sidechain_.emit(f.sidechain.data(), merge);
        f.sidechainChannels = scChannels_;
        scChannels_ = 0;
        levels_.publish();
    }
    {
        auto& f = gain_.back();
        const bool merge = gain_.unread();
        f.header = {++gainStamp_, samplePosition};
        for (size_t b = 0; b < gainNow_.size(); ++b) {
            const float peak = merge ? larger(gainPeak_[b], gainPending_[b]) : gainPeak_[b];
            f.currentDb[b] = gainNow_[b];
            f.peakDb[b] = peak;
            gainPending_[b] = peak;
            gainPeak_[b] = gainNow_[b];
        }
        gain_.publish();
    }
}

}  // namespace amq::analysis
