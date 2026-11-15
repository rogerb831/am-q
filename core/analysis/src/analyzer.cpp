#include "amq/analysis/analyzer.hpp"
#include "amq/dsp/latency.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE__)
#include <xmmintrin.h>
#define AMQ_ANALYZER_FTZ 1
#endif

namespace amq::analysis {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kChunk = 4096;     // frames per tap read
constexpr int kPollMs = 5;            // worker sleep between passes

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 300; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

// Periodic windows (DFT-even), n = 0 .. N-1.
void makeWindow(AnalyzerWindow type, int n, std::vector<float>& w, WindowScale& s, double fs) {
    w.resize(static_cast<size_t>(n));
    const double i0b = besselI0(kAnalyzerKaiserBeta);
    double sum = 0.0, sq = 0.0;
    for (int i = 0; i < n; ++i) {
        const double x = 2.0 * kPi * i / n;
        double v;
        if (type == AnalyzerWindow::BlackmanHarris) {  // 4-term, -92 dB sidelobes (Harris 1978)
            v = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2.0 * x) - 0.01168 * std::cos(3.0 * x);
        } else {
            const double t = (i - n / 2.0) / (n / 2.0);
            v = besselI0(kAnalyzerKaiserBeta * std::sqrt(std::max(0.0, 1.0 - t * t))) / i0b;
        }
        w[static_cast<size_t>(i)] = static_cast<float>(v);
        sum += v;
        sq += v * v;
    }
    s.sum = sum;
    s.sumSquares = sq;
    s.amplitude = (2.0 / sum) * (2.0 / sum);
    s.psd = 2.0 / (fs * sq);
}

}  // namespace

int analyzerRateFactor(double sampleRate) noexcept { return dsp::rateFactor(sampleRate); }

int analyzerFftSize(int resolution, double sampleRate) noexcept {
    const int r = std::clamp(resolution, 0, kResolutionCount - 1);
    return (2048 << r) * analyzerRateFactor(sampleRate);
}

void Analyzer::subscribe(FrameConsumer* c, uint8_t sourceMask, SizeChoice size) {
    if (!c) return;
    subs_.push_back({c, sourceMask, size});
    if (std::find(consumers_.begin(), consumers_.end(), c) == consumers_.end()) consumers_.push_back(c);
}

bool Analyzer::prepare(double sampleRate, const AnalyzerSettings& s) {
    if (!(sampleRate > 0.0)) return false;
    sampleRate_ = sampleRate;
    settings_ = s;
    displaySize_ = analyzerFftSize(s.displayResolution, sampleRate);
    highestSize_ = analyzerFftSize(kResolutionCount - 1, sampleRate);
    const auto tapFrames = static_cast<uint32_t>(std::ceil(kTapSeconds * sampleRate));
    for (int si = 0; si < kSourceCount; ++si) {
        const auto src = static_cast<size_t>(si);
        taps_[src].prepare(tapFrames);
        frames_[src].store(0, std::memory_order_relaxed);
        Feed& fd = feeds_[src];
        fd.framers.clear();
        for (auto& c : fd.chunk) c.assign(kChunk, 0.0f);
        for (const auto& sub : subs_) {
            if (!(sub.mask & sourceBit(static_cast<Source>(si)))) continue;
            const int n = fftSize(sub.size);
            Framer* f = nullptr;
            for (auto& existing : fd.framers)
                if (existing->n == n) f = existing.get();
            if (!f) {
                auto nf = std::make_unique<Framer>();
                nf->n = n;
                nf->hop = n / 4;  // 75 % overlap (R4)
                nf->fft = std::make_unique<dsp::RealFft>(n);
                makeWindow(s.window, n, nf->window, nf->scale, sampleRate);
                nf->windowed.assign(static_cast<size_t>(n), 0.0f);
                for (auto& b : nf->buf) b.assign(static_cast<size_t>(n), 0.0f);
                for (auto& p : nf->power) p.assign(static_cast<size_t>(n / 2 + 1), 0.0f);
                nf->mean.assign(static_cast<size_t>(n / 2 + 1), 0.0f);
                nf->spec.assign(static_cast<size_t>(n / 2 + 1), {});
                f = nf.get();
                fd.framers.push_back(std::move(nf));
            }
            f->consumers.push_back(sub.consumer);
        }
    }
    return true;
}

