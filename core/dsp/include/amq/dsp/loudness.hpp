#pragma once
// ITU-R BS.1770-4 loudness (area 05 R11 validation, area 20 R? EQ Match level alignment).
//
// K-weighting is the standard's two-stage pre-filter: a high shelf (about +4 dB above 1.5 kHz,
// modelling the head) and the RLB high-pass (about 38 Hz). The standard gives the two biquads at
// 48 kHz; for other rates they are re-derived from the analog parameters that reproduce those
// biquads (published analyses of the standard: shelf f0 = 1681.97 Hz, gain 4.0 dB, Q = 0.7072;
// high-pass f0 = 38.135 Hz, Q = 0.5003), through the bilinear transform with pre-warp. At 48 kHz
// the result matches the standard's coefficients to about 1e-5.
//
// Integrated loudness: 400 ms blocks with 75 % overlap, mean square of the K-weighted channels,
// channel weights (1 for L/R/C, 1.41 for surrounds, 0 for LFE), loudness -0.691 + 10 log10(sum);
// absolute gate -70 LUFS, then a relative gate 10 LU below the mean of the blocks above it.
//
// Not real-time code: the meter keeps every block (validation and EQ Match captures).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace amq::dsp {

struct KBiquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    double process(double x) noexcept {  // transposed direct form II
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0; }
};

// The two K-weighting stages at a sample rate.
inline std::array<KBiquad, 2> kWeightingFilters(double fs) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    std::array<KBiquad, 2> f;
    {  // stage 1: high shelf
        const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan(kPi * f0 / fs);
        const double vh = std::pow(10.0, g / 20.0), vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        f[0].b0 = (vh + vb * k / q + k * k) / a0;
        f[0].b1 = 2.0 * (k * k - vh) / a0;
        f[0].b2 = (vh - vb * k / q + k * k) / a0;
        f[0].a1 = 2.0 * (k * k - 1.0) / a0;
        f[0].a2 = (1.0 - k / q + k * k) / a0;
    }
    {  // stage 2: RLB high-pass
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan(kPi * f0 / fs);
        const double a0 = 1.0 + k / q + k * k;
        f[1].b0 = 1.0, f[1].b1 = -2.0, f[1].b2 = 1.0;
        f[1].a1 = 2.0 * (k * k - 1.0) / a0;
        f[1].a2 = (1.0 - k / q + k * k) / a0;
    }
    return f;
}

// |K(f)|^2 of the K-weighting at a frequency (evaluated at 48 kHz, below 20 kHz).
inline double kWeightingPower(double hz) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    static const std::array<KBiquad, 2> f = kWeightingFilters(48000.0);
    const double w = 2.0 * kPi * hz / 48000.0;
    double p = 1.0;
    for (const auto& s : f) {
        const double c1 = std::cos(w), c2 = std::cos(2 * w), s1 = std::sin(w), s2 = std::sin(2 * w);
        const double nr = s.b0 + s.b1 * c1 + s.b2 * c2, ni = -(s.b1 * s1 + s.b2 * s2);
        const double dr = 1.0 + s.a1 * c1 + s.a2 * c2, di = -(s.a1 * s1 + s.a2 * s2);
        p *= (nr * nr + ni * ni) / (dr * dr + di * di);
    }
    return p;
}

class LoudnessMeter {
public:
    void prepare(double fs, int channels) {
        fs_ = fs;
        ch_ = std::clamp(channels, 1, 8);
        filt_.assign(static_cast<size_t>(ch_), kWeightingFilters(fs));
        weight_.assign(static_cast<size_t>(ch_), 1.0);
        step_ = static_cast<size_t>(std::llround(0.1 * fs));  // 100 ms: blocks of 4 steps, 75 % overlap
        reset();
    }
    void setChannelWeight(int c, double w) {
        if (c >= 0 && c < ch_) weight_[static_cast<size_t>(c)] = w;
    }
    void reset() {
        for (auto& f : filt_)
            for (auto& s : f) s.reset();
        steps_.clear();
        acc_ = 0.0;
        fill_ = 0;
    }
    // Interleaved-free: one call per sample frame, x[c] for each channel.
    void push(const double* x) {
        double sum = 0.0;
        for (int c = 0; c < ch_; ++c) {
            double v = x[c];
            for (auto& s : filt_[static_cast<size_t>(c)]) v = s.process(v);
            sum += weight_[static_cast<size_t>(c)] * v * v;
        }
        acc_ += sum;
        if (++fill_ == step_) {
            steps_.push_back(acc_ / static_cast<double>(step_));
            acc_ = 0.0;
            fill_ = 0;
        }
    }
    // Integrated loudness in LUFS (-inf when nothing passes the gates).
    [[nodiscard]] double integrated() const {
        std::vector<double> blocks;
        for (size_t i = 3; i < steps_.size(); ++i)
            blocks.push_back(0.25 * (steps_[i] + steps_[i - 1] + steps_[i - 2] + steps_[i - 3]));
        auto lufs = [](double ms) { return -0.691 + 10.0 * std::log10(ms); };
        double sum = 0.0;
        size_t n = 0;
        for (double b : blocks)
            if (b > 0.0 && lufs(b) > -70.0) sum += b, ++n;
        if (n == 0) return -HUGE_VAL;
        const double rel = lufs(sum / static_cast<double>(n)) - 10.0;
        sum = 0.0, n = 0;
        for (double b : blocks)
            if (b > 0.0 && lufs(b) > -70.0 && lufs(b) > rel) sum += b, ++n;
        return n ? lufs(sum / static_cast<double>(n)) : -HUGE_VAL;
    }

private:
    double fs_ = 48000.0;
    int ch_ = 2;
    std::vector<std::array<KBiquad, 2>> filt_;
    std::vector<double> weight_;
    std::vector<double> steps_;
    size_t step_ = 4800, fill_ = 0;
    double acc_ = 0.0;
};

}  // namespace amq::dsp
