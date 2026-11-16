#include "amq/analysis/match_capture.hpp"

#include "amq/dsp/loudness.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

namespace amq::analysis {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

const std::array<double, kMatchGridPoints>& matchGrid() noexcept {
    static const std::array<double, kMatchGridPoints> g = [] {
        std::array<double, kMatchGridPoints> a{};
        for (int i = 0; i < kMatchGridPoints; ++i) a[static_cast<size_t>(i)] = kMatchGridMinHz * std::exp2(i / kMatchPointsPerOctave);
        return a;
    }();
    return g;
}

void MatchCapture::prepare(double sampleRate, int fftSize) {
    fs_ = sampleRate;
    n_ = fftSize;
    hop_ = fftSize / 4;
    const int bins = fftSize / 2 + 1, last = bins - 1;
    // The analyzer's window (periodic 4-term Blackman-Harris) and its energy.
    win_.assign(static_cast<size_t>(n_), 0.0f);
    double sq = 0.0;
    for (int i = 0; i < n_; ++i) {
        const double x = 2.0 * kPi * i / n_;
        const double v = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2.0 * x) - 0.01168 * std::cos(3.0 * x);
        win_[static_cast<size_t>(i)] = static_cast<float>(v);
        sq += v * v;
    }
    binPower_ = 2.0 / (static_cast<double>(n_) * sq);
    const double binsPerHz = n_ / fs_;
    const auto& g = matchGrid();
    for (int i = 0; i < kMatchGridPoints; ++i) {
        const double f = g[static_cast<size_t>(i)];
        const double half = std::exp2(0.5 / kMatchPointsPerOctave);
        const double lo = f / half * binsPerHz, hi = f * half * binsPerHz;
        Point& p = map_[static_cast<size_t>(i)];
        p.lo = std::clamp(static_cast<int>(std::ceil(lo)), 0, last);
        p.hi = std::clamp(static_cast<int>(std::floor(hi)), 0, last);
        if (std::ceil(lo) > std::floor(hi)) p.hi = p.lo - 1;
        const double x = std::min(f * binsPerHz, static_cast<double>(last - 1));
        p.k0 = std::min(static_cast<int>(x), last - 1);
        p.frac = x - p.k0;
    }
    band0_ = std::max(1, static_cast<int>(std::ceil(20.0 * binsPerHz)));
    band1_ = std::min(last, static_cast<int>(std::floor(std::min(20000.0, 0.5 * fs_) * binsPerHz)));
    kWeight_.assign(static_cast<size_t>(bins), 0.0);
    for (int k = 0; k < bins; ++k) kWeight_[static_cast<size_t>(k)] = k ? dsp::kWeightingPower(std::min(k / binsPerHz, 23999.0)) : 0.0;
    sum_.assign(kMatchGridPoints, 0.0);
    blocks_.assign(static_cast<size_t>(kMatchMaxBlocks) * kMatchGridPoints, 0.0);
    blockCount_.assign(kMatchMaxBlocks, 0);
    framesPerBlock_ = std::max(1, static_cast<int>(std::lround(kMatchBlockSeconds * fs_ / hop_)));
    framesPer400_ = std::max(1, static_cast<int>(std::lround(0.4 * fs_ / hop_)));
    loudBlocks_.clear();
    loudBlocks_.reserve(static_cast<size_t>(3600.0 / 0.4) + 16);  // an hour without reallocating
    fft_ = std::make_unique<dsp::RealFft>(n_);
    frame_.assign(static_cast<size_t>(n_), 0.0f);
    spec_.assign(static_cast<size_t>(bins), {});
    power_.assign(static_cast<size_t>(bins), 0.0f);
    fileComplete_.store(false);
    doReset();
}

void MatchCapture::doReset() noexcept {
    std::fill(sum_.begin(), sum_.end(), 0.0);
    std::fill(blocks_.begin(), blocks_.end(), 0.0);
    std::fill(blockCount_.begin(), blockCount_.end(), 0u);
    block_ = 0, inBlock_ = 0;
    loudBlocks_.clear();
    loudAcc_ = 0.0, loudFill_ = 0;
    counted_.store(0);
    skipped_.store(0);
}

