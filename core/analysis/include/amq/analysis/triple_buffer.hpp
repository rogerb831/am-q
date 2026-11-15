#pragma once
// Single-producer, single-consumer triple buffer (areas 11 R3, 12 R9/R10, 14 R14).
//
// The producer (audio thread) writes back(), then publish() swaps it with the shared middle
// slot. The consumer (GUI thread) calls acquire(), which swaps the middle slot into front()
// only when a new frame was published. Neither side ever waits, locks or allocates. When the
// consumer is slow or absent, each publish replaces the unread middle frame (drop when full),
// so memory use is fixed. A textbook construction, built from public descriptions.

#include <array>
#include <atomic>
#include <cstdint>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // cache-line padding is intended
#endif

namespace amq::analysis {

template <typename T>
class TripleBuffer {
public:
    TripleBuffer() = default;
    TripleBuffer(const TripleBuffer&) = delete;
    TripleBuffer& operator=(const TripleBuffer&) = delete;

    // ---- Producer (one thread) ----
    [[nodiscard]] T& back() noexcept { return slots_[back_].value; }

    // True while the last published frame has not been acquired yet. Producer-side peek: the
    // consumer may acquire it right after, which only makes a merge redundant, never lossy.
    [[nodiscard]] bool unread() const noexcept { return (state_.load(std::memory_order_acquire) & kFresh) != 0; }

    // Publish back(). Returns true if the frame it replaced had been acquired (or none existed).
    bool publish() noexcept {
        const uint8_t prev = state_.exchange(static_cast<uint8_t>(back_ | kFresh), std::memory_order_acq_rel);
        back_ = static_cast<uint8_t>(prev & kIndex);
        return (prev & kFresh) == 0;
    }

    // ---- Consumer (one thread) ----
    // Takes the newest published frame into front(). False if nothing new since the last call.
    bool acquire() noexcept {
        if ((state_.load(std::memory_order_acquire) & kFresh) == 0) return false;
        const uint8_t prev = state_.exchange(front_, std::memory_order_acq_rel);
        front_ = static_cast<uint8_t>(prev & kIndex);
        return true;
    }
    [[nodiscard]] const T& front() const noexcept { return slots_[front_].value; }

    // Not thread-safe: only while neither side is running (prepare, tests).
    void resetAll(const T& value) {
        for (auto& s : slots_) s.value = value;
        back_ = 0;
        front_ = 2;
        state_.store(1, std::memory_order_release);
    }

private:
    static constexpr uint8_t kIndex = 0x3;
    static constexpr uint8_t kFresh = 0x4;
    struct alignas(64) Slot {
        T value{};
    };
    std::array<Slot, 3> slots_{};
    alignas(64) std::atomic<uint8_t> state_{1};  // middle index | fresh bit
    alignas(64) uint8_t back_ = 0;               // producer only
    alignas(64) uint8_t front_ = 2;              // consumer only
};

}  // namespace amq::analysis

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
