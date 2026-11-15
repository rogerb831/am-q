#pragma once
// Refined band designs off the audio thread (area 01 R12, R19; docs/plan/areas/01 "Design tiers").
//
// The audio thread designs bands with the fast three-point design while parameters move. Once
// a band settles it posts a request here; a worker thread runs the joint-refined design (1-66
// ms per band, allocates) and posts the result back. Per band there is one request slot,
// written only by the audio thread, and one result slot, written only by the worker. Both are
// sequence-locked: a reader that sees a write in progress simply tries again at its next read,
// so the audio thread never waits.

#include "amq/dsp/realtime_design.hpp"
#include "amq/params/param_spec.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

namespace amq::engine {

struct DesignInputs {
    double sampleRate = 0.0;
    double freqLog2 = 0.0, gainDb = 0.0, qPos = 0.0;
    int shape = -1, slope = -1;
    bool operator==(const DesignInputs&) const = default;
};

struct DesignRequest {
    uint64_t id = 0;  // 0 = none
    DesignInputs in;
};

struct DesignResult {
    uint64_t id = 0;
    bool ok = false;
    dsp::SvfCascade cascade;
};

// Single-writer sequence lock. Readers copy the value and accept it only if the sequence was
// even and unchanged across the copy.
template <typename T>
class SeqSlot {
public:
    void write(const T& v) noexcept {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);  // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);
        value_ = v;
        seq_.store(s + 2, std::memory_order_release);
    }
    [[nodiscard]] bool tryRead(T& out) const noexcept {
        const uint32_t s1 = seq_.load(std::memory_order_acquire);
        if (s1 & 1u) return false;
        out = value_;
        std::atomic_thread_fence(std::memory_order_acquire);
        return seq_.load(std::memory_order_relaxed) == s1;
    }

private:
    std::atomic<uint32_t> seq_{0};
    T value_{};
};

// The refined design as an SVF cascade. Allocates and takes milliseconds: never call it on a
// real-time audio thread (offline rendering may).
[[nodiscard]] bool designRefinedCascade(const DesignInputs& in, dsp::SvfCascade& out);

class DesignWorker {
public:
    DesignWorker() = default;
    ~DesignWorker() { stop(); }
    DesignWorker(const DesignWorker&) = delete;
    DesignWorker& operator=(const DesignWorker&) = delete;

    // Main thread.
    void start();
    void stop();
    [[nodiscard]] bool running() const noexcept { return thread_.joinable(); }

    // Audio thread.
    void post(int band, const DesignRequest& r) noexcept { requests_[static_cast<size_t>(band)].write(r); }
    // Cheap check before copying a result: the id of the band's latest finished request.
    [[nodiscard]] uint64_t readyId(int band) const noexcept {
        return readyIds_[static_cast<size_t>(band)].load(std::memory_order_acquire);
    }
    [[nodiscard]] bool tryResult(int band, DesignResult& out) const noexcept {
        return results_[static_cast<size_t>(band)].tryRead(out);
    }

    // Runs every outstanding request once; returns how many were designed. The worker thread
    // loops on this; tests call it directly (DesignMode::Manual).
    int runPending();

private:
    std::array<SeqSlot<DesignRequest>, params::kBandCount> requests_{};
    std::array<SeqSlot<DesignResult>, params::kBandCount> results_{};
    std::array<std::atomic<uint64_t>, params::kBandCount> readyIds_{};
    std::array<uint64_t, params::kBandCount> done_{};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace amq::engine
