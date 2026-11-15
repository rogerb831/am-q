#include "amq/analysis/audio_tap.hpp"

#include <algorithm>
#include <cstring>

namespace amq::analysis {

void AudioTap::prepare(uint32_t minFrames) {
    uint64_t cap = 1024;
    while (cap < minFrames) cap <<= 1;
    for (auto& b : buf_) b.assign(static_cast<size_t>(cap), 0.0f);
    cap_ = cap;
    mask_ = cap - 1;
    claim_.store(0, std::memory_order_relaxed);
    wpos_.store(0, std::memory_order_relaxed);
    channels_.store(0, std::memory_order_relaxed);
    rpos_ = 0;
    dropped_.store(0, std::memory_order_relaxed);
}

void AudioTap::write(const float* const* x, uint32_t channels, uint32_t frames) noexcept {
    if (cap_ == 0 || channels == 0 || frames == 0 || !x || !x[0]) return;
    const uint64_t w = wpos_.load(std::memory_order_relaxed);
    const uint64_t end = w + frames;
    // Announce the range first: a reader that copied any of it will see the claim.
    claim_.store(end, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    const float* src[kChannels] = {x[0], channels > 1 && x[1] ? x[1] : x[0]};
    // A block longer than the ring keeps only its newest cap_ frames.
    const uint64_t first = frames > cap_ ? end - cap_ : w;
    for (uint32_t c = 0; c < kChannels; ++c) {
        float* dst = buf_[c].data();
        uint64_t p = first;
        while (p < end) {
            const uint64_t at = p & mask_;
            const uint64_t n = std::min(end - p, cap_ - at);
            std::memcpy(dst + at, src[c] + (p - w), static_cast<size_t>(n) * sizeof(float));
            p += n;
        }
    }
    channels_.store(std::min(channels, kChannels), std::memory_order_relaxed);
    wpos_.store(end, std::memory_order_release);
}

AudioTap::ReadResult AudioTap::read(float* l, float* r, uint32_t maxFrames) noexcept {
    ReadResult res;
    if (cap_ == 0) return res;
    const uint64_t w = wpos_.load(std::memory_order_acquire);
    res.channels = channels_.load(std::memory_order_relaxed);
    if (w - rpos_ > cap_) {
        // Fallen a ring behind: skip to an eighth of a ring behind the writer, so the copy
        // below is unlikely to race with the next block.
        const uint64_t to = w - cap_ + cap_ / 8;
        dropped_.fetch_add(to - rpos_, std::memory_order_relaxed);
        rpos_ = to;
        res.gap = true;
    }
    const auto n = static_cast<uint32_t>(std::min<uint64_t>(w - rpos_, maxFrames));
    float* dst[kChannels] = {l, r};
    for (uint32_t c = 0; c < kChannels; ++c) {
        uint64_t p = rpos_;
        const uint64_t end = rpos_ + n;
        while (p < end) {
            const uint64_t at = p & mask_;
            const uint64_t k = std::min(end - p, cap_ - at);
            std::memcpy(dst[c] + (p - rpos_), buf_[c].data() + at, static_cast<size_t>(k) * sizeof(float));
            p += k;
        }
    }
    // Any frame the producer may have started overwriting during the copy is invalid.
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t claim = claim_.load(std::memory_order_relaxed);
    uint32_t bad = 0;
    if (claim > rpos_ + cap_) bad = static_cast<uint32_t>(std::min<uint64_t>(n, claim - cap_ - rpos_));
    if (bad > 0) {
        dropped_.fetch_add(bad, std::memory_order_relaxed);
        res.gap = true;
        for (uint32_t c = 0; c < kChannels; ++c) std::memmove(dst[c], dst[c] + bad, static_cast<size_t>(n - bad) * sizeof(float));
    }
    rpos_ += n;
    res.frames = n - bad;
    res.endPosition = rpos_;
    return res;
}

}  // namespace amq::analysis
