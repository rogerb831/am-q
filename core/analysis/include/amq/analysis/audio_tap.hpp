#pragma once
// Lock-free audio tap (area 17 R1, R6; area 19 R1): the audio thread copies each block into a
// ring; the analyzer worker reads it. One producer, one consumer.
//
// The producer never waits and never checks the reader: it always writes, overwriting the
// oldest audio. The reader notices when it has fallen more than a ring behind, skips ahead,
// and counts the skipped frames as dropped (area 17 R6). A read that races with an overwrite
// is detected with a seqlock-style check (the producer announces the range it is about to
// write before writing it; Boehm, "Can seqlocks get along with programming language memory
// models?", 2012), and the overwritten part is dropped too. Every read reports whether a gap
// precedes the frames it returns, so the framer can restart and use only full frames (R4).
//
// Stores two channels. A mono block is stored in both, so channel means are unchanged.

#include <atomic>
#include <cstdint>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // cache-line padding is intended
#endif

namespace amq::analysis {

class AudioTap {
public:
    static constexpr uint32_t kChannels = 2;

    // Allocates a ring of at least minFrames (rounded up to a power of two) and resets both
    // sides. Neither side may run during prepare().
    void prepare(uint32_t minFrames);
    [[nodiscard]] uint32_t capacity() const noexcept { return static_cast<uint32_t>(cap_); }

    // ---- Producer (audio thread) ----
    // channels 0 writes nothing. No allocation, no lock, no wait.
    void write(const float* const* x, uint32_t channels, uint32_t frames) noexcept;

    // ---- Consumer (analyzer worker) ----
    struct ReadResult {
        uint32_t frames = 0;       // valid frames written to l and r
        uint32_t channels = 0;     // channel count of the latest producer block (1 or 2)
        bool gap = false;          // frames were dropped before these
        uint64_t endPosition = 0;  // tap position just after the last returned frame
    };
    ReadResult read(float* l, float* r, uint32_t maxFrames) noexcept;
    // Frames available to read now (may exceed capacity when the reader has fallen behind).
    [[nodiscard]] uint64_t available() const noexcept {
        return wpos_.load(std::memory_order_acquire) - rpos_;
    }

    // ---- Any thread ----
    [[nodiscard]] uint64_t written() const noexcept { return wpos_.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t droppedFrames() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    std::vector<float> buf_[kChannels];
    uint64_t cap_ = 0, mask_ = 0;
    alignas(64) std::atomic<uint64_t> claim_{0};  // end of the range being written
    std::atomic<uint64_t> wpos_{0};               // end of the range fully written
    std::atomic<uint32_t> channels_{0};
    alignas(64) uint64_t rpos_ = 0;
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace amq::analysis

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