void Analyzer::start() {
    if (thread_.joinable()) return;
    stop_.store(false);
    thread_ = std::thread([this] {
#if defined(AMQ_ANALYZER_FTZ)
        // Flush denormals to zero on this thread: decaying tails would otherwise make the FFTs
        // many times slower and steal time from the audio thread. ARM (macOS) comes with the
        // Mac build.
        _mm_setcsr(_mm_getcsr() | 0x8040);  // FTZ (bit 15) | DAZ (bit 6)
#endif
        while (!stop_.load(std::memory_order_relaxed)) {
            runPending();
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        }
    });
}

void Analyzer::stop() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    thread_.join();
}

uint64_t Analyzer::runPending() noexcept {
    uint64_t total = 0;
    for (int si = 0; si < kSourceCount; ++si) {
        const auto src = static_cast<size_t>(si);
        AudioTap& tap = taps_[src];
        Feed& fd = feeds_[src];
        if (fd.chunk[0].empty()) continue;  // not prepared
        // Read at most what was there at the start (plus one ring), so a fast producer cannot
        // keep the worker here forever.
        uint64_t budget = tap.available() + tap.capacity();
        while (budget > 0) {
            const auto res = tap.read(fd.chunk[0].data(), fd.chunk[1].data(),
                                      static_cast<uint32_t>(std::min<uint64_t>(kChunk, budget)));
            if (res.gap)
                for (auto& f : fd.framers) f->fill = 0;  // only full frames after a gap (area 17 R4)
            if (res.frames == 0) break;
            budget -= std::min<uint64_t>(budget, res.frames);
            total += res.frames;
            for (auto& f : fd.framers)
                push(static_cast<Source>(si), *f, fd.chunk[0].data(), fd.chunk[1].data(), res.frames,
                     std::max<uint32_t>(1, res.channels), res.endPosition);
        }
    }
    for (auto* c : consumers_) c->onPassEnd();
    return total;
}

void Analyzer::push(Source s, Framer& f, const float* l, const float* r, uint32_t n, uint32_t channels,
                    uint64_t endPosition) noexcept {
    uint32_t done = 0;
    while (done < n) {
        const auto k = static_cast<uint32_t>(std::min<int>(f.n - f.fill, static_cast<int>(n - done)));
        std::memcpy(f.buf[0].data() + f.fill, l + done, k * sizeof(float));
        std::memcpy(f.buf[1].data() + f.fill, r + done, k * sizeof(float));
        f.fill += static_cast<int>(k);
        done += k;
        if (f.fill == f.n) {
            emit(s, f, channels, endPosition - (n - done));
            const auto keep = static_cast<size_t>(f.n - f.hop);
            for (auto& b : f.buf) std::memmove(b.data(), b.data() + f.hop, keep * sizeof(float));
            f.fill = f.n - f.hop;
        }
    }
}

void Analyzer::emit(Source s, Framer& f, uint32_t channels, uint64_t endPosition) noexcept {
    const uint32_t chans = std::min(channels, AudioTap::kChannels);
    const auto bins = static_cast<size_t>(f.n / 2 + 1);
    for (uint32_t c = 0; c < chans; ++c) {
        const float* x = f.buf[c].data();
        for (int i = 0; i < f.n; ++i) f.windowed[static_cast<size_t>(i)] = x[i] * f.window[static_cast<size_t>(i)];
        f.fft->forward(f.windowed.data(), f.spec.data());
        float* p = f.power[c].data();
        for (size_t k = 0; k < bins; ++k) p[k] = std::norm(f.spec[k]);
    }
    if (chans == 1) {
        std::memcpy(f.mean.data(), f.power[0].data(), bins * sizeof(float));
    } else {
        for (size_t k = 0; k < bins; ++k) f.mean[k] = 0.5f * (f.power[0][k] + f.power[1][k]);
    }
    SpectrumFrame fr;
    fr.source = s;
    fr.fftSize = f.n;
    fr.bins = static_cast<int>(bins);
    fr.sampleRate = sampleRate_;
    fr.channels = chans;
    fr.power[0] = f.power[0].data();
    fr.power[1] = chans > 1 ? f.power[1].data() : f.power[0].data();
    fr.meanPower = f.mean.data();
    fr.scale = &f.scale;
    fr.endPosition = endPosition;
    fr.index = f.index++;
    for (auto* c : f.consumers) c->onFrame(fr);
    frames_[static_cast<size_t>(s)].fetch_add(1, std::memory_order_relaxed);
}

}  // namespace amq::analysis
