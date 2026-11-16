// Analyzer engine core (area 19 R1-R4, area 17 R1, R4-R6, R13; area 12 R10).
#include "amq/analysis/analyzer.hpp"
#include "amq/analysis/audio_tap.hpp"
#include "amq/analysis/display_spectrum.hpp"
#include "amq/dsp/log_grid.hpp"
#include "amq/engine/engine.hpp"
#include "amq/params/parameter_table.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace amqtest {
extern thread_local bool gCountAllocs;
extern std::atomic<long> gAllocs;
}  // namespace amqtest

using namespace amq::analysis;
using amqtest::gAllocs;
using amqtest::gCountAllocs;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Records every frame of one source: count, last mean power, running mean of the PSD.
struct Recorder final : FrameConsumer {
    int frames = 0, passes = 0;
    int fftSize = 0;
    std::vector<double> lastAmplitudeDb;  // mean power * amplitude scale, in dB
    std::vector<double> psdSum;
    std::vector<uint64_t> ends;
    void onFrame(const SpectrumFrame& f) noexcept override {
        if (lastAmplitudeDb.size() != static_cast<size_t>(f.bins)) {
            lastAmplitudeDb.assign(static_cast<size_t>(f.bins), 0.0);
            psdSum.assign(static_cast<size_t>(f.bins), 0.0);
        }
        fftSize = f.fftSize;
        for (int k = 0; k < f.bins; ++k) {
            const double p = f.meanPower[k];
            lastAmplitudeDb[static_cast<size_t>(k)] = 10.0 * std::log10(std::max(p * f.scale->amplitude, 1e-30));
            psdSum[static_cast<size_t>(k)] += p * f.scale->psd;
        }
        if (ends.size() < 64) ends.push_back(f.endPosition);  // reserved below; no allocation after
        ++frames;
    }
    void onPassEnd() noexcept override { ++passes; }
};

void writeBlocks(Analyzer& a, Source s, const std::vector<float>& l, const std::vector<float>& r, uint32_t block) {
    for (size_t pos = 0; pos < l.size(); pos += block) {
        const auto n = static_cast<uint32_t>(std::min<size_t>(block, l.size() - pos));
        const float* x[2] = {l.data() + pos, r.data() + pos};
        a.write(s, x, 2, n);
    }
}

std::vector<float> sine(size_t n, double hz, double fs, double amp) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<float>(amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) / fs));
    return v;
}

}  // namespace

TEST_CASE("FFT size follows the resolution and scales with the sample rate (area 19 R2)") {
    for (int r = 0; r < kResolutionCount; ++r) {
        CHECK(analyzerFftSize(r, 48000.0) == (2048 << r));
        CHECK(analyzerFftSize(r, 44100.0) == (2048 << r));
        CHECK(analyzerFftSize(r, 96000.0) == (4096 << r));
        CHECK(analyzerFftSize(r, 192000.0) == (8192 << r));
        CHECK(analyzerFftSize(r, 384000.0) == (16384 << r));
        CHECK(amq::dsp::RealFft::validSize(analyzerFftSize(r, 384000.0)));
    }
    // Bin width stays within a factor of two of 48 kHz / 4096 at every rate.
    for (double fs : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 352800.0, 384000.0}) {
        const double w = fs / analyzerFftSize(kDefaultResolution, fs);
        CHECK(w > 10.0);
        CHECK(w < 12.0);
    }
}