void MatchCapture::addFrame(const float* p, int bins) noexcept {
    if (resetRequested_.exchange(false, std::memory_order_acq_rel)) doReset();
    if (!recording() || bins != n_ / 2 + 1 || sum_.empty()) return;
    // Gate on the broadband level (area 18 R2): mean square of 20 Hz .. 20 kHz, as a sine's peak dBFS.
    double ms = 0.0, kms = 0.0;
    for (int k = band0_; k <= band1_; ++k) ms += p[k];
    for (int k = 1; k < bins; ++k) kms += p[k] * kWeight_[static_cast<size_t>(k)];
    ms *= binPower_, kms *= binPower_;
    if (!(ms > 0.0) || 10.0 * std::log10(2.0 * ms) < kMatchGateDb) {
        skipped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    double* blk = blocks_.data() + static_cast<size_t>(block_) * kMatchGridPoints;
    for (int i = 0; i < kMatchGridPoints; ++i) {
        const Point& m = map_[static_cast<size_t>(i)];
        double v;
        if (m.hi >= m.lo) {
            v = 0.0;
            for (int k = m.lo; k <= m.hi; ++k) v += p[k];
            v /= static_cast<double>(m.hi - m.lo + 1);
        } else {
            v = p[m.k0] + m.frac * (static_cast<double>(p[m.k0 + 1]) - p[m.k0]);
        }
        sum_[static_cast<size_t>(i)] += v;
        blk[i] += v;
    }
    ++blockCount_[static_cast<size_t>(block_)];
    if (++inBlock_ >= static_cast<uint32_t>(framesPerBlock_)) {  // next block of the ring
        inBlock_ = 0;
        block_ = (block_ + 1) % kMatchMaxBlocks;
        std::fill(blocks_.begin() + static_cast<ptrdiff_t>(block_) * kMatchGridPoints,
                  blocks_.begin() + static_cast<ptrdiff_t>(block_ + 1) * kMatchGridPoints, 0.0);
        blockCount_[static_cast<size_t>(block_)] = 0;
    }
    loudAcc_ += kms;
    if (++loudFill_ >= framesPer400_) {
        if (loudBlocks_.size() < loudBlocks_.capacity()) loudBlocks_.push_back(loudAcc_ / loudFill_);
        loudAcc_ = 0.0, loudFill_ = 0;
    }
    counted_.fetch_add(1, std::memory_order_relaxed);
}

bool MatchCapture::analyseSignal(const float* const* ch, int channelCount, size_t frames, std::atomic<float>* progress,
                                 const std::atomic<bool>* cancel) {
    if (!fft_ || channelCount <= 0) return false;
    const bool wasRecording = recording();
    setRecording(true);
    const int bins = n_ / 2 + 1;
    for (size_t start = 0; start + static_cast<size_t>(n_) <= frames; start += static_cast<size_t>(hop_)) {  // full frames only
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            setRecording(wasRecording);
            return false;
        }
        std::fill(power_.begin(), power_.end(), 0.0f);
        for (int c = 0; c < channelCount; ++c) {
            for (int i = 0; i < n_; ++i) frame_[static_cast<size_t>(i)] = ch[c][start + static_cast<size_t>(i)] * win_[static_cast<size_t>(i)];
            fft_->forward(frame_.data(), spec_.data());
            for (int k = 0; k < bins; ++k) power_[static_cast<size_t>(k)] += std::norm(spec_[static_cast<size_t>(k)]) / channelCount;
        }
        addFrame(power_.data(), bins);
        if (progress) progress->store(static_cast<float>(double(start + static_cast<size_t>(n_)) / double(frames)), std::memory_order_relaxed);
    }
    setRecording(wasRecording);
    if (progress) progress->store(1.0f, std::memory_order_relaxed);
    return true;
}

bool MatchCapture::ready() const noexcept {
    return fileComplete_.load(std::memory_order_acquire) ? countedFrames() > 0 : countedSeconds() >= kMatchReadySeconds;
}

double MatchCapture::countedSeconds() const noexcept {
    return hop_ > 0 ? static_cast<double>(countedFrames()) * hop_ / fs_ : 0.0;
}

bool MatchCapture::meanDb(std::array<double, kMatchGridPoints>& out) const noexcept {
    if (window_.load(std::memory_order_relaxed) == Window::Whole) {
        const auto n = static_cast<double>(countedFrames());
        if (n <= 0.0) return false;
        for (int i = 0; i < kMatchGridPoints; ++i)
            out[static_cast<size_t>(i)] = 10.0 * std::log10(std::max(sum_[static_cast<size_t>(i)] / n * binPower_, 1e-30));
        return true;
    }
    // Last N seconds: the current block and the ones before it, ceil(N / 0.5) in all.
    const double sec = std::clamp(seconds_.load(std::memory_order_relaxed), 1.0, 60.0);
    const int k = std::clamp(static_cast<int>(std::ceil(sec / kMatchBlockSeconds)), 1, kMatchMaxBlocks);
    double count = 0.0;
    std::array<double, kMatchGridPoints> acc{};
    for (int j = 0; j < k; ++j) {
        const int b = (block_ - j + kMatchMaxBlocks) % kMatchMaxBlocks;
        count += blockCount_[static_cast<size_t>(b)];
        const double* blk = blocks_.data() + static_cast<size_t>(b) * kMatchGridPoints;
        for (int i = 0; i < kMatchGridPoints; ++i) acc[static_cast<size_t>(i)] += blk[i];
    }
    if (count <= 0.0) return false;
    for (int i = 0; i < kMatchGridPoints; ++i)
        out[static_cast<size_t>(i)] = 10.0 * std::log10(std::max(acc[static_cast<size_t>(i)] / count * binPower_, 1e-30));
    return true;
}

double MatchCapture::loudnessLufs() const noexcept {
    auto lufs = [](double ms) { return -0.691 + 10.0 * std::log10(ms); };
    double sum = 0.0;
    size_t n = 0;
    for (double b : loudBlocks_)
        if (b > 0.0 && lufs(b) > -70.0) sum += b, ++n;
    if (n == 0) return -HUGE_VAL;
    const double rel = lufs(sum / static_cast<double>(n)) - 10.0;
    sum = 0.0, n = 0;
    for (double b : loudBlocks_)
        if (b > 0.0 && lufs(b) > -70.0 && lufs(b) > rel) sum += b, ++n;
    return n ? lufs(sum / static_cast<double>(n)) : -HUGE_VAL;
}

}  // namespace amq::analysis
