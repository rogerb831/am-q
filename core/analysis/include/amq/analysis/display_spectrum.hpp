#pragma once
// First display consumer of the analyzer (area 19 R8, area 12 R10): maps each source's latest
// frame onto the shared 512-point log grid in dBFS and publishes the pre, post and external
// spectra through the publisher's spectrum triple buffer, at most once per analyzer pass.
//
// Display mapping (a display stage, never fed to EQ Match): a grid point that covers one or
// more FFT bins takes the largest of them, so a sine reads its level; a point narrower than a
// bin interpolates the power linearly between the two nearest bins. Amplitude scaling: a sine
// of peak amplitude A centred on a bin reads 20 log10(A) dBFS.
//
// Phase 1 scope: no ballistics, tilt, freeze or peak hold yet (area 19, Phase 2). A source
// that stops (for example the side chain is unplugged) holds its last frame until then.

#include "amq/analysis/analyzer.hpp"
#include "amq/analysis/publisher.hpp"
#include "amq/dsp/log_grid.hpp"

#include <array>

namespace amq::analysis {

class DisplaySpectrum final : public FrameConsumer {
public:
    // Main thread, with the analyzer worker stopped. fftSize: the analyzer's display size.
    void prepare(SnapshotPublisher* publisher, double sampleRate, int fftSize);

    void onFrame(const SpectrumFrame& f) noexcept override;
    void onPassEnd() noexcept override;

    // Worker thread (tests): the latest values of a source, in dBFS per grid point.
    [[nodiscard]] const std::array<float, dsp::kLogGridPoints>& latest(Source s) const noexcept {
        return latest_[static_cast<size_t>(s)];
    }

private:
    struct Point {
        int lo = 0, hi = -1;  // covered bins, inclusive; hi < lo means interpolate
        int k0 = 0;           // interpolation: bins k0 and k0 + 1
        float frac = 0.0f;
    };
    SnapshotPublisher* publisher_ = nullptr;
    int fftSize_ = 0;
    std::array<Point, dsp::kLogGridPoints> map_{};
    std::array<std::array<float, dsp::kLogGridPoints>, kSourceCount> latest_{};
    bool dirty_ = false;
    uint64_t position_ = 0;
};

}  // namespace amq::analysis