TEST_CASE("Audio tap: round trip across the wrap, mono stored in both channels") {
    AudioTap t;
    t.prepare(1000);
    CHECK(t.capacity() == 1024);
    std::vector<float> l(700), r(700), ol(1024), orr(1024);
    for (size_t i = 0; i < l.size(); ++i) l[i] = static_cast<float>(i), r[i] = -static_cast<float>(i);
    const float* x[2] = {l.data(), r.data()};
    t.write(x, 2, 700);
    auto res = t.read(ol.data(), orr.data(), 500);
    CHECK(res.frames == 500);
    CHECK_FALSE(res.gap);
    t.write(x, 1, 700);  // mono: wraps the ring
    res = t.read(ol.data(), orr.data(), 1024);
    REQUIRE(res.frames == 900);
    CHECK(res.channels == 1);
    CHECK(ol[0] == 500.0f);
    CHECK(orr[0] == -500.0f);
    CHECK(ol[200] == 0.0f);   // first mono frame
    CHECK(orr[899] == 699.0f);  // mono copied into channel 2
    CHECK(res.endPosition == 1400);
    CHECK(t.droppedFrames() == 0);
}

TEST_CASE("Audio tap: a reader more than a ring behind skips ahead and counts the drop (area 17 R6)") {
    AudioTap t;
    t.prepare(1024);
    std::vector<float> x0(5000);
    for (size_t i = 0; i < x0.size(); ++i) x0[i] = static_cast<float>(i);
    const float* x[2] = {x0.data(), x0.data()};
    gAllocs = 0;
    gCountAllocs = true;
    for (int b = 0; b < 5; ++b) t.write(x, 1, 1000);  // writer never blocks
    gCountAllocs = false;
    CHECK(gAllocs.load() == 0);
    std::vector<float> l(2048), r(2048);
    auto res = t.read(l.data(), r.data(), 2048);
    CHECK(res.gap);
    CHECK(t.droppedFrames() == 5000 - 1024 + 128);
    REQUIRE(res.frames == 1024 - 128);
    // The surviving frames are the newest ones, intact.
    const uint64_t first = res.endPosition - res.frames;
    for (uint32_t i = 0; i < res.frames; ++i) CHECK(l[i] == static_cast<float>((first + i) % 1000));
    res = t.read(l.data(), r.data(), 2048);
    CHECK(res.frames == 0);
    CHECK_FALSE(res.gap);
}

TEST_CASE("Audio tap under a racing producer never returns torn or misplaced frames") {
    // The producer writes the sample index (exact in float below 2^24) as fast as it can; the
    // consumer is deliberately slow, so overwrites race with its copies.
    AudioTap t;
    t.prepare(4096);
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        std::vector<float> blk(97);
        uint64_t pos = 0;
        while (!stop.load() && pos < (1u << 23)) {
            for (size_t i = 0; i < blk.size(); ++i) blk[i] = static_cast<float>(pos + i);
            const float* x[2] = {blk.data(), blk.data()};
            t.write(x, 1, static_cast<uint32_t>(blk.size()));
            pos += blk.size();
        }
    });
    std::vector<float> l(3000), r(3000);
    uint64_t good = 0, bad = 0;
    bool sawGap = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (int it = 0; (good < 2000000 || !sawGap) && std::chrono::steady_clock::now() < deadline; ++it) {
        const auto res = t.read(l.data(), r.data(), 3000);
        sawGap |= res.gap;
        const uint64_t first = res.endPosition - res.frames;
        for (uint32_t i = 0; i < res.frames; ++i) {
            if (l[i] == static_cast<float>(first + i) && r[i] == l[i]) ++good;
            else ++bad;
        }
        if (it % 8 == 0) std::this_thread::yield();
    }
    stop = true;
    producer.join();
    MESSAGE(good << " frames read intact, " << t.droppedFrames() << " dropped");
    CHECK(bad == 0);
    CHECK(good > 0);
    CHECK(sawGap);
}

