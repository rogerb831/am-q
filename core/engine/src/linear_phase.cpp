#include "amq/engine/linear_phase.hpp"

#include "amq/dsp/analog_prototype.hpp"
#include "amq/dsp/latency.hpp"
#include "amq/engine/band_response.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace amq::engine {

bool LinearPhaseStage::prepare(uint32_t maxFrames, double sampleRate, int resolution, const CorrectionRequest& initial) {
    stopWorker();
    const auto size = dsp::linearPhaseSize(resolution, sampleRate);
    taps_ = size.firTaps;
    head_ = size.partition;
    sampleRate_ = sampleRate;
    maxFrames_ = maxFrames;
    const int maxPart = kLinearPhaseMaxPartitionBase * dsp::rateFactor(sampleRate);
    try {
        for (auto& s : set_)
            for (auto& c : s)
                if (!c.prepare(head_, maxPart, taps_)) return false;
        buf_.assign(static_cast<size_t>(maxFrames) * 10, 0.0f);
        window_.assign(static_cast<size_t>(taps_), 1.0f);
        if (!allocate(worker_) || !allocate(audio_)) return false;
        if (!workerRequest_) workerRequest_ = std::make_unique<CorrectionRequest>();
    } catch (...) {
        return false;
    }
    // Tukey window: raised-cosine tapers over the outer alpha / 2 of each side.
    const double edge = 0.5 * kLinearPhaseTukeyAlpha * taps_;
    for (int i = 0; i < taps_; ++i) {
        const double d = std::min(i, taps_ - 1 - i);
        window_[static_cast<size_t>(i)] =
            d < edge ? static_cast<float>(0.5 - 0.5 * std::cos(3.14159265358979323846 * d / edge)) : 1.0f;
    }
    build(initial, worker_);
    for (auto& s : set_)
        for (size_t k = 0; k < 4; ++k)
            if (!s[k].setKernel(worker_.taps[k].data(), taps_)) return false;
    request_.write(CorrectionRequest{});
    builtId_ = appliedId_ = initial.id;
    worker_.pending = audio_.pending = 0;
    standbyId_.store(0, std::memory_order_relaxed);
    active_.store(0, std::memory_order_relaxed);
    phase_.store(kIdle, std::memory_order_relaxed);
    fadeLen_ = static_cast<uint32_t>(std::lround(kLinearPhaseFadeMs * 1e-3 * sampleRate));
    fadePos_ = 0;
    swaps_ = 0;
    return true;
}

bool LinearPhaseStage::allocate(Builder& b) {
    b.fft = std::make_unique<dsp::RealFft>(taps_);
    b.spec.assign(static_cast<size_t>(taps_ / 2 + 1), {});
    b.mag.assign(static_cast<size_t>(4 * (taps_ / 2 + 1)), 1.0);
    for (auto& t : b.taps) t.assign(static_cast<size_t>(taps_), 0.0f);
    b.pending = 0;
    return true;
}

void LinearPhaseStage::build(const CorrectionRequest& r, Builder& bd) const {
    const int bins = taps_ / 2 + 1;
    auto& out = bd.taps;
    double* l = bd.mag.data();
    double* rr = l + bins;
    double* m = rr + bins;
    double* s = m + bins;
    std::fill(bd.mag.begin(), bd.mag.end(), 1.0);
    bool ms = false;
    for (const auto& b : r.bands) {
        if (!b.active) continue;
        const dsp::AnalogPrototype p = dsp::designAnalogPrototype(bandSpecFrom(b.in));
        if (!p.supported) continue;
        double* a = b.stage == 0 ? l : m;
        double* c = b.stage == 0 ? rr : s;
        if (b.stage == 1) ms = true;
        for (int k = 0; k < bins; ++k) {
            const double g = std::abs(p.evaluateHz(k * sampleRate_ / taps_));
            if (b.mask & 1u) a[k] *= g;
            if (b.mask & 2u) c[k] *= g;
        }
    }
    for (int e = 0; e < 4; ++e) {
        if (!ms && (e == 1 || e == 2)) {  // no M/S band: no cross terms
            std::fill(out[static_cast<size_t>(e)].begin(), out[static_cast<size_t>(e)].end(), 0.0f);
            continue;
        }
        for (int k = 0; k < bins; ++k) {
            double v = 0.0;
            switch (e) {
                case 0: v = 0.5 * (m[k] + s[k]) * l[k]; break;
                case 1: v = 0.5 * (m[k] - s[k]) * rr[k]; break;
                case 2: v = 0.5 * (m[k] - s[k]) * l[k]; break;
                default: v = 0.5 * (m[k] + s[k]) * rr[k]; break;
            }
            // Zero phase, delayed by N/2 samples: e^{-j pi k} = (-1)^k.
            bd.spec[static_cast<size_t>(k)] = {static_cast<float>(k & 1 ? -v : v), 0.0f};
        }
        float* h = out[static_cast<size_t>(e)].data();
        bd.fft->inverse(bd.spec.data(), h);
        for (int i = 0; i < taps_; ++i) h[i] *= window_[static_cast<size_t>(i)];
    }
}

