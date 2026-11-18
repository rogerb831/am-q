#include "amq/engine/spectral.hpp"

#include "amq/dsp/analog_prototype.hpp"
#include "amq/dsp/spectral_apply.hpp"
#include "amq/engine/band_response.hpp"

#include <algorithm>
#include <cmath>

namespace amq::engine {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

bool SpectralStage::prepare(double sampleRate, uint32_t maxFrames, int resolution, uint32_t padSamples) {
    (void)maxFrames;
    fs_ = sampleRate;
    n_ = spectralFrameSize(resolution, sampleRate);
    hop_ = n_ / 4;
    bins_ = n_ / 2 + 1;
    pad_ = padSamples;
    try {
        fft_ = std::make_unique<dsp::RealFft>(n_);
        win_.assign(static_cast<size_t>(n_), 0.0f);
        double sum = 0.0;
        for (int i = 0; i < n_; ++i) {  // sqrt of the periodic Hann window
            const double h = 0.5 - 0.5 * std::cos(2.0 * kPi * i / n_);
            win_[static_cast<size_t>(i)] = static_cast<float>(std::sqrt(h));
            sum += std::sqrt(h);
        }
        magNorm_ = 2.0 / sum;  // a sine of amplitude A gives |X| = A * sum / 2 at its bin
        for (auto& r : ring_) r.assign(static_cast<size_t>(n_), 0.0f);
        for (auto& o : ola_) o.assign(static_cast<size_t>(n_), 0.0f);
        for (auto& o : out_) o.assign(static_cast<size_t>(hop_), 0.0f);
        for (auto& p : padLine_) p.assign(pad_, 0.0);
        frameBuf_.assign(static_cast<size_t>(n_), 0.0f);
        for (auto& s : spec_) s.assign(static_cast<size_t>(bins_), {});
        for (auto& g : gainDb_) g.assign(static_cast<size_t>(bins_), 0.0);
        scMag_.assign(static_cast<size_t>(bins_), 0.0);
        for (auto& m : magCache_) m.assign(static_cast<size_t>(bins_), 0.0);
        for (auto& b : band_) {
            b.a.assign(dsp::kSpectralMaxBands, Analysis{});
            b.old.assign(dsp::kSpectralMaxBands, Analysis{});
            b.level.assign(dsp::kSpectralMaxBands, 0.0);
            b.lanes.resize(dsp::kSpectralMaxBands);
            b.lanesLive = false;
            b.relDb.assign(static_cast<size_t>(bins_), 0.0f);
            b.binBand.assign(static_cast<size_t>(bins_), 0);
            b.binWeight.assign(static_cast<size_t>(bins_), 1.0f);
            b.was = false;
            b.relValid = false;
            b.gridKey.fill(-1.0);
            b.grid.count = 0;
        }
    } catch (...) {
        return false;
    }
    pos_ = hopFill_ = outPos_ = 0;
    padPos_ = 0;
    rebuilds_ = 0;
    return true;
}

double SpectralStage::grDb(int band) const noexcept {
    if (band < 0 || band >= params::kBandCount) return 0.0;
    const auto& b = band_[static_cast<size_t>(band)];
    if (!b.was) return 0.0;
    double m = 0.0;
    for (int i = 0; i < b.grid.count; ++i) m = std::max(m, b.lanesLive ? b.lanes.zy[static_cast<size_t>(i)] : b.a[static_cast<size_t>(i)].gr);
    return m;
}

int SpectralStage::analysisBands(int band) const noexcept {
    return band >= 0 && band < params::kBandCount ? band_[static_cast<size_t>(band)].grid.count : 0;
}

double SpectralStage::analysisCentreHz(int band, int i) const noexcept {
    if (band < 0 || band >= params::kBandCount) return 0.0;
    const auto& g = band_[static_cast<size_t>(band)].grid;
    return i >= 0 && i < g.count ? g.centreHz[static_cast<size_t>(i)] : 0.0;
}

void SpectralStage::process(double* const* w, const double* const* input, const float* const* sc, uint32_t numSc,
                            uint32_t channels, uint32_t n,
                            const std::array<SpectralBandConfig, params::kBandCount>& cfg) noexcept {
    if (n_ == 0) return;
    bool external = false;
    for (const auto& c : cfg) external = external || (c.on && c.external);
    const bool haveSc = sc && numSc > 0 && sc[0];
    for (uint32_t i = 0; i < n; ++i) {
        const size_t p = static_cast<size_t>(pos_);
        ring_[0][p] = static_cast<float>(w[0][i]);
        ring_[1][p] = static_cast<float>(channels >= 2 ? w[1][i] : 0.0);
        ring_[2][p] = static_cast<float>(input[0][i]);
        ring_[3][p] = static_cast<float>(channels >= 2 ? input[1][i] : input[0][i]);
        const float e0 = haveSc ? sc[0][i] : 0.0f;
        ring_[4][p] = e0;
        ring_[5][p] = haveSc && numSc >= 2 && sc[1] ? sc[1][i] : e0;
        if (++pos_ == n_) pos_ = 0;
        // Output: the block made ready by the last frame (latency n_), then the padding delay.
        double y0 = out_[0][static_cast<size_t>(outPos_)], y1 = out_[1][static_cast<size_t>(outPos_)];
        ++outPos_;
        if (pad_) {
            double& a = padLine_[0][padPos_];
            double& b = padLine_[1][padPos_];
            std::swap(a, y0);
            std::swap(b, y1);
            if (++padPos_ == pad_) padPos_ = 0;
        }
        w[0][i] = y0;
        if (channels >= 2) w[1][i] = y1;
        if (++hopFill_ == hop_) {
            hopFill_ = 0;
            frame(cfg, channels, external);
            outPos_ = 0;
        }
    }
}

namespace {
// 10^(dB / 20) as exp(dB ln10 / 20): the same value to rounding, and cheaper than pow.
inline float dbToGain(double db) noexcept { return static_cast<float>(std::exp(db * 0.11512925464970229)); }
}  // namespace

void SpectralStage::frame(const std::array<SpectralBandConfig, params::kBandCount>& cfg, uint32_t channels,
                          bool external) noexcept {
    // Windowed frames of the last n_ samples of each needed ring, oldest first.
    auto transform = [&](int r) {
        const auto& ring = ring_[static_cast<size_t>(r)];
        for (int i = 0; i < n_; ++i) {
            const int k = (pos_ + i) % n_;
            frameBuf_[static_cast<size_t>(i)] = ring[static_cast<size_t>(k)] * win_[static_cast<size_t>(i)];
        }
        fft_->forward(frameBuf_.data(), spec_[static_cast<size_t>(r)].data());
    };
    bool any = false;
    for (const auto& c : cfg) any = any || c.on;
    transform(0);
    if (channels >= 2) transform(1);
    for (auto& g : gainDb_) std::fill(g.begin(), g.end(), 0.0);
    if (any) {
        transform(2);
        transform(3);
        if (external) {
            transform(4);
            transform(5);
        }
        magValid_.fill(false);  // this frame's spectra: magnitudes recomputed on first use
        for (int b = 0; b < params::kBandCount; ++b) {
            const auto& c = cfg[static_cast<size_t>(b)];
            if (c.on) {
                bandFrame(b, c);
            } else {
                band_[static_cast<size_t>(b)].was = false;
                band_[static_cast<size_t>(b)].lanesLive = false;
            }
        }
    }
    // Apply: L/R components, then M/S through an exact encode / decode.
    auto& sl = spec_[0];
    auto& sr = spec_[1];
    bool ms = false;
    for (int k = 0; k < bins_ && !ms; ++k) ms = gainDb_[2][static_cast<size_t>(k)] != 0.0 || gainDb_[3][static_cast<size_t>(k)] != 0.0;
    for (int k = 0; k < bins_; ++k) {
        const auto kk = static_cast<size_t>(k);
        if (gainDb_[0][kk] != 0.0) sl[kk] *= dbToGain(gainDb_[0][kk]);
        if (gainDb_[1][kk] != 0.0) sr[kk] *= dbToGain(gainDb_[1][kk]);
        if (ms) {
            std::complex<float> m = 0.5f * (sl[kk] + sr[kk]), s = 0.5f * (sl[kk] - sr[kk]);
            if (gainDb_[2][kk] != 0.0) m *= dbToGain(gainDb_[2][kk]);
            if (gainDb_[3][kk] != 0.0) s *= dbToGain(gainDb_[3][kk]);
            sl[kk] = m + s;
            sr[kk] = m - s;
        }
    }
    // Synthesis: inverse, window, overlap-add (x 0.5: the Hann product sums to 2 at hop n/4).
    for (uint32_t c = 0; c < 2; ++c) {
        auto& ola = ola_[c];
        if (c < channels) {
            fft_->inverse(spec_[c].data(), frameBuf_.data());
            for (int i = 0; i < n_; ++i)
                ola[static_cast<size_t>(i)] += 0.5f * frameBuf_[static_cast<size_t>(i)] * win_[static_cast<size_t>(i)];
        }
        // The oldest hop is complete: it becomes the next output block; shift by one hop.
        std::copy(ola.begin(), ola.begin() + hop_, out_[c].begin());
        std::copy(ola.begin() + hop_, ola.end(), ola.begin());
        std::fill(ola.end() - hop_, ola.end(), 0.0f);
    }
}

const std::vector<double>& SpectralStage::detectorMagnitudes(int src, int detect) noexcept {
    const size_t key = static_cast<size_t>(src * 5 + std::clamp(detect, 0, 4));
    auto& out = magCache_[key];
    if (magValid_[key]) return out;
    magValid_[key] = true;
    const auto& xl = spec_[static_cast<size_t>(src ? 4 : 2)];
    const auto& xr = spec_[static_cast<size_t>(src ? 5 : 3)];
    // |z| as sqrt(re^2 + im^2): std::abs on a complex is a slower hypot, and these magnitudes are
    // far from overflow.
    auto mag = [](std::complex<float> z) {
        const double re = z.real(), im = z.imag();
        return std::sqrt(re * re + im * im);
    };
    for (int k = 0; k < bins_; ++k) {
        const auto kk = static_cast<size_t>(k);
        double m = 0.0;
        switch (detect) {
            case 0: m = std::max(mag(xl[kk]), mag(xr[kk])); break;  // linked
            case 1: m = mag(xl[kk]); break;
            case 2: m = mag(xr[kk]); break;
            case 3: m = mag(0.5f * (xl[kk] + xr[kk])); break;
            case 4: m = mag(0.5f * (xl[kk] - xr[kk])); break;
            default: break;
        }
        out[kk] = m * magNorm_;
    }
    return out;
}

void SpectralStage::bandFrame(int b, const SpectralBandConfig& c) noexcept {
    auto& st = band_[static_cast<size_t>(b)];
    const double frameRate = fs_ / hop_;
    const double binHz = fs_ / n_;
    const double cap = std::abs(c.rangeDb);
    if (cap <= 0.0) return;

    // Grid: rebuild at this frame boundary when Density or the span changes (area 08 R3). Each new
    // analysis band starts from the nearest old one (R5).
    const std::array<double, 3> key{c.lo, c.hi, c.density};
    const double ka = c.autoThr ? dsp::kDefaultTimingKnob : c.attackKnob;
    const double kr = c.autoThr ? dsp::kDefaultTimingKnob : c.releaseKnob;
    const bool rebuild = key != st.gridKey || !st.was;
    // New timing: the knobs differ from those the lanes were loaded with (a band's centre only
    // changes with its grid, which rebuilds).
    const bool timing = !rebuild && (ka != st.timingKa || kr != st.timingKr);
    const bool objects = rebuild || timing || (c.autoThr && !st.autoWas);
    if (objects && st.lanesLive) {
        for (int i = 0; i < st.grid.count; ++i) {
            auto& an = st.a[static_cast<size_t>(i)];
            st.lanes.store(static_cast<size_t>(i), an.ref, an.core);
            an.gr = st.lanes.zy[static_cast<size_t>(i)];
        }
        st.lanesLive = false;
    }
    if (rebuild) {
        const bool continuing = st.was && st.grid.count > 0;
        st.oldGrid = st.grid;
        std::swap(st.a, st.old);
        st.grid.build(c.lo, c.hi, c.density, binHz);
        st.gridKey = key;
        ++rebuilds_;
        for (int k = 0; k < bins_; ++k) {  // per-bin weights, once per grid
            int ia, ib;
            double wa;
            st.grid.weights(std::max(k * binHz, 1e-3), ia, ib, wa);
            if (ia == ib) wa = 1.0;
            st.binBand[static_cast<size_t>(k)] = ia;
            st.binWeight[static_cast<size_t>(k)] = static_cast<float>(wa);
        }
        for (int i = 0; i < st.grid.count; ++i) {
            auto& an = st.a[static_cast<size_t>(i)];
            if (continuing) {
                int best = 0;
                double bd = 1e9;
                for (int j = 0; j < st.oldGrid.count; ++j) {
                    const double d = std::abs(std::log2(st.grid.centreHz[static_cast<size_t>(i)] /
                                                        st.oldGrid.centreHz[static_cast<size_t>(j)]));
                    if (d < bd) bd = d, best = j;
                }
                an = st.old[static_cast<size_t>(best)];
            } else {
                an.core.prepare(frameRate);
                an.ref.prepare(frameRate);
                an.gr = 0.0;
                an.timing.fill(-1.0);
            }
        }
    }
    if (c.autoThr && !st.autoWas)
        for (int i = 0; i < st.grid.count; ++i) st.a[static_cast<size_t>(i)].ref.reset();  // warm up (R9)
    st.autoWas = c.autoThr;
    st.was = true;

    // The band's relative response |H(gain + range) / H(gain)| in dB per bin (analog prototypes).
    if (!st.relValid || !(st.relStatic == c.staticIn) || !(st.relDyn == c.dynIn)) {
        const auto ps = dsp::designAnalogPrototype(bandSpecFrom(c.staticIn));
        const auto pd = dsp::designAnalogPrototype(bandSpecFrom(c.dynIn));
        for (int k = 0; k < bins_; ++k) {
            const double hz = std::max(k * binHz, 1.0);
            const double r = std::abs(pd.evaluateHz(hz)) / std::max(std::abs(ps.evaluateHz(hz)), 1e-30);
            st.relDb[static_cast<size_t>(k)] = static_cast<float>(20.0 * std::log10(std::max(r, 1e-15)));
        }
        st.relLo = bins_, st.relHi = 0;
        for (int k = 1; k < bins_; ++k)
            if (std::abs(st.relDb[static_cast<size_t>(k)]) > 1e-9f) st.relLo = std::min(st.relLo, k), st.relHi = k;
        st.relStatic = c.staticIn;
        st.relDyn = c.dynIn;
        st.relValid = true;
    }

    // Detector magnitudes per bin, encoded to the placement (shared by bands with the same source
    // and mode).
    const auto& mag = detectorMagnitudes(c.external ? 1 : 0, c.detect);
    // Levels: the largest weighted bin magnitude per analysis band, over the span.
    std::fill(st.level.begin(), st.level.begin() + st.grid.count, 0.0);
    const int k0 = std::max(1, static_cast<int>(std::floor(c.lo / binHz)));
    const int k1 = std::min(bins_ - 1, static_cast<int>(std::ceil(c.hi / binHz)));
    const int last = st.grid.count - 1;
    for (int k = k0; k <= k1; ++k) {
        const int ia = st.binBand[static_cast<size_t>(k)];
        const int ib = std::min(ia + 1, last);
        const double wa = st.binWeight[static_cast<size_t>(k)];
        const double m = mag[static_cast<size_t>(k)];
        st.level[static_cast<size_t>(ia)] = std::max(st.level[static_cast<size_t>(ia)], wa * m);
        st.level[static_cast<size_t>(ib)] = std::max(st.level[static_cast<size_t>(ib)], (1.0 - wa) * m);
    }
    // Per analysis band: threshold, gain computer, smoothing (at frame rate). The dB chain is the
    // dynamic EQ's (level, auto reference, knee, dual release, final stage), so it runs in the
    // GainLane kernels across analysis bands, with the same operations in the same order as the
    // object chain (tests/test_dynamics.cpp). The final stage's value is the reduction in dB.
    const int na = st.grid.count;
    if (!st.lanesLive) {  // the objects changed (or the band just started): new timing, then load
        for (int i = 0; i < na; ++i) {
            auto& an = st.a[static_cast<size_t>(i)];
            const double fc = st.grid.centreHz[static_cast<size_t>(i)];
            if (const std::array<double, 3> t{ka, kr, fc}; t != an.timing) {
                an.timing = t;
                an.core.setTimes(dsp::attackSeconds(ka, fc), dsp::spectralReleaseSeconds(kr, fc));
            }
            st.lanes.load(static_cast<size_t>(i), an.ref, an.core);
        }
        st.lanesLive = true;
        st.timingKa = ka, st.timingKr = kr;
        st.laneKey[5] = -1.0;  // settings below are rewritten
    }
    // Per-lane settings, rewritten only when they change (the tilt offsets cost a log2 each).
    const std::array<double, 6> lk{c.rangeDb, c.thresholdDb, c.autoThr ? 1.0 : 0.0, c.tilt ? c.centreHz : 0.0,
                                   0.0, static_cast<double>(rebuilds_)};
    if (lk != st.laneKey) {
        st.laneKey = lk;
        const double offset = std::min(cap, dsp::kAutoOffsetCapDb);
        for (int i = 0; i < na; ++i) {
            const auto ii = static_cast<size_t>(i);
            const double fc = st.grid.centreHz[ii];
            st.lanes.autoThr[ii] = c.autoThr ? 1 : 0, st.lanes.thresholdDb[ii] = c.thresholdDb, st.lanes.offsetDb[ii] = offset;
            st.lanes.tiltDb[ii] = c.tilt ? dsp::kTiltDbPerOctave * std::log2(fc / c.centreHz) : 0.0;
            st.lanes.kneeDb[ii] = dsp::kneeWidthDb(st.grid.widthOct[ii], c.rangeDb);
            st.lanes.capDb[ii] = cap, st.lanes.inv[ii] = 1.0 / cap;
        }
    }
    dsp::gainLanesStep(st.lanes, st.level.data(), static_cast<size_t>(na));
    const double* grDb = st.lanes.zy.data();
    // Apply: interpolate the reduction to bins and scale the band's relative response.
    // Bins where the band's relative response is flat (|relDb| <= 1e-9 dB) would add nothing, so the
    // loop covers relLo..relHi only.
    const double inv = 1.0 / cap;
    double* comps[4];
    int nc = 0;
    for (int comp = 0; comp < 4; ++comp)
        if (c.apply & (1u << comp)) comps[nc++] = gainDb_[static_cast<size_t>(comp)].data();
    int k = st.relLo;
    static const bool avx2 = dsp::cpuHasAvx2();
    if (avx2 && nc > 0) {  // four bins at a time, the same bits per bin (dsp/spectral_apply.hpp)
        dsp::SpectralApplyArgs args;
        args.band = st.binBand.data(), args.weight = st.binWeight.data(), args.relDb = st.relDb.data();
        args.grDb = grDb, args.last = last, args.inv = inv, args.nc = nc;
        for (int j = 0; j < nc; ++j) args.comps[j] = comps[j];
        k = dsp::spectralApplyAvx2(args, k, st.relHi);
    }
    for (; k <= st.relHi; ++k) {
        const int ia = st.binBand[static_cast<size_t>(k)];
        const int ib = std::min(ia + 1, last);
        const double wa = st.binWeight[static_cast<size_t>(k)];
        const double frac = (wa * grDb[ia] + (1.0 - wa) * grDb[ib]) * inv;
        if (frac <= 0.0) continue;
        const double db = std::min(frac, 1.0) * st.relDb[static_cast<size_t>(k)];
        for (int j = 0; j < nc; ++j) comps[j][k] += db;
    }
}

}  // namespace amq::engine