TEST_CASE("Framer: only full frames, hop N/4, and a restart after a gap (area 17 R4, area 19 R4)") {
    Analyzer a;
    Recorder rec;
    rec.ends.reserve(64);
    a.subscribe(&rec, sourceBit(Source::Pre), SizeChoice::Display);
    REQUIRE(a.prepare(48000.0));
    const int n = a.fftSize(SizeChoice::Display);
    REQUIRE(n == 4096);
    std::vector<float> z(static_cast<size_t>(n), 0.1f);
    const float* x[2] = {z.data(), z.data()};
    a.write(Source::Pre, x, 2, static_cast<uint32_t>(n - 1));
    a.runPending();
    CHECK(rec.frames == 0);  // a partial frame never counts
    a.write(Source::Pre, x, 2, 1);
    a.runPending();
    CHECK(rec.frames == 1);
    CHECK(rec.ends[0] == static_cast<uint64_t>(n));
    a.write(Source::Pre, x, 2, static_cast<uint32_t>(n / 4 * 3));
    a.runPending();
    CHECK(rec.frames == 4);
    CHECK(rec.ends[3] == static_cast<uint64_t>(n + 3 * n / 4));
    CHECK(a.frameCount(Source::Pre) == 4);
    CHECK(a.frameCount(Source::Post) == 0);  // nothing subscribed, nothing written
    // Overflow the ring (0.5 s): the framer restarts and waits for a full frame again.
    std::vector<float> big(48000, 0.1f);
    const float* xb[2] = {big.data(), big.data()};
    const int before = rec.frames;
    a.write(Source::Pre, xb, 2, 48000);
    a.runPending();
    CHECK(a.droppedFrames(Source::Pre) > 0);
    const uint64_t kept = 48000 - a.droppedFrames(Source::Pre);
    CHECK(rec.frames - before == static_cast<int>((kept - static_cast<uint64_t>(n)) / static_cast<uint64_t>(n / 4) + 1));
}

TEST_CASE("Sine calibration: a -20 dBFS sine reads -20 dBFS at every resolution (area 19)") {
    for (int res = 0; res < kResolutionCount; ++res) {
        for (auto w : {AnalyzerWindow::BlackmanHarris, AnalyzerWindow::Kaiser}) {
            Analyzer a;
            Recorder rec;
            a.subscribe(&rec, sourceBit(Source::Pre), SizeChoice::Display);
            REQUIRE(a.prepare(48000.0, {res, w}));
            const int n = a.fftSize(SizeChoice::Display);
            const int bin = static_cast<int>(std::lround(1000.0 * n / 48000.0));
            const auto [centre, worst] = [&] {
                double readCentre = 0.0, scallop = 0.0;
                for (double offset : {0.0, 0.5}) {
                    const auto x = sine(static_cast<size_t>(n) * 2, (bin + offset) * 48000.0 / n, 48000.0, 0.1);
                    writeBlocks(a, Source::Pre, x, x, 512);
                    a.runPending();
                    const double peak = *std::max_element(rec.lastAmplitudeDb.begin(), rec.lastAmplitudeDb.end());
                    if (offset == 0.0) readCentre = peak;
                    else scallop = peak;
                }
                return std::pair{readCentre, scallop};
            }();
            const std::string name = w == AnalyzerWindow::Kaiser ? "Kaiser b=12" : "Blackman-Harris";
            INFO("resolution " << res << " (" << n << "), window " << name);
            CHECK(centre == doctest::Approx(-20.0).epsilon(0.01));  // within 0.2 dB
            if (res == kDefaultResolution)
                MESSAGE(name << ": half-bin scalloping "
                        << (-20.0 - worst) << " dB");
        }
    }
}

TEST_CASE("Noise calibration: averaged PSD matches the analytical white-noise PSD (area 19)") {
    Analyzer a;
    Recorder rec;
    a.subscribe(&rec, sourceBit(Source::Post), SizeChoice::Display);
    const double fs = 48000.0;
    REQUIRE(a.prepare(fs));
    std::mt19937 rng(3);
    std::normal_distribution<float> g(0.0f, 0.1f);
    std::vector<float> l(4800), r(4800);
    for (int s = 0; s < 600; ++s) {  // 60 s, read as we go so the ring never overflows
        for (size_t i = 0; i < l.size(); ++i) l[i] = g(rng), r[i] = g(rng);
        writeBlocks(a, Source::Post, l, r, 480);
        a.runPending();
    }
    REQUIRE(a.droppedFrames(Source::Post) == 0);
    const double expected = 2.0 * 0.01 / fs;  // one-sided PSD of variance 0.01
    double sq = 0.0;
    int count = 0;
    for (int k = 0; k < static_cast<int>(rec.psdSum.size()); ++k) {
        const double hz = k * fs / rec.fftSize;
        if (hz < 20.0 || hz > 20000.0) continue;
        const double db = 10.0 * std::log10(rec.psdSum[static_cast<size_t>(k)] / rec.frames / expected);
        sq += db * db;
        ++count;
    }
    const double rms = std::sqrt(sq / count);
    MESSAGE(rec.frames << " frames, PSD error " << rms << " dB RMS over 20 Hz - 20 kHz");
    CHECK(rms < 0.2);
}

