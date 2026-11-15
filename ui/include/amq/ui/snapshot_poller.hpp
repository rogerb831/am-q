#pragma once
// The editor side of the snapshot feed (area 11 R3/R4, area 12 R6/R10/R11). The editor calls
// poll() about 30 times a second from a timer on Visage's display-paced event loop. poll()
// takes only frames whose stamp is newer than the last one seen, and reports which displays
// actually changed, so an idle editor (silence, nothing moving) redraws nothing.

#include "amq/analysis/snapshot.hpp"
#include "amq/ui/edit_controller.hpp"

#include <cmath>
#include <cstdint>

namespace amq::ui {

class SnapshotPoller {
public:
    enum Changed : unsigned { kNone = 0, kLevels = 1u << 0, kGainChange = 1u << 1, kSpectrum = 1u << 2 };

    static constexpr double kPollMs = 33.0;      // about 30 Hz (area 12 R10)
    static constexpr float kRedrawStepDb = 0.1f;  // smaller level moves are not redrawn

    unsigned poll(EditController& c) noexcept {
        ++polls_;
        unsigned changed = kNone;
        if (const auto* f = c.pullLevels(); f && take(f->header, levelStamp_)) {
            if (differs(*f, levels_)) changed |= kLevels;
            levels_ = *f;
        }
        if (const auto* f = c.pullGainChange(); f && take(f->header, gainStamp_)) {
            if (differs(f->currentDb, gain_.currentDb) || differs(f->peakDb, gain_.peakDb)) changed |= kGainChange;
            gain_ = *f;
        }
        if (const auto* f = c.pullSpectrum(); f && take(f->header, spectrumStamp_)) {
            spectrum_ = f;  // the front buffer stays valid until the next pull
            changed |= kSpectrum;
        }
        return changed;
    }

    // Silence floor until the first frame arrives (area 12 R11).
    [[nodiscard]] const analysis::LevelSnapshot& levels() const noexcept { return levels_; }
    [[nodiscard]] const analysis::GainChangeSnapshot& gainChange() const noexcept { return gain_; }
    [[nodiscard]] const analysis::SpectrumSnapshot* spectrum() const noexcept { return spectrum_; }
    [[nodiscard]] uint64_t polls() const noexcept { return polls_; }
    [[nodiscard]] uint64_t framesTaken() const noexcept { return taken_; }
    [[nodiscard]] uint64_t staleSkipped() const noexcept { return stale_; }

private:
    bool take(const analysis::SnapshotHeader& h, uint64_t& last) noexcept {
        if (h.stamp <= last) {  // never treat an old frame as new
            ++stale_;
            return false;
        }
        last = h.stamp;
        ++taken_;
        return true;
    }
    static bool moved(float a, float b) noexcept { return std::abs(a - b) >= kRedrawStepDb; }
    template <typename A>
    static bool differs(const A& a, const A& b) noexcept {
        for (size_t i = 0; i < a.size(); ++i)
            if (moved(a[i], b[i])) return true;
        return false;
    }
    static bool differs(const analysis::LevelSnapshot& a, const analysis::LevelSnapshot& b) noexcept {
        if (a.channels != b.channels || a.sidechainChannels != b.sidechainChannels) return true;
        for (size_t c = 0; c < a.input.size(); ++c)
            if (differs(a.input[c], b.input[c]) || differs(a.output[c], b.output[c]) ||
                differs(a.sidechain[c], b.sidechain[c]))
                return true;
        return false;
    }
    static bool differs(const analysis::ChannelLevel& a, const analysis::ChannelLevel& b) noexcept {
        return moved(a.peakDb, b.peakDb) || moved(a.rmsDb, b.rmsDb) || moved(a.holdDb, b.holdDb);
    }

    analysis::LevelSnapshot levels_{};
    analysis::GainChangeSnapshot gain_{};
    const analysis::SpectrumSnapshot* spectrum_ = nullptr;
    uint64_t levelStamp_ = 0, gainStamp_ = 0, spectrumStamp_ = 0;
    uint64_t polls_ = 0, taken_ = 0, stale_ = 0;
};

}  // namespace amq::ui
