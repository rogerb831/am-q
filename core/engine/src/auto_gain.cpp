#include "amq/engine/auto_gain.hpp"

#include "amq/dsp/analog_prototype.hpp"
#include "amq/dsp/auto_gain.hpp"
#include "amq/dsp/speakers.hpp"
#include "amq/engine/band_response.hpp"

#include <chrono>
#include <cmath>

namespace amq::engine {

AutoGainStage::AutoGainStage()
    : workerCache_(std::make_unique<std::array<Entry, params::kBandCount>>()),
      audioCache_(std::make_unique<std::array<Entry, params::kBandCount>>()),
      workerRequest_(std::make_unique<CorrectionRequest>()) {
    (void)dsp::autoGainWeights();  // build the weight table now, never on the audio thread (R13)
}

void AutoGainStage::prepare() {
    stopWorker();
    for (auto& e : *workerCache_) e.valid = false;
    for (auto& e : *audioCache_) e.valid = false;
    request_.write(CorrectionRequest{});
    builtId_ = 0;
    id_.store(0, std::memory_order_relaxed);
    db_.store(0.0, std::memory_order_relaxed);
}

double AutoGainStage::compute(const CorrectionRequest& r, std::array<Entry, params::kBandCount>& cache) noexcept {
    using C = std::complex<double>;
    const auto& grid = dsp::logGrid();
    bool any = false;
    for (size_t b = 0; b < r.bands.size(); ++b) {
        const auto& band = r.bands[b];
        if (!band.active) continue;
        auto& e = cache[b];
        if (!e.valid || !(e.in == band.in)) {  // one-band edits recompute one band
            const auto p = dsp::designAnalogPrototype(bandSpecFrom(band.in));
            for (int i = 0; i < dsp::kLogGridPoints; ++i)
                e.h[static_cast<size_t>(i)] = p.supported ? p.evaluateHz(grid[static_cast<size_t>(i)]) : C(1.0);
            e.in = band.in;
            e.valid = true;
            recomputes_.fetch_add(1, std::memory_order_relaxed);
        }
        any = true;
    }
    if (!any) return 0.0;  // all flat: exactly 0 dB (area 05 "Auto Gain flat")
    static thread_local std::array<double, dsp::kLogGridPoints> pl, pr;
    for (int i = 0; i < dsp::kLogGridPoints; ++i) {
        C l = 1.0, rr = 1.0, m = 1.0, s = 1.0;
        for (size_t b = 0; b < r.bands.size(); ++b) {
            const auto& band = r.bands[b];
            if (!band.active) continue;
            const C h = cache[b].h[static_cast<size_t>(i)];
            if (band.stage == 0) {
                if (band.mask & 1u) l *= h;
                if (band.mask & 2u) rr *= h;
            } else {
                if (band.mask & 1u) m *= h;
                if (band.mask & 2u) s *= h;
            }
        }
        if (r.channels < 2) {
            pl[static_cast<size_t>(i)] = std::norm(l);
            continue;
        }
        const C ll = 0.5 * (m + s) * l, lr = 0.5 * (m - s) * rr, rl = 0.5 * (m - s) * l, rrr = 0.5 * (m + s) * rr;
        pl[static_cast<size_t>(i)] = std::norm(ll) + std::norm(lr);  // uncorrelated, equal-power inputs
        pr[static_cast<size_t>(i)] = std::norm(rl) + std::norm(rrr);
    }
    if (r.channels < 2) return dsp::autoGainChannelDb(pl.data());
    // Weighted geometric mean over channels with the speaker loudness weights (R9, R12).
    const double wl = dsp::speakerLoudnessWeight(dsp::speakerOf(dsp::ChannelLayout::Stereo, 0));
    const double wr = dsp::speakerLoudnessWeight(dsp::speakerOf(dsp::ChannelLayout::Stereo, 1));
    const double g = (wl * dsp::autoGainChannelDb(pl.data()) + wr * dsp::autoGainChannelDb(pr.data())) / (wl + wr);
    return std::clamp(g, -dsp::kAutoGainClampDb, dsp::kAutoGainClampDb);
}

int AutoGainStage::runPending() {
    if (!request_.tryRead(*workerRequest_)) return 0;
    if (workerRequest_->id == 0 || workerRequest_->id == builtId_) return 0;
    const double db = compute(*workerRequest_, *workerCache_);
    builtId_ = workerRequest_->id;
    db_.store(db, std::memory_order_relaxed);
    id_.store(builtId_, std::memory_order_release);
    return 1;
}

double AutoGainStage::computeNow(const CorrectionRequest& r) noexcept {
    const double db = compute(r, *audioCache_);
    db_.store(db, std::memory_order_relaxed);
    id_.store(r.id, std::memory_order_release);
    return db;
}

void AutoGainStage::startWorker() {
    if (thread_.joinable()) return;
    stop_.store(false);
    thread_ = std::thread([this] {
        while (!stop_.load(std::memory_order_relaxed))
            if (runPending() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
}

void AutoGainStage::stopWorker() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    thread_.join();
}

}  // namespace amq::engine
