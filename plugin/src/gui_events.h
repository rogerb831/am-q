#pragma once
// GUI -> host event queue (area 09 R4): single producer (main thread, the editor's gestures),
// single consumer (process() or params.flush(), which turn the events into CLAP gesture and
// value events in order). The table is written on the main thread through set(id, plain,
// GUI); this queue only tells the host. No locks, no allocation after construction.

#include <clap/clap.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace amq::plugin {

struct GuiEvent {
    enum Kind : uint8_t { Begin, Value, End } kind = Value;
    clap_id id = 0;
    double value = 0.0;  // host-domain value for Value events
};

class GuiEventQueue {
public:
    bool push(const GuiEvent& e) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t next = (h + 1) % kSize;
        if (next == tail_.load(std::memory_order_acquire)) return false;  // full: the event is dropped
        buf_[h] = e;
        head_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(GuiEvent& e) noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        e = buf_[t];
        tail_.store((t + 1) % kSize, std::memory_order_release);
        return true;
    }

private:
    static constexpr size_t kSize = 8192;
    std::array<GuiEvent, kSize> buf_{};
    std::atomic<size_t> head_{0}, tail_{0};
};

}  // namespace amq::plugin