TEST_CASE("Sidelobes: a tone 60 dB down, 10 bins away, reads its level within 1 dB (area 19)") {
    for (auto w : {AnalyzerWindow::BlackmanHarris, AnalyzerWindow::Kaiser}) {
        Analyzer a;
        Recorder rec;
        a.subscribe(&rec, sourceBit(Source::Pre), SizeChoice::Display);
        REQUIRE(a.prepare(48000.0, {kDefaultResolution, w}));
        const int n = a.fftSize(SizeChoice::Display);
        const int k1 = 200, k2 = 210;
        const auto s1 = sine(static_cast<size_t>(n) * 2, k1 * 48000.0 / n, 48000.0, 0.5);
        const auto s2 = sine(static_cast<size_t>(n) * 2, k2 * 48000.0 / n, 48000.0, 0.0005);
        std::vector<float> x(s1.size());
        for (size_t i = 0; i < x.size(); ++i) x[i] = s1[i] + s2[i];
        writeBlocks(a, Source::Pre, x, x, 512);
        a.runPending();
        CHECK(rec.lastAmplitudeDb[k1] == doctest::Approx(20.0 * std::log10(0.5)).epsilon(0.01));
        CHECK(std::abs(rec.lastAmplitudeDb[k2] - 20.0 * std::log10(0.0005)) < 1.0);
    }
}

TEST_CASE("Channel merge by mean power: hard left, constant-power centre and hard right agree (area 17 R5)") {
    std::mt19937 rng(9);
    std::normal_distribution<float> g(0.0f, 0.2f);
    std::vector<float> x(48000), zero(48000, 0.0f), c(48000);
    for (auto& v : x) v = g(rng);
    for (size_t i = 0; i < x.size(); ++i) c[i] = x[i] / std::sqrt(2.0f);
    std::vector<double> result[3];
    const std::vector<float>* lr[3][2] = {{&x, &zero}, {&c, &c}, {&zero, &x}};
    for (int p = 0; p < 3; ++p) {
        Analyzer a;
        Recorder rec;
        a.subscribe(&rec, sourceBit(Source::Pre), SizeChoice::Display);
        REQUIRE(a.prepare(48000.0));
        writeBlocks(a, Source::Pre, *lr[p][0], *lr[p][1], 512);
        a.runPending();
        result[p] = rec.psdSum;
    }
    double worst = 0.0;
    for (size_t k = 2; k < result[0].size(); ++k)
        for (int p = 1; p < 3; ++p) worst = std::max(worst, std::abs(10.0 * std::log10(result[p][k] / result[0][k])));
    MESSAGE("worst merge difference " << worst << " dB");
    CHECK(worst < 0.1);
}

