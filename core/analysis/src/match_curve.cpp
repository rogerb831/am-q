#include "amq/analysis/match_curve.hpp"

#include <algorithm>
#include <cmath>

namespace amq::analysis {

double smoothingOctaves(MatchSmoothing s) noexcept {
    switch (s) {
        case MatchSmoothing::Third: return 1.0 / 3.0;
        case MatchSmoothing::Sixth: return 1.0 / 6.0;
        case MatchSmoothing::Twelfth: return 1.0 / 12.0;
        case MatchSmoothing::TwentyFourth: return 1.0 / 24.0;
    }
    return 1.0 / 6.0;
}

void smoothFractionalOctave(const std::array<double, kMatchGridPoints>& in, double octaves,
                            std::array<double, kMatchGridPoints>& out) noexcept {
    // The grid is uniform in log frequency, so a fixed fraction of an octave is a fixed point count.
    const int half = std::max(0, static_cast<int>(std::lround(0.5 * octaves * kMatchPointsPerOctave)));
    for (int i = 0; i < kMatchGridPoints; ++i) {
        const int h = std::min({half, i, kMatchGridPoints - 1 - i});  // shrink at the edges, keep centred
        double s = 0.0;
        for (int j = i - h; j <= i + h; ++j) s += in[static_cast<size_t>(j)];
        out[static_cast<size_t>(i)] = s / (2 * h + 1);
    }
}

bool computeMatchCurve(const MatchCapture& input, const MatchCapture& reference, const MatchCurveSettings& s,
                       MatchCurve& out) noexcept {
    out.valid = false;
    if (!input.ready() || !reference.ready()) return false;  // never a silent flat curve (R7)
    if (!input.meanDb(out.inputDb) || !reference.meanDb(out.referenceDb)) return false;
    out.sampleRate = input.sampleRate();
    const double li = input.loudnessLufs(), lr = reference.loudnessLufs();
    out.levelOffsetDb = std::isfinite(li) && std::isfinite(lr) ? lr - li : 0.0;
    out.aligned = s.levelAlign;
    std::array<double, kMatchGridPoints> d{};
    for (int i = 0; i < kMatchGridPoints; ++i)
        d[static_cast<size_t>(i)] = out.referenceDb[static_cast<size_t>(i)] - out.inputDb[static_cast<size_t>(i)] -
                                   (s.levelAlign ? out.levelOffsetDb : 0.0);
    smoothFractionalOctave(d, smoothingOctaves(s.smoothing), out.target);
    for (auto& v : out.target) v = std::clamp(v, -kMatchClampDb, kMatchClampDb);
    // Confidence: distance of each capture above its floor.
    auto floorOf = [](const std::array<double, kMatchGridPoints>& db) {
        return std::max(*std::max_element(db.begin(), db.end()) - kMatchFloorRangeDb, kMatchAbsoluteFloorDb);
    };
    const double fi = floorOf(out.inputDb), fr = floorOf(out.referenceDb);
    const double usable = std::min(20000.0, 0.45 * out.sampleRate);
    const auto& g = matchGrid();
    for (int i = 0; i < kMatchGridPoints; ++i) {
        const auto k = static_cast<size_t>(i);
        if (g[k] > usable) {
            out.weight[k] = 0.0;
            continue;
        }
        const double m = std::min(out.inputDb[k] - fi, out.referenceDb[k] - fr) - kMatchConfidenceRampDb;
        out.weight[k] = std::clamp(m / kMatchConfidenceRampDb, 0.0, 1.0);
    }
    out.valid = true;
    return true;
}

}  // namespace amq::analysis