void LinearPhaseStage::post(const CorrectionRequest& r) noexcept { request_.write(r); }

bool LinearPhaseStage::trySubmit(Builder& bd) noexcept {
    if (phase_.load(std::memory_order_acquire) != kIdle) return false;
    auto& standby = set_[static_cast<size_t>(1 - active_.load(std::memory_order_relaxed))];
    for (auto& c : standby)
        if (c.swapInProgress()) return false;
    for (size_t k = 0; k < 4; ++k) standby[k].submitKernel(bd.taps[k].data(), taps_);
    standbyId_.store(bd.pending, std::memory_order_relaxed);
    bd.pending = 0;
    phase_.store(kSubmitted, std::memory_order_release);
    return true;
}

int LinearPhaseStage::runPending() {
    if (!workerRequest_) return 0;
    if (request_.tryRead(*workerRequest_) && workerRequest_->id != 0 && workerRequest_->id != builtId_) {
        build(*workerRequest_, worker_);
        builtId_ = workerRequest_->id;
        worker_.pending = builtId_;
    }
    return worker_.pending && trySubmit(worker_) ? 1 : 0;
}

void LinearPhaseStage::buildNow(const CorrectionRequest& r) noexcept {
    // Offline rendering: build here (milliseconds are fine offline) and submit as soon as the
    // standby set is free.
    build(r, audio_);
    audio_.pending = r.id;
}

void LinearPhaseStage::process(double* const* w, uint32_t channels, uint32_t n) noexcept {
    if (maxFrames_ == 0 || n == 0) return;
    if (audio_.pending) trySubmit(audio_);  // offline builds
    const int a = active_.load(std::memory_order_relaxed);
    const int b = 1 - a;
    // A submitted set whose segments have all switched: start the crossfade.
    if (phase_.load(std::memory_order_acquire) == kSubmitted) {
        bool ready = true;
        for (const auto& c : set_[static_cast<size_t>(b)]) ready = ready && !c.swapInProgress();
        if (ready) {
            phase_.store(kSwitching, std::memory_order_relaxed);
            fadePos_ = 0;
        }
    }
    float* xl = buf_.data();
    float* xr = xl + maxFrames_;
    float* o = xr + maxFrames_;  // [set][kernel]: 8 outputs
    for (uint32_t i = 0; i < n; ++i) xl[i] = static_cast<float>(w[0][i]);
    if (channels >= 2)
        for (uint32_t i = 0; i < n; ++i) xr[i] = static_cast<float>(w[1][i]);
    else
        std::fill(xr, xr + n, 0.0f);
    for (int sIdx = 0; sIdx < 2; ++sIdx)
        for (int k = 0; k < 4; ++k) {
            float* y = o + static_cast<size_t>(sIdx * 4 + k) * maxFrames_;
            set_[static_cast<size_t>(sIdx)][static_cast<size_t>(k)].process(k % 2 == 0 ? xl : xr, y, n);
        }
    auto out = [&](int sIdx, int k, uint32_t i) {
        return static_cast<double>(o[static_cast<size_t>(sIdx * 4 + k) * maxFrames_ + i]);
    };
    const bool switching = phase_.load(std::memory_order_relaxed) == kSwitching;
    for (uint32_t i = 0; i < n; ++i) {
        double yl = out(a, 0, i) + out(a, 1, i);
        double yr = out(a, 2, i) + out(a, 3, i);
        if (switching && fadePos_ < fadeLen_) {
            const double t = static_cast<double>(++fadePos_) / fadeLen_;
            yl += t * (out(b, 0, i) + out(b, 1, i) - yl);
            yr += t * (out(b, 2, i) + out(b, 3, i) - yr);
        } else if (switching) {
            yl = out(b, 0, i) + out(b, 1, i);
            yr = out(b, 2, i) + out(b, 3, i);
        }
        w[0][i] = yl;
        if (channels >= 2) w[1][i] = yr;
    }
    if (switching && fadePos_ >= fadeLen_) {
        active_.store(b, std::memory_order_relaxed);
        appliedId_ = standbyId_.load(std::memory_order_relaxed);
        ++swaps_;
        phase_.store(kIdle, std::memory_order_release);
    }
}

void LinearPhaseStage::startWorker() {
    if (thread_.joinable()) return;
    stop_.store(false);
    thread_ = std::thread([this] {
        while (!stop_.load(std::memory_order_relaxed))
            if (runPending() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
}

void LinearPhaseStage::stopWorker() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    thread_.join();
}

}  // namespace amq::engine
