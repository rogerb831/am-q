#pragma once
// Spectral dynamics stage (area 07; area 02 R8, R9, R12; spec in docs/dsp/spectral-dynamics.md).
//
// A spectral band is a gain band with Dynamic and Spectral on and a non-zero range, in Zero Latency
// or Natural Phase (in Linear Phase it falls back to the IIR dynamic blend, PROVISIONAL). Its
// static shape stays an IIR band; its dynamics run here, on an STFT of the processed signal:
//
//   - Framing: N = 1024, 2048, 4096, 8192, 16384 for Low .. Maximum, times the rate factor (area 02
//     R11); hop N / 4; sqrt-Hann analysis and synthesis windows (their product, a periodic Hann,
//     sums to 2 at this overlap, so unity gains reconstruct the input exactly, N samples late).
//   - Side chain: the plugin input (or the external side-chain bus) is framed the same way and
//     encoded to the band's placement (linked L/R for Stereo, L, R, M or S).
//   - Analysis bands (dsp/spectral_grid.hpp): Density sets their width over the band's span; each
//     band's level is the largest weighted bin magnitude (max pooling, PROVISIONAL), in dBFS peak.
//   - Per analysis band, at frame rate: threshold (manual, or the auto reference envelope), Spectral
//     Tilt, the soft-knee gain computer capped at |range|, and the dual-release smoother with
//     frequency-scaled times (area 06 core, dsp/dynamics.hpp).
//   - Apply: per bin, the gain reduction interpolated with the same weights, divided by |range|,
//     scales the band's relative response |H(gain + range) / H(gain)| in dB (analog prototypes), so
//     the dynamics keep the band's shape. Gains of all spectral bands add in dB per component; L/R
//     components first, then M/S through an exact encode / decode.
//   - Grid changes (Density, span) rebuild the grid at a frame boundary without allocating; each new
//     analysis band starts from the state of the nearest old one, so gains continue (R5).
//
// The stage's latency is N; a delay pads the processed path so the total equals the reported
// FFT-path latency (dsp/latency.hpp). Everything is allocated in prepare(); process() is real-time
// safe.

#include "amq/dsp/fft.hpp"
#include "amq/dsp/latency.hpp"
#include "amq/dsp/spectral_grid.hpp"
#include "amq/engine/design_worker.hpp"
#include "amq/params/param_spec.hpp"

#include <array>
#include <complex>
#include <memory>
#include <vector>

namespace amq::engine {

inline constexpr std::array<int, 5> kSpectralBaseFrame = {1024, 2048, 4096, 8192, 16384};

[[nodiscard]] inline int spectralFrameSize(int resolution, double sampleRate) noexcept {
    const int r = resolution < 0 ? 0 : (resolution > 4 ? 4 : resolution);
    return kSpectralBaseFrame[static_cast<size_t>(r)] * dsp::rateFactor(sampleRate);
}

struct SpectralBandConfig {
    bool on = false;
    uint8_t detect = 0;   // 0 stereo linked, 1 L (or mono), 2 R, 3 Mid, 4 Side
    uint8_t apply = 0;    // components the gain applies to: bit 0 L, 1 R, 2 M, 3 S
    bool external = false, autoThr = true, tilt = false;
    double rangeDb = 0.0, thresholdDb = -20.0, attackKnob = 50.0, releaseKnob = 50.0, density = 50.0;
    double lo = 20.0, hi = 20000.0, centreHz = 1000.0;
    DesignInputs staticIn, dynIn;  // the band at its gain, and at gain + range
};

class SpectralStage {
public:
    bool prepare(double sampleRate, uint32_t maxFrames, int resolution, uint32_t padSamples);
    // In place on w (channels <= 2, n <= maxFrames). input: the plugin input of these samples
    // (internal side chain); sc / numSc: the external side-chain bus (may be null).
    void process(double* const* w, const double* const* input, const float* const* sc, uint32_t numSc,
                 uint32_t channels, uint32_t n,
                 const std::array<SpectralBandConfig, params::kBandCount>& cfg) noexcept;

    [[nodiscard]] int frameSize() const noexcept { return n_; }
    [[nodiscard]] int latency() const noexcept { return n_ + static_cast<int>(pad_); }
    [[nodiscard]] double grDb(int band) const noexcept;  // largest analysis-band gain reduction
    [[nodiscard]] int analysisBands(int band) const noexcept;
    [[nodiscard]] double analysisCentreHz(int band, int i) const noexcept;
    [[nodiscard]] uint64_t gridRebuilds() const noexcept { return rebuilds_; }

private:
    struct Analysis {
        dsp::BandDynamics core;
        dsp::ReferenceEnvelope ref;
        double gr = 0.0;
        std::array<double, 3> timing{-1, -1, -1};
    };
    struct BandState {
        bool was = false;
        dsp::SpectralGrid grid, oldGrid;
        std::array<double, 3> gridKey{-1, -1, -1};
        std::vector<Analysis> a, old;   // kSpectralMaxBands each
        std::vector<double> level;      // kSpectralMaxBands
        std::vector<float> relDb;       // n/2 + 1
        int relLo = 1, relHi = 0;       // the bins where |relDb| > 1e-9 dB (the apply loop's range)
        std::vector<int> binBand;       // n/2 + 1: the lower of the two bands weighting a bin
        std::vector<float> binWeight;   // n/2 + 1: its weight (the next band gets 1 - w)
        DesignInputs relStatic, relDyn;
        bool relValid = false;
        bool autoWas = false;
    };
    void frame(const std::array<SpectralBandConfig, params::kBandCount>& cfg, uint32_t channels, bool external) noexcept;
    void bandFrame(int b, const SpectralBandConfig& c) noexcept;

    double fs_ = 48000.0;
    int n_ = 0, hop_ = 0, bins_ = 0;
    uint32_t pad_ = 0;
    double magNorm_ = 1.0;  // |X| -> peak amplitude of a sine
    std::unique_ptr<dsp::RealFft> fft_;
    std::vector<float> win_;                       // sqrt-Hann, n
    std::array<std::vector<float>, 6> ring_;       // main L, R; input L, R; external L, R (n each)
    int pos_ = 0, hopFill_ = 0;
    std::array<std::vector<float>, 2> ola_, out_;  // overlap-add accumulator (n), ready output (hop)
    int outPos_ = 0;
    std::array<std::vector<double>, 2> padLine_;
    uint32_t padPos_ = 0;
    std::vector<float> frameBuf_;
    std::array<std::vector<std::complex<float>>, 6> spec_;  // spectra of the six rings
    std::array<std::vector<double>, 4> gainDb_;              // per bin: L, R, M, S
    std::vector<double> scMag_;                               // per bin: detector magnitude
    // Detector magnitudes are the same for every band with the same source and detect mode: computed
    // once per frame for each (source: internal / external) x (mode 0-4) that a band uses.
    std::array<std::vector<double>, 10> magCache_;
    std::array<bool, 10> magValid_{};
    const std::vector<double>& detectorMagnitudes(int src, int detect) noexcept;
    std::array<BandState, params::kBandCount> band_;
    uint64_t rebuilds_ = 0;
};

}  // namespace amq::engine
