#pragma once
// EQ Match capture (area 17 R1-R13; area 18 R1-R10; area 20 BS.1770 meter per capture).
//
// One capture (input or reference) accumulates long-term power on the match grid. It never stores
// audio, only accumulated spectra and counters (area 17 R13).
//
//   Frames: the analyzer's full frames at its highest resolution (75 % overlap, periodic 4-term
//     Blackman-Harris), channels merged by mean power (area 17 R3-R5); the live display keeps its own
//     state (area 18 R10). An audio-file reference is framed the same way here (analyseSignal).
//   Gate (area 18 R2): a frame counts only if its broadband level (mean power from 20 Hz to 20 kHz,
//     as dBFS of a sine's peak) is at or above -90 dBFS; skipped frames add nothing.
//   Grid (area 18 R6, R9): the match grid, 1/48 octave from 20 Hz to 20 kHz (480 points), fixed, not
//     tied to pixels. A point averages the power of the bins it covers; a point narrower than a bin
//     interpolates between the two nearest bins.
//   Windows (area 18 R3-R5): whole capture (one double sum and count per point) or the last N
//     seconds, 1 to 60 s, from a ring of 0.5 s blocks (old audio stops counting within one block).
//   Loudness (area 20 R3): each counted frame's K-weighted mean square (BS.1770 K-weighting applied
//     to the power spectrum, Parseval); frames are grouped into 400 ms blocks and gated like
//     BS.1770 (absolute -70 LUFS, relative -10 LU).
//   Ready (area 17 R8): at least 2 s of counted audio; a file reference once its analysis is done.
//
// Threading: prepare() on the main thread (allocates). addFrame(), analyseSignal() and the results
// on the analysis worker. The control setters are atomics, safe from the GUI thread.

#include "amq/dsp/fft.hpp"

#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

namespace amq::analysis {

inline constexpr int kMatchGridPoints = 480;
inline constexpr double kMatchGridMinHz = 20.0;
inline constexpr double kMatchPointsPerOctave = 48.0;
inline constexpr double kMatchGateDb = -90.0;
inline constexpr double kMatchReadySeconds = 2.0;  // planning value (area 17 R8)
inline constexpr double kMatchBlockSeconds = 0.5;   // last-N-seconds block (area 18 R4)
inline constexpr int kMatchMaxBlocks = 120;         // 60 s

[[nodiscard]] const std::array<double, kMatchGridPoints>& matchGrid() noexcept;

class MatchCapture {
public:
    enum class Window : uint8_t { Whole, LastSeconds };

    // Main thread. fftSize: the analyzer's highest size at this rate.
    void prepare(double sampleRate, int fftSize);

    // Control (any thread).
    void setRecording(bool on) noexcept { recording_.store(on, std::memory_order_relaxed); }
    [[nodiscard]] bool recording() const noexcept { return recording_.load(std::memory_order_relaxed); }
    void requestReset() noexcept { resetRequested_.store(true, std::memory_order_release); }
    void setWindow(Window w, double seconds) noexcept {
        window_.store(w, std::memory_order_relaxed);
        seconds_.store(seconds, std::memory_order_relaxed);
    }

    // Worker: one frame of |X[k]|^2 (mean over channels), bins = fftSize / 2 + 1.
    void addFrame(const float* meanPower, int bins) noexcept;
    // Worker: analyses audio (an audio-file reference, already at the capture's rate), framed like
    // the analyzer. progress (0..1) and cancel are optional. False if cancelled.
    bool analyseSignal(const float* const* channels, int channelCount, size_t frames,
                       std::atomic<float>* progress = nullptr, const std::atomic<bool>* cancel = nullptr);
    // Marks the capture as a finished file reference (ready regardless of duration).
    void setFileComplete(bool done) noexcept { fileComplete_.store(done, std::memory_order_release); }

    // Results (worker).
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] uint64_t countedFrames() const noexcept { return counted_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t skippedFrames() const noexcept { return skipped_.load(std::memory_order_relaxed); }
    [[nodiscard]] double countedSeconds() const noexcept;
    // Mean power per grid point in dB (window applied); false when nothing is counted.
    bool meanDb(std::array<double, kMatchGridPoints>& out) const noexcept;
    // Integrated loudness of the counted frames (LUFS, -inf when nothing passes the gates).
    [[nodiscard]] double loudnessLufs() const noexcept;
    [[nodiscard]] double sampleRate() const noexcept { return fs_; }
    [[nodiscard]] int fftSize() const noexcept { return n_; }

private:
    void doReset() noexcept;
    struct Point {
        int lo = 0, hi = -1;  // covered bins, inclusive; hi < lo: interpolate between k0, k0 + 1
        int k0 = 0;
        double frac = 0.0;
    };

    double fs_ = 48000.0;
    int n_ = 0, hop_ = 0;
    double binPower_ = 1.0;  // |X|^2 -> mean-square share of a bin: 2 / (N sum w^2)
    std::array<Point, kMatchGridPoints> map_{};
    int band0_ = 1, band1_ = 1;  // bins of 20 Hz .. 20 kHz (gate)
    std::vector<double> kWeight_;  // |K(f_k)|^2 per bin

    std::vector<double> sum_;       // whole capture: per point
    std::vector<double> blocks_;    // last N seconds: kMatchMaxBlocks x points
    std::vector<uint32_t> blockCount_;
    int block_ = 0, framesPerBlock_ = 1;
    uint32_t inBlock_ = 0;
    std::vector<double> loudBlocks_;  // 400 ms groups of K-weighted mean squares
    double loudAcc_ = 0.0;
    int loudFill_ = 0, framesPer400_ = 1;

    std::atomic<uint64_t> counted_{0}, skipped_{0};
    std::atomic<bool> recording_{false}, resetRequested_{false}, fileComplete_{false};
    std::atomic<Window> window_{Window::Whole};
    std::atomic<double> seconds_{10.0};

    // File analysis framing.
    std::unique_ptr<dsp::RealFft> fft_;
    std::vector<float> win_, frame_;
    std::vector<std::complex<float>> spec_;
    std::vector<float> power_;
};

}  // namespace amq::analysis