TEST_CASE("Display spectrum: grid mapping, floor before data, snapshot published per pass (area 12 R10)") {
    SnapshotPublisher pub;
    Analyzer a;
    DisplaySpectrum d;
    a.subscribe(&d, kAllSources, SizeChoice::Display);
    REQUIRE(a.prepare(48000.0));
    d.prepare(&pub, 48000.0, a.fftSize(SizeChoice::Display));
    a.runPending();
    CHECK(pub.pullSpectrum() == nullptr);  // nothing analysed yet: nothing published
    const int n = a.fftSize(SizeChoice::Display);
    const int bin = 341;  // ~4 kHz
    const double hz = bin * 48000.0 / n;
    const auto x = sine(static_cast<size_t>(n) * 2, hz, 48000.0, 0.1);
    gAllocs = 0;
    gCountAllocs = true;
    writeBlocks(a, Source::Post, x, x, 256);
    a.runPending();
    gCountAllocs = false;
    CHECK(gAllocs.load() == 0);  // the worker pass does not allocate either
    const SpectrumSnapshot* s = pub.pullSpectrum();
    REQUIRE(s != nullptr);
    CHECK(s->binCount == static_cast<uint32_t>(amq::dsp::kLogGridPoints));
    CHECK(s->header.stamp == pub.spectrumCount());
    const auto& grid = amq::dsp::logGrid();
    size_t at = 0;
    for (size_t i = 0; i < grid.size(); ++i)
        if (s->post[i] > s->post[at]) at = i;
    CHECK(s->post[at] == doctest::Approx(-20.0f).epsilon(0.01));
    CHECK(std::abs(std::log2(grid[at] / hz)) < 1.0 / 48.0);  // the peak lands on the sine's grid point
    CHECK(s->pre[at] == kFloorDb);       // pre had no audio
    CHECK(s->external[at] == kFloorDb);  // nor the side chain
    a.runPending();
    CHECK(pub.pullSpectrum() == nullptr);  // no new frame, no new snapshot
}

