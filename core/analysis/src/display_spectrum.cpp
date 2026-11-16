#include "amq/analysis/display_spectrum.hpp"

#include <algorithm>
#include <cmath>

namespace amq::analysis {

static_assert(dsp::kLogGridPoints <= static_cast<int>(kMaxSpectrumBins), "the snapshot must hold the grid");

void DisplaySpectrum::prepare(SnapshotPublisher* publisher, double sampleRate, int fftSize) {
    publisher_ = publisher;
    fftSize_ = fftSize;
    sampleRate_ = sampleRate;
    const auto& g = dsp::logGrid();
    const int last = fftSize / 2;
    const double binsPerHz = fftSize / sampleRate;
    for (int i = 0; i < dsp::kLogGridPoints; ++i) {
        const double f = g[static_cast<size_t>(i)];
        const double below = i > 0 ? g[static_cast<size_t>(i - 1)] : f * f / g[1];
        const double above = i + 1 < dsp::kLogGridPoints ? g[static_cast<size_t>(i + 1)] : f * f / g[static_cast<size_t>(i - 1)];
        const double lo = std::sqrt(below * f) * binsPerHz, hi = std::sqrt(f * above) * binsPerHz;
        Point& p = map_[static_cast<size_t>(i)];
        p.lo = std::clamp(static_cast<int>(std::ceil(lo)), 0, last);
        p.hi = std::clamp(static_cast<int>(std::floor(hi)), 0, last);
        if (std::ceil(lo) > std::floor(hi)) p.hi = p.lo - 1;  // no bin inside: interpolate
        const double x = std::min(f * binsPerHz, static_cast<double>(last));
        p.k0 = std::min(static_cast<int>(x), last - 1);
        p.frac = static_cast<float>(x - p.k0);
    }
    for (int i = 0; i < dsp::kLogGridPoints; ++i) tiltOf_[static_cast<size_t>(i)] = std::log2(g[static_cast<size_t>(i)] / 1000.0);
    for (auto& s : latest_) s.fill(kFloorDb);
    peak_.fill(kFloorDb);
    lastPos_.fill(0);
    dirty_ = false;
    position_ = 0;
}

void DisplaySpectrum::onFrame(const SpectrumFrame& f) noexcept {
    if (f.fftSize != fftSize_) return;
    const float* p = f.meanPower;
    const double scale = f.scale->amplitude;
    const auto src = static_cast<size_t>(f.source);
    auto& out = latest_[src];
    if (freeze_.load(std::memory_order_relaxed)) {  // Freeze: keep what is shown
        lastPos_[src] = f.endPosition;
        return;
    }
    // Follower coefficients from this frame's interval (90 % times).
    const int sp = std::clamp(speed_.load(std::memory_order_relaxed), 0, 4);
    const double dt = lastPos_[src] && f.endPosition > lastPos_[src] ? double(f.endPosition - lastPos_[src]) / sampleRate_ : 0.0;
    lastPos_[src] = f.endPosition;
    const double cRise = dt > 0.0 ? 1.0 - std::pow(0.1, dt / (kRiseMs[static_cast<size_t>(sp)] * 1e-3)) : 1.0;
    const double cFall = dt > 0.0 ? 1.0 - std::pow(0.1, dt / (kFallMs[static_cast<size_t>(sp)] * 1e-3)) : 1.0;
    const double tilt = tilt_.load(std::memory_order_relaxed);
    const bool post = f.source == Source::Post;
    const float peakFall = static_cast<float>(kPeakFallDbPerSecond * dt);
    for (int i = 0; i < dsp::kLogGridPoints; ++i) {
        const Point& m = map_[static_cast<size_t>(i)];
        double v;
        if (m.hi >= m.lo) {
            v = p[m.lo];
            for (int k = m.lo + 1; k <= m.hi; ++k) v = std::max(v, static_cast<double>(p[k]));
        } else {
            v = p[m.k0] + m.frac * (static_cast<double>(p[m.k0 + 1]) - p[m.k0]);
        }
        const double db = std::max(10.0 * std::log10(std::max(v * scale, 1e-30)), static_cast<double>(kFloorDb)) +
                          tilt * tiltOf_[static_cast<size_t>(i)];
        float& y = out[static_cast<size_t>(i)];
        const double prev = y;
        const double next = prev + (db > prev ? cRise : cFall) * (db - prev);
        y = static_cast<float>(std::max(next, static_cast<double>(kFloorDb)));
        if (post) {
            float& pk = peak_[static_cast<size_t>(i)];
            pk = std::max(y, pk - peakFall);
        }
    }
    position_ = std::max(position_, f.endPosition);
    dirty_ = true;
}

void DisplaySpectrum::onPassEnd() noexcept {
    if (!dirty_ || !publisher_) return;
    SpectrumSnapshot& s = publisher_->spectrumBack();
    s.binCount = dsp::kLogGridPoints;
    std::copy(latest_[0].begin(), latest_[0].end(), s.pre.begin());
    std::copy(latest_[1].begin(), latest_[1].end(), s.post.begin());
    std::copy(latest_[2].begin(), latest_[2].end(), s.external.begin());
    std::copy(peak_.begin(), peak_.end(), s.postPeak.begin());
    publisher_->publishSpectrum(position_);
    dirty_ = false;
}

}  // namespace amq::analysis
