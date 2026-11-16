#include "amq/engine/eq_match_fit.hpp"

#include "amq/dsp/realtime_design.hpp"

#include <algorithm>
#include <cmath>

namespace amq::engine {

namespace {

constexpr int kN = analysis::kMatchGridPoints;
constexpr double kMinHz = 10.0, kMaxHz = 30000.0, kMinQ = 0.025, kMaxQ = 40.0;
constexpr double kCutMinQ = 0.3, kCutMaxQ = 1.0;  // non-resonant shelves and cuts (R9, PROVISIONAL)
constexpr double kGolden = 0.6180339887498949;

bool isCutOrShelf(dsp::Shape s) {
    return s == dsp::Shape::LowCut || s == dsp::Shape::HighCut || s == dsp::Shape::LowShelf || s == dsp::Shape::HighShelf;
}

void clampBand(FittedBand& b) {
    b.hz = std::clamp(std::isfinite(b.hz) ? b.hz : 1000.0, kMinHz, kMaxHz);
    b.gainDb = std::clamp(std::isfinite(b.gainDb) ? b.gainDb : 0.0, -30.0, 30.0);
    const double lo = isCutOrShelf(b.shape) ? kCutMinQ : kMinQ, hi = isCutOrShelf(b.shape) ? kCutMaxQ : kMaxQ;
    b.q = std::clamp(std::isfinite(b.q) ? b.q : 1.0, lo, hi);
    if (!dsp::shapeHasGain(b.shape)) b.gainDb = 0.0;
}

// Fast search response: the analog prototype in dB.
void analogDb(const FittedBand& b, MatchArray& out) {
    const auto p = dsp::designAnalogPrototype({b.shape, b.hz, b.gainDb, b.q, b.slope});
    const auto& g = analysis::matchGrid();
    for (int i = 0; i < kN; ++i)
        out[static_cast<size_t>(i)] =
            p.supported ? 20.0 * std::log10(std::max(std::abs(p.evaluateHz(g[static_cast<size_t>(i)])), 1e-9)) : 0.0;
}

double errorWith(const MatchArray& residual, const MatchArray& weight, const MatchArray& resp) {
    double e = 0.0, w = 0.0;
    for (int i = 0; i < kN; ++i) {
        const auto k = static_cast<size_t>(i);
        const double d = residual[k] - resp[k];
        e += weight[k] * d * d;
        w += weight[k];
    }
    return w > 0.0 ? e / w : 0.0;
}

// Golden-section search of one coordinate over [lo, hi]; the band moves only if the error drops.
template <typename Set>
void lineSearch(FittedBand& b, Set set, double lo, double hi, const MatchArray& r, const MatchArray& w, MatchArray& tmp,
                double& best) {
    FittedBand t = b;
    auto eval = [&](double x) {
        t = b;
        set(t, x);
        clampBand(t);
        analogDb(t, tmp);
        return errorWith(r, w, tmp);
    };
    double a = lo, c = hi;
    double x1 = c - kGolden * (c - a), x2 = a + kGolden * (c - a);
    double f1 = eval(x1), f2 = eval(x2);
    for (int it = 0; it < 18; ++it) {
        if (f1 < f2) c = x2, x2 = x1, f2 = f1, x1 = c - kGolden * (c - a), f1 = eval(x1);
        else a = x1, x1 = x2, f1 = f2, x2 = a + kGolden * (c - a), f2 = eval(x2);
    }
    const double x = f1 < f2 ? x1 : x2, fx = std::min(f1, f2);
    if (fx < best) {
        set(b, x);
        clampBand(b);
        best = fx;
    }
}

void refine(FittedBand& b, const MatchArray& r, const MatchArray& w, int sweeps, MatchArray& tmp) {
    clampBand(b);
    analogDb(b, tmp);
    double best = errorWith(r, w, tmp);
    for (int s = 0; s < sweeps; ++s) {
        const double lf = std::log2(b.hz);
        lineSearch(b, [](FittedBand& t, double x) { t.hz = std::exp2(x); }, std::max(lf - 1.0, std::log2(kMinHz)),
                   std::min(lf + 1.0, std::log2(kMaxHz)), r, w, tmp, best);
        if (dsp::shapeHasGain(b.shape))
            lineSearch(b, [](FittedBand& t, double x) { t.gainDb = x; }, std::max(b.gainDb - 12.0, -30.0),
                       std::min(b.gainDb + 12.0, 30.0), r, w, tmp, best);
        const double lq = std::log2(b.q);
        lineSearch(b, [](FittedBand& t, double x) { t.q = std::exp2(x); }, lq - 2.0, lq + 2.0, r, w, tmp, best);
    }
}

}  // namespace

double weightedError(const MatchArray& r, const MatchArray& w) noexcept {
    double e = 0.0, s = 0.0;
    for (int i = 0; i < kN; ++i) e += w[static_cast<size_t>(i)] * r[static_cast<size_t>(i)] * r[static_cast<size_t>(i)], s += w[static_cast<size_t>(i)];
    return s > 0.0 ? e / s : 0.0;
}

void bandResponseDb(const FittedBand& b, double fs, MatchArray& out) {
    dsp::SvfCascade c;
    const auto res = dsp::designBandRealtime({b.shape, b.hz, b.gainDb, b.q, b.slope}, fs, c);
    const auto& g = analysis::matchGrid();
    for (int i = 0; i < kN; ++i) {
        const double hz = std::min(g[static_cast<size_t>(i)], 0.4999 * fs);
        out[static_cast<size_t>(i)] = res == dsp::RealtimeDesignResult::Unsupported
                                          ? 0.0
                                          : 20.0 * std::log10(std::max(std::abs(dsp::svfCascadeResponseHz(c, hz, fs)), 1e-9));
    }
}

FitResult fitMatch(const MatchArray& target, const MatchArray& weightIn, const FitSettings& s, std::atomic<float>* progress,
                   const std::atomic<bool>* cancel) {
    FitResult out;
    MatchArray residual = target, weight = weightIn, tmp{}, resp{};
    for (int i = 0; i < kN; ++i) {  // garbage in: zero weight, zero target (R18)
        const auto k = static_cast<size_t>(i);
        if (!std::isfinite(residual[k]) || !std::isfinite(weight[k])) residual[k] = 0.0, weight[k] = 0.0;
        weight[k] = std::max(weight[k], 0.0);
    }
    out.initialError = out.finalError = weightedError(residual, weight);
    const auto& g = analysis::matchGrid();
    double err = out.initialError;
    for (int round = 0; round < std::min(s.maxBands, 24); ++round) {
        if (err <= 0.0) break;
        std::vector<FittedBand> cands;
        // Cuts at every slope, starting where the edge deficit begins.
        int lo = 0;
        while (lo < kN - 1 && residual[static_cast<size_t>(lo)] < -3.0) ++lo;
        int hi = kN - 1;
        while (hi > 0 && residual[static_cast<size_t>(hi)] < -3.0) --hi;
        for (int sl = 0; sl < dsp::kSlopeCount; ++sl) {
            const auto slope = static_cast<dsp::Slope>(sl);
            if (dsp::slopeAllowed(dsp::Shape::LowCut, slope))
                cands.push_back({dsp::Shape::LowCut, slope, std::max(g[static_cast<size_t>(lo)], 15.0), 0.0, 0.7071});
            if (dsp::slopeAllowed(dsp::Shape::HighCut, slope))
                cands.push_back({dsp::Shape::HighCut, slope, std::min(g[static_cast<size_t>(hi)], 20000.0), 0.0, 0.7071});
        }
        // Shelves from the outer octaves.
        double low = 0.0, high = 0.0;
        for (int i = 0; i < 48; ++i) low += residual[static_cast<size_t>(i)], high += residual[static_cast<size_t>(kN - 1 - i)];
        cands.push_back({dsp::Shape::LowShelf, dsp::Slope::Db12, 150.0, low / 48.0, 0.7071});
        cands.push_back({dsp::Shape::HighShelf, dsp::Slope::Db12, 6000.0, high / 48.0, 0.7071});
        // Bells at the K largest residual peaks by area over +-1/3 octave.
        std::vector<std::pair<double, int>> peaks;
        for (int i = 1; i < kN - 1; ++i) {
            const double r = residual[static_cast<size_t>(i)];
            if (std::abs(r) < std::abs(residual[static_cast<size_t>(i - 1)]) || std::abs(r) < std::abs(residual[static_cast<size_t>(i + 1)]))
                continue;
            double area = 0.0;
            for (int j = std::max(0, i - 16); j <= std::min(kN - 1, i + 16); ++j)
                area += weight[static_cast<size_t>(j)] * residual[static_cast<size_t>(j)];
            peaks.emplace_back(-std::abs(area), i);
        }
        std::sort(peaks.begin(), peaks.end());  // deterministic: by area, then index
        for (int k = 0; k < std::min<int>(s.peaks, static_cast<int>(peaks.size())); ++k) {
            const int i = peaks[static_cast<size_t>(k)].second;
            cands.push_back({dsp::Shape::Bell, dsp::Slope::Db12, g[static_cast<size_t>(i)], residual[static_cast<size_t>(i)], 2.0});
        }
        // Refine every candidate, score the best few exactly, keep the best.
        FittedBand best;
        double bestErr = err;
        bool found = false;
        for (size_t c = 0; c < cands.size(); ++c) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                out.cancelled = true;
                return out;
            }
            FittedBand b = cands[c];
            refine(b, residual, weight, s.sweeps, tmp);
            bandResponseDb(b, s.sampleRate, resp);  // exact digital response (R4)
            const double e = errorWith(residual, weight, resp);
            if (std::isfinite(e) && e < bestErr) bestErr = e, best = b, found = true;
            if (progress)
                progress->store(static_cast<float>((round + double(c + 1) / cands.size()) / std::min(s.maxBands, 24)),
                                std::memory_order_relaxed);
        }
        if (!found) break;
        const double gain = err - bestErr;
        best.errorReduction = gain;
        out.bands.push_back(best);
        bandResponseDb(best, s.sampleRate, resp);
        for (int i = 0; i < kN; ++i) residual[static_cast<size_t>(i)] -= resp[static_cast<size_t>(i)];
        err = bestErr;
        if (gain < s.tolerance * out.initialError) break;
    }
    out.finalError = err;
    // Strongest first (R10). dB responses add, so the order does not change the fitted curve; a
    // stable sort keeps acceptance order between equal contributions (deterministic).
    std::stable_sort(out.bands.begin(), out.bands.end(),
                     [](const FittedBand& a, const FittedBand& b) { return a.errorReduction > b.errorReduction; });
    if (progress) progress->store(1.0f, std::memory_order_relaxed);
    return out;
}

}  // namespace amq::engine
