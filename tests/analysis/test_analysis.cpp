// Snapshot feed tests (area 11 R3/R4/R6, area 12 R9-R11, area 14 R13/R14).
#include "amq/analysis/publisher.hpp"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

// Allocation counter for the publishing path (area 14 R14).
namespace {
thread_local bool gCountAllocs = false;
std::atomic<long> gAllocs{0};
}  // namespace
void* operator new(std::size_t n) {
    if (gCountAllocs) ++gAllocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace amq::analysis;

namespace {

constexpr double kFs = 48000.0;
constexpr uint32_t kBlock = 64;

// Feed n blocks of one channel through the publisher; impulseAt: sample index of a single
// sample of amplitude amp (or -1).
void feed(SnapshotPublisher& p, uint64_t& pos, uint32_t blocks, int64_t impulseAt = -1, float amp = 0.5f) {
    static std::array<float, kBlock> x{};  // no allocation inside the counted region
    for (uint32_t b = 0; b < blocks; ++b) {
        x.fill(0.0f);
        if (impulseAt >= static_cast<int64_t>(pos) && impulseAt < static_cast<int64_t>(pos + kBlock))
            x[static_cast<size_t>(impulseAt - static_cast<int64_t>(pos))] = amp;
        const float* ch[1] = {x.data()};
        p.meterInput(ch, 1, kBlock);
        p.meterOutput(ch, 1, kBlock);
        pos += kBlock;
        p.advance(kBlock, pos);
    }
}

}  // namespace

TEST_CASE("snapshots start at the silence floor and nothing is pulled before a publish") {
    LevelSnapshot l;
    CHECK(l.header.stamp == 0);
    CHECK(l.output[0].peakDb == kFloorDb);
    CHECK(l.output[1].rmsDb == kFloorDb);
    SpectrumSnapshot s;
    CHECK(s.pre[0] == kFloorDb);
    CHECK(s.external[kMaxSpectrumBins - 1] == kFloorDb);
    SnapshotPublisher p;
    p.prepare(kFs, 2);
    CHECK(p.pullLevels() == nullptr);
    CHECK(p.pullGainChange() == nullptr);
    CHECK(p.pullSpectrum() == nullptr);
    CHECK(toDb(0.0) == kFloorDb);
    CHECK(toDb(1e-40) == kFloorDb);
    CHECK(toDb(std::nan("")) == kFloorDb);
}

TEST_CASE("triple buffer: concurrent producer and consumer, no torn or repeated frames") {
    struct Frame {
        uint64_t stamp = 0;
        std::array<uint64_t, 31> payload{};
    };
    TripleBuffer<Frame> tb;
    constexpr uint64_t kFrames = 2'000'000;
    std::atomic<bool> done{false};
    uint64_t torn = 0, backwards = 0, taken = 0, last = 0;
    std::thread consumer([&] {
        while (true) {
            const bool finished = done.load(std::memory_order_acquire);
            if (tb.acquire()) {
                const Frame& f = tb.front();
                for (auto v : f.payload) torn += v != f.stamp;
                backwards += f.stamp <= last;
                last = f.stamp;
                ++taken;
            } else if (finished) {
                break;
            }
        }
    });
    for (uint64_t i = 1; i <= kFrames; ++i) {
        Frame& f = tb.back();
        f.stamp = i;
        f.payload.fill(i);
        tb.publish();
    }
    done.store(true, std::memory_order_release);
    consumer.join();
    MESSAGE("frames taken by the consumer: " << taken << " of " << kFrames);
    CHECK(torn == 0);
    CHECK(backwards == 0);
    CHECK(last == kFrames);  // the newest frame is always delivered
    CHECK(taken > 0);
}

TEST_CASE("publish rate, stamps and drop when nobody reads (area 14 R14)") {
    SnapshotPublisher p;
    p.prepare(kFs, 1);
    uint64_t pos = 0;
    gAllocs = 0;
    gCountAllocs = true;
    feed(p, pos, static_cast<uint32_t>(10 * kFs / kBlock));  // 10 s, editor closed
    gCountAllocs = false;
    CHECK(gAllocs.load() == 0);
    const double perSecond = static_cast<double>(p.publishedCount()) / 10.0;
    MESSAGE("publishes per second: " << perSecond);
    CHECK(perSecond == doctest::Approx(kPublishHz).epsilon(0.1));
    const LevelSnapshot* l = p.pullLevels();
    REQUIRE(l != nullptr);
    CHECK(l->header.stamp == p.publishedCount());  // the newest one
    CHECK(l->header.samplePosition <= pos);
    CHECK(p.pullLevels() == nullptr);  // nothing new until the next publish
}

TEST_CASE("a transient in an unread frame is merged into the next one (area 11 R4)") {
    SnapshotPublisher p;
    p.prepare(kFs, 1);
    uint64_t pos = 0;
    feed(p, pos, 100);
    REQUIRE(p.pullLevels() != nullptr);
    // An impulse, then several more publishes without a pull: the frame that held it is dropped.
    const int64_t at = static_cast<int64_t>(pos) + 10;
    feed(p, pos, 13, at);   // publishes the impulse frame
    feed(p, pos, 13 * 4);   // four more frames, unread
    const LevelSnapshot* l = p.pullLevels();
    REQUIRE(l != nullptr);
    CHECK(l->output[0].peakDb == doctest::Approx(20.0 * std::log10(0.5)).epsilon(1e-4));
    CHECK(l->input[0].peakDb == doctest::Approx(20.0 * std::log10(0.5)).epsilon(1e-4));
    // Once read, the peak is not repeated in later frames.
    feed(p, pos, 13);
    l = p.pullLevels();
    REQUIRE(l != nullptr);
    CHECK(l->output[0].peakDb == kFloorDb);
}

TEST_CASE("RMS and peak hold") {
    SnapshotPublisher p;
    p.prepare(kFs, 2);
    std::vector<float> a(kBlock), b(kBlock);
    uint64_t pos = 0;
    for (int blk = 0; blk < static_cast<int>(2 * kFs / kBlock); ++blk) {  // 2 s: full-scale sine and silence
        for (uint32_t i = 0; i < kBlock; ++i) a[i] = static_cast<float>(std::sin(2.0 * 3.141592653589793 * 1000.0 * (pos + i) / kFs));
        const float* ch[2] = {a.data(), b.data()};
        p.meterOutput(ch, 2, kBlock);
        pos += kBlock;
        p.advance(kBlock, pos);
    }
    const LevelSnapshot* l = p.pullLevels();
    REQUIRE(l != nullptr);
    CHECK(l->channels == 2);
    CHECK(l->output[0].rmsDb == doctest::Approx(-3.0103).epsilon(0.01));
    CHECK(l->output[0].peakDb == doctest::Approx(0.0).epsilon(0.01));
    CHECK(l->output[1].peakDb == kFloorDb);  // silent channel stays at the floor
    CHECK(l->output[1].rmsDb == kFloorDb);

    // Peak hold: an impulse holds for kHoldMs, then the hold falls to the current peak.
    SnapshotPublisher h;
    h.prepare(kFs, 1);
    uint64_t q = 0;
    feed(h, q, 10, 5, 0.5f);
    feed(h, q, static_cast<uint32_t>(0.5 * kHoldMs * 1e-3 * kFs / kBlock));
    const LevelSnapshot* m = h.pullLevels();
    REQUIRE(m != nullptr);
    CHECK(m->output[0].holdDb == doctest::Approx(20.0 * std::log10(0.5)).epsilon(1e-4));
    feed(h, q, static_cast<uint32_t>(kHoldMs * 1e-3 * kFs / kBlock));
    m = h.pullLevels();
    REQUIRE(m != nullptr);
    CHECK(m->output[0].holdDb == kFloorDb);
}

TEST_CASE("gain change: current value and largest-magnitude peak, merged when unread") {
    SnapshotPublisher p;
    p.prepare(kFs, 1);
    uint64_t pos = 0;
    p.setGainChange(3, -6.0f);
    feed(p, pos, 13);
    p.setGainChange(3, -1.0f);
    feed(p, pos, 13 * 3);  // unread frames
    const GainChangeSnapshot* g = p.pullGainChange();
    REQUIRE(g != nullptr);
    CHECK(g->currentDb[3] == -1.0f);
    CHECK(g->peakDb[3] == -6.0f);
    CHECK(g->currentDb[0] == 0.0f);
    feed(p, pos, 13);
    g = p.pullGainChange();
    REQUIRE(g != nullptr);
    CHECK(g->peakDb[3] == -1.0f);  // the old peak was read, so it is not repeated
}
