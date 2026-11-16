#include "amq/engine/natural_phase.hpp"

#include "amq/dsp/analog_prototype.hpp"
#include "amq/engine/band_response.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <memory>

namespace amq::engine {

namespace {

using cd = std::complex<double>;
using K = dsp::NaturalPhaseKernelBuilder;
constexpr double kPi = 3.14159265358979323846;

struct M2 {
    cd a, b, c, d;  // [[a, b], [c, d]]
};

M2 mul(const M2& x, const M2& y) noexcept {
    return {x.a * y.a + x.b * y.c, x.a * y.b + x.b * y.d, x.c * y.a + x.d * y.c, x.c * y.b + x.d * y.d};
}
M2 diag(cd p, cd q) noexcept { return {p, 0.0, 0.0, q}; }

const M2 kEnc{0.5, 0.5, 0.5, -0.5};  // M = (L+R)/2, S = (L-R)/2 (area 03 R1)
const M2 kDec{1.0, 1.0, 1.0, -1.0};  // L = M+S, R = M-S

double capLimit() noexcept {
    static const double limit = std::pow(10.0, kCorrectionCapDb / 20.0);
    return limit;
}

// PROVISIONAL guard (natural_phase.hpp): 1/d, with |d| floored relative to its target a so
// that |a / d| never exceeds the cap. Relative, because a cut's response is tiny near DC in
// both its analog and digital form while their ratio stays near 1.
cd inverse(cd d, cd a) noexcept {
    const double lo = std::max(std::abs(a) / capLimit(), 1e-300);
    const double m = std::abs(d);
    if (!(m >= lo)) return m > 0.0 ? (1.0 / lo) * (std::conj(d) / m) : cd(1.0 / lo);
    return 1.0 / d;
}

cd cap(cd c) noexcept {
    if (!std::isfinite(c.real()) || !std::isfinite(c.imag())) return 1.0;
    const double m = std::abs(c);
    return m > capLimit() ? c * (capLimit() / m) : c;
}

}  // namespace

namespace {

bool sameCascade(const dsp::SvfCascade& x, const dsp::SvfCascade& y) noexcept {
    if (x.sections.size() != y.sections.size()) return false;
    for (size_t i = 0; i < x.sections.size(); ++i) {
        const auto &p = x.sections[i], &q = y.sections[i];
        if (p.g != q.g || p.k != q.k || p.m0 != q.m0 || p.m1 != q.m1 || p.m2 != q.m2) return false;
    }
    return true;
}

}  // namespace

void buildCorrectionKernels(const CorrectionRequest& r, dsp::NaturalPhaseKernelBuilder& builder,
                            CorrectionKernels& out, const CorrectionOptions& opt) {
    auto cache = std::make_unique<CorrectionCache>();
    buildCorrectionKernels(r, builder, *cache, out, opt);
}

void buildCorrectionKernels(const CorrectionRequest& r, dsp::NaturalPhaseKernelBuilder& builder,
                            CorrectionCache& cache, CorrectionKernels& out, const CorrectionOptions& opt) noexcept {
    out.id = r.id;
    const bool stereo = r.channels >= 2;
    std::array<bool, params::kBandCount> use{};
    bool any = false, anyMs = false;
    for (size_t b = 0; b < r.bands.size(); ++b) {
        const auto& band = r.bands[b];
        const uint8_t mask = stereo ? band.mask : static_cast<uint8_t>(band.mask & 1u);
        if (!band.active || mask == 0 || band.cascade.sections.empty()) continue;
        if (!stereo && band.stage == 1) continue;  // no M/S stage on a mono channel
        // Area 04 R12: recompute this band's responses only if its design changed.
        auto& e = cache.bands[b];
        if (!e.valid || !(e.in == band.in) || !sameCascade(e.cascade, band.cascade)) {
            e.in = band.in;
            e.cascade = band.cascade;
            e.proto = dsp::designAnalogPrototype(bandSpecFrom(band.in));
            if (e.proto.supported)
                for (int k = 0; k < K::kBins; ++k) {
                    const double hz = std::max(K::binHz(k, r.sampleRate), 1e-3);  // DC: the limit of the ratio
                    e.target[static_cast<size_t>(k)] = e.proto.evaluateHz(hz);
                    e.digital[static_cast<size_t>(k)] = dsp::svfCascadeResponseHz(band.cascade, hz, r.sampleRate);
                }
            e.valid = true;
            ++cache.recomputed;
        }
        if (!e.proto.supported || e.proto.brickwall) continue;  // Brickwall stays its IIR (area 03 R16)
        use[b] = true;
        any = true;
        anyMs = anyMs || band.stage == 1;
    }
    if (!any) {  // every band flat: the exact identity, no cross terms
        K::identity(out.taps[0].data());
        out.taps[1].fill(0.0f);
        out.taps[2].fill(0.0f);
        K::identity(out.taps[3].data());
        return;
    }

    // The 2x2 correction at one frequency, and the smallest digital band response there. Bin k
    // (k >= 0) reads the cached responses; k < 0 evaluates at hz (the singular-bin limit).
    auto correctionAt = [&](double hz, double& minD, int k = -1) {
        cd lrA[2] = {1.0, 1.0}, lrD[2] = {1.0, 1.0}, msA[2] = {1.0, 1.0}, msD[2] = {1.0, 1.0};
        minD = 1e300;
        for (size_t b = 0; b < r.bands.size(); ++b) {
            if (!use[b]) continue;
            const auto& band = r.bands[b];
            const auto& e = cache.bands[b];
            const cd a = k >= 0 ? e.target[static_cast<size_t>(k)] : e.proto.evaluateHz(hz);
            const cd d = k >= 0 ? e.digital[static_cast<size_t>(k)] : dsp::svfCascadeResponseHz(band.cascade, hz, r.sampleRate);
            minD = std::min(minD, std::abs(d));
            for (int c = 0; c < 2; ++c) {
                if (!(band.mask & (1u << c))) continue;
                if (band.stage == 0) lrA[c] *= a, lrD[c] *= d;
                else msA[c] *= a, msD[c] *= d;
            }
        }
        if (!anyMs) return diag(lrA[0] * inverse(lrD[0], lrA[0]), lrA[1] * inverse(lrD[1], lrA[1]));
        // C = T * H^-1, H^-1 = diag(1/LRd) * Dec * diag(1/MSd) * Enc.
        const M2 t = mul(mul(mul(kDec, diag(msA[0], msA[1])), kEnc), diag(lrA[0], lrA[1]));
        const M2 hInv = mul(mul(mul(diag(inverse(lrD[0], lrA[0]), inverse(lrD[1], lrA[1])), kDec),
                                diag(inverse(msD[0], msA[0]), inverse(msD[1], msA[1]))),
                            kEnc);
        return mul(t, hInv);
    };

    std::array<std::array<cd, K::kBins>, 4> corr;
    for (int k = 0; k < K::kBins; ++k) {
        const double hz = std::max(K::binHz(k, r.sampleRate), 1e-3);  // DC: the limit of the ratio
        double minD = 0.0;
        M2 cm = correctionAt(hz, minD, k);
        if (opt.singularityLimit && minD < kSingularResponse) {
            // A band's zero sits on this bin: target and digital response are both rounding noise
            // here, but their ratio is smooth through it (a removable singularity: matched designs
            // put the digital zero on the analog one). Take the limit as the mean of the
            // correction just either side, widening the step until both sides are resolvable.
            for (double delta = 1e-4; delta <= 1.6e-2; delta *= 2.0) {
                double lo = 0.0, hi = 0.0;
                const M2 a = correctionAt(hz * (1.0 - delta), lo), b = correctionAt(hz * (1.0 + delta), hi);
                if (lo >= kSingularResponse && hi >= kSingularResponse) {
                    cm = {0.5 * (a.a + b.a), 0.5 * (a.b + b.b), 0.5 * (a.c + b.c), 0.5 * (a.d + b.d)};
                    break;
                }
            }
        }
        const size_t kk = static_cast<size_t>(k);
        corr[0][kk] = cap(cm.a);
        corr[1][kk] = anyMs ? cap(cm.b) : 0.0;
        corr[2][kk] = anyMs ? cap(cm.c) : 0.0;
        corr[3][kk] = stereo ? cap(cm.d) : corr[0][kk];
    }

    // Nyquist phase taper (area 03 R15). The digital response is real at Nyquist, the analog
    // target is not, so the correction's phase there is arbitrary, and the builder keeps only
    // the real part of the Nyquist bin. Above taperStart x Nyquist the unwrapped phase is scaled
    // down to zero with a raised cosine, so the kernel sees no jump; the magnitude is kept.
    const double nyq = 0.5 * r.sampleRate;
    if (opt.taperStart < 1.0) {
        const double f0 = opt.taperStart * nyq;
        for (auto& c : corr) {
            double prev = 0.0, unwrapped = 0.0;
            bool started = false;
            for (int k = 0; k < K::kBins; ++k) {
                const double hz = K::binHz(k, r.sampleRate);
                if (hz < f0) continue;
                auto& v = c[static_cast<size_t>(k)];
                const double ph = std::arg(v);
                if (!started) {
                    unwrapped = ph;
                    started = true;
                } else {
                    double d = ph - prev;
                    while (d > kPi) d -= 2.0 * kPi;
                    while (d < -kPi) d += 2.0 * kPi;
                    unwrapped += d;
                }
                prev = ph;
                const double w = 0.5 * (1.0 + std::cos(kPi * (hz - f0) / (nyq - f0)));
                v = std::polar(std::abs(v), w * unwrapped);
            }
        }
    }

    for (int i = 0; i < 4; ++i) {
        if ((i == 1 || i == 2) && !anyMs) {
            out.taps[static_cast<size_t>(i)].fill(0.0f);
            continue;
        }
        builder.build(corr[static_cast<size_t>(i)].data(), out.taps[static_cast<size_t>(i)].data());
    }
}

bool NaturalPhaseStage::prepare(uint32_t maxFrames, const CorrectionRequest& initial) {
    stopWorker();
    try {
        for (auto& c : conv_)
            if (!c.prepare(dsp::PartitionedConvolver::kDefaultPartition, CorrectionKernels::kTaps)) return false;
        if (!workerBuilder_) workerBuilder_ = std::make_unique<dsp::NaturalPhaseKernelBuilder>();
        if (!audioBuilder_) audioBuilder_ = std::make_unique<dsp::NaturalPhaseKernelBuilder>();
        if (!workerRequest_) workerRequest_ = std::make_unique<CorrectionRequest>();
        if (!workerKernels_) workerKernels_ = std::make_unique<CorrectionKernels>();
        if (!audioKernels_) audioKernels_ = std::make_unique<CorrectionKernels>();
        workerCache_ = std::make_unique<CorrectionCache>();  // fresh per activation
        audioCache_ = std::make_unique<CorrectionCache>();
        buf_.assign(static_cast<size_t>(maxFrames) * 6, 0.0f);
    } catch (...) {
        return false;
    }
    maxFrames_ = maxFrames;
    buildCorrectionKernels(initial, *workerBuilder_, *workerCache_, *workerKernels_);
    for (size_t i = 0; i < conv_.size(); ++i)
        if (!conv_[i].setKernel(workerKernels_->taps[i].data(), CorrectionKernels::kTaps)) return false;
    request_.write(CorrectionRequest{});
    readyId_.store(0, std::memory_order_relaxed);
    builtId_ = appliedId_ = initial.id;
    pendingOffline_ = false;
    swaps_ = 0;
    return true;
}

void NaturalPhaseStage::post(const CorrectionRequest& r) noexcept { request_.write(r); }

void NaturalPhaseStage::buildNow(const CorrectionRequest& r) noexcept {
    buildCorrectionKernels(r, *audioBuilder_, *audioCache_, *audioKernels_);
    pendingOffline_ = true;
}

int NaturalPhaseStage::runPending() {
    if (!workerRequest_ || !request_.tryRead(*workerRequest_)) return 0;
    if (workerRequest_->id == 0 || workerRequest_->id == builtId_) return 0;
    buildCorrectionKernels(*workerRequest_, *workerBuilder_, *workerCache_, *workerKernels_);
    result_.write(*workerKernels_);
    readyId_.store(workerRequest_->id, std::memory_order_release);
    builtId_ = workerRequest_->id;
    return 1;
}

void NaturalPhaseStage::submitAll(const CorrectionKernels& k) noexcept {
    // The four convolvers process the same sample counts, so they share partition boundaries
    // and fade states: when the first is free, all are, and they swap at the same boundary.
    for (size_t i = 0; i < conv_.size(); ++i) conv_[i].submitKernel(k.taps[i].data(), CorrectionKernels::kTaps);
    appliedId_ = k.id;
    ++swaps_;
}

void NaturalPhaseStage::process(double* const* w, uint32_t channels, uint32_t n) noexcept {
    if (maxFrames_ == 0 || n == 0) return;
    if (!conv_[0].swapInProgress()) {
        if (pendingOffline_) {
            submitAll(*audioKernels_);
            pendingOffline_ = false;
        } else if (readyId_.load(std::memory_order_acquire) > appliedId_ && result_.tryRead(*audioKernels_) &&
                   audioKernels_->id > appliedId_) {
            submitAll(*audioKernels_);
        }
    }
    float* xl = buf_.data();
    float* xr = xl + maxFrames_;
    float* o[4] = {xr + maxFrames_, xr + 2 * maxFrames_, xr + 3 * maxFrames_, xr + 4 * maxFrames_};
    for (uint32_t i = 0; i < n; ++i) xl[i] = static_cast<float>(w[0][i]);
    if (channels >= 2)
        for (uint32_t i = 0; i < n; ++i) xr[i] = static_cast<float>(w[1][i]);
    else
        std::fill(xr, xr + n, 0.0f);
    // All four always run, so they stay in step (mono feeds the right input silence).
    conv_[0].process(xl, o[0], n);
    conv_[1].process(xr, o[1], n);
    conv_[2].process(xl, o[2], n);
    conv_[3].process(xr, o[3], n);
    for (uint32_t i = 0; i < n; ++i) w[0][i] = static_cast<double>(o[0][i]) + static_cast<double>(o[1][i]);
    if (channels >= 2)
        for (uint32_t i = 0; i < n; ++i) w[1][i] = static_cast<double>(o[2][i]) + static_cast<double>(o[3][i]);
}

void NaturalPhaseStage::startWorker() {
    if (thread_.joinable()) return;
    stop_.store(false);
    thread_ = std::thread([this] {
        while (!stop_.load(std::memory_order_relaxed))
            if (runPending() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
}

void NaturalPhaseStage::stopWorker() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    thread_.join();
}

}  // namespace amq::engine