TEST_CASE("Engine taps pre, post and side chain; the analyzer follows prepare and release (area 19 R1)") {
    amq::params::ParameterTable t;
    t.set(amq::params::globalParam(amq::params::GlobalParam::AnalyzerTilt), 0.0, amq::params::ParamSource::Preset);  // calibrated reading
    amq::engine::Engine e(t);
    REQUIRE(e.prepare(48000.0, 512));
    const int n = e.analyzer().fftSize(SizeChoice::Display);
    const auto x = sine(static_cast<size_t>(n) * 2, 100 * 48000.0 / n, 48000.0, 0.1);
    std::vector<float> ol(512), orr(512);
    const auto scs = sine(512, 296 * 48000.0 / n, 48000.0, 0.01);  // whole cycles in 512 samples
    for (size_t pos = 0; pos + 512 <= x.size(); pos += 512) {
        const float* in[2] = {x.data() + pos, x.data() + pos};
        float* out[2] = {ol.data(), orr.data()};
        const float* sc[2] = {scs.data(), scs.data()};
        e.process(in, 2, out, 2, 512, sc, 2);
    }
    CHECK_FALSE(e.analyzer().running());  // Manual design mode: the tests drive it
    e.runAnalyzerForTests();
    const SpectrumSnapshot* s = e.snapshots().pullSpectrum();
    REQUIRE(s != nullptr);
    const float prePeak = *std::max_element(s->pre.begin(), s->pre.begin() + s->binCount);
    const float postPeak = *std::max_element(s->post.begin(), s->post.begin() + s->binCount);
    const float extPeak = *std::max_element(s->external.begin(), s->external.begin() + s->binCount);
    CHECK(prePeak == doctest::Approx(-20.0f).epsilon(0.01));
    CHECK(postPeak == doctest::Approx(-20.0f).epsilon(0.01));  // flat EQ
    CHECK(extPeak == doctest::Approx(-40.0f).epsilon(0.01));

    // Thread mode: the worker runs from prepare to release, with no editor involved.
    amq::engine::Engine live(t);
    live.setDesignMode(amq::engine::DesignMode::Thread);
    REQUIRE(live.prepare(48000.0, 512));
    CHECK(live.analyzer().running());
    const uint64_t before = live.snapshots().spectrumCount();
    for (size_t pos = 0; pos + 512 <= x.size(); pos += 512) {
        const float* in[2] = {x.data() + pos, x.data() + pos};
        float* out[2] = {ol.data(), orr.data()};
        live.process(in, 2, out, 2, 512);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (int i = 0; i < 200 && live.snapshots().spectrumCount() == before; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(live.snapshots().spectrumCount() > before);
    live.release();
    CHECK_FALSE(live.analyzer().running());
}

TEST_CASE("Analyzer CPU at the highest resolution, display and match consumers, 48 kHz stereo (report)") {
    Analyzer a;
    Recorder display, match;
    a.subscribe(&display, kAllSources, SizeChoice::Display);
    a.subscribe(&match, sourceBit(Source::Pre) | sourceBit(Source::External), SizeChoice::Highest);
    REQUIRE(a.prepare(48000.0, {kResolutionCount - 1, AnalyzerWindow::BlackmanHarris}));
    std::mt19937 rng(4);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    std::vector<float> l(4800), r(4800);
    for (size_t i = 0; i < l.size(); ++i) l[i] = u(rng), r[i] = u(rng);
    double busy = 0.0;
    for (int s = 0; s < 100; ++s) {  // 10 s of audio
        for (int src = 0; src < kSourceCount; ++src) writeBlocks(a, static_cast<Source>(src), l, r, 480);
        const auto t0 = std::chrono::steady_clock::now();
        a.runPending();
        busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    MESSAGE("analyzer worker, 3 sources stereo at 16384 points: " << busy / 10.0 * 100.0 << " % of one core");
    CHECK(busy / 10.0 < 0.03);  // area 19 R10 planning budget
}

TEST_CASE("Display follower: tilt, rise and fall, freeze and peak hold (area 19 display follower)") {
    using amq::params::GlobalParam;
    amq::params::ParameterTable t;
    t.set(amq::params::globalParam(GlobalParam::AnalyzerTilt), 4.5, amq::params::ParamSource::Preset);
    t.set(amq::params::globalParam(GlobalParam::AnalyzerSpeed), 4, amq::params::ParamSource::Preset);  // Very Fast
    amq::engine::Engine e(t);
    REQUIRE(e.prepare(48000.0, 512));
    const int n = e.analyzer().fftSize(SizeChoice::Display);
    const double hz = 100 * 48000.0 / n;
    auto run = [&](float amp, size_t len) {
        const auto x = sine(len, hz, 48000.0, amp);
        std::vector<float> ol(512), orr(512);
        for (size_t pos = 0; pos + 512 <= x.size(); pos += 512) {
            const float* in[2] = {x.data() + pos, x.data() + pos};
            float* out[2] = {ol.data(), orr.data()};
            e.process(in, 2, out, 2, 512);
            e.runAnalyzerForTests();
        }
    };
    auto peakOf = [&](const SpectrumSnapshot* s) { return *std::max_element(s->post.begin(), s->post.begin() + s->binCount); };
    run(0.1f, static_cast<size_t>(n) * 4);
    const SpectrumSnapshot* s = e.snapshots().pullSpectrum();
    REQUIRE(s != nullptr);
    // Tilt: 4.5 dB per octave about 1 kHz.
    CHECK(peakOf(s) == doctest::Approx(-20.0 + 4.5 * std::log2(hz / 1000.0)).epsilon(0.01));
    const float settled = peakOf(s);
    // Fall: a 20 dB drop is followed, not jumped to; the peak hold stays above the fresh value.
    run(0.01f, 2048);
    s = e.snapshots().pullSpectrum();
    REQUIRE(s != nullptr);
    CHECK(peakOf(s) < settled);
    CHECK(peakOf(s) > settled - 20.0f);
    const float held = *std::max_element(s->postPeak.begin(), s->postPeak.begin() + s->binCount);
    CHECK(held >= peakOf(s));
    // Freeze: nothing moves while it is on.
    t.set(amq::params::globalParam(GlobalParam::AnalyzerFreeze), 1, amq::params::ParamSource::Host);
    const float frozen = peakOf(s);
    run(0.1f, static_cast<size_t>(n) * 2);
    const SpectrumSnapshot* f = e.snapshots().pullSpectrum();
    if (f) CHECK(peakOf(f) == frozen);
}
