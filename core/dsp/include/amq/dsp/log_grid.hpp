#pragma once
// The shared log-frequency grid (area 04 R14): 512 points, geometrically spaced from 20 Hz to
// 20 kHz inclusive. Auto Gain, the GUI curve tests and the response tests all use this one
// definition, so their results are directly comparable ("Grid sharing" acceptance test).

#include <array>
#include <cmath>

namespace amq::dsp {

inline constexpr int kLogGridPoints = 512;
inline constexpr double kLogGridMinHz = 20.0;
inline constexpr double kLogGridMaxHz = 20000.0;

[[nodiscard]] inline const std::array<double, kLogGridPoints>& logGrid() noexcept {
    static const std::array<double, kLogGridPoints> grid = [] {
        std::array<double, kLogGridPoints> g{};
        const double ratio = kLogGridMaxHz / kLogGridMinHz;
        for (int i = 0; i < kLogGridPoints; ++i)
            g[static_cast<size_t>(i)] = kLogGridMinHz * std::pow(ratio, i / static_cast<double>(kLogGridPoints - 1));
        g[0] = kLogGridMinHz;  // exact end points
        g[kLogGridPoints - 1] = kLogGridMaxHz;
        return g;
    }();
    return grid;
}

}  // namespace amq::dsp
