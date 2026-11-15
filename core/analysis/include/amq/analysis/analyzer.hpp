#pragma once
// The analyzer engine core (area 19 R1-R4, R9; area 17 R1, R3, R4, R5, R6, R13).
//
// One engine serves the live display and, later, EQ Match. The audio thread only copies
// blocks into three lock-free taps (pre-EQ input, post-EQ output, external side chain). A
// worker thread reads the taps, cuts full frames with 75 % overlap (hop N/4), windows them,
// runs the FFT per channel and hands each frame to the consumers subscribed to that source and
// size. Frames are shared; consumers keep their own state (area 18 R10).
//
// FFT size follows the resolution (2048, 4096, 8192, 16384 at 44.1 and 48 kHz), times the
// power-of-two rate factor so bin width stays roughly constant (R2). Consumers ask for the
// display size or the highest size (EQ Match always uses the highest, R3); when the two are
// equal one framer serves both.
//
// Scaling (R4). A frame carries |X[k]|^2 per channel and their mean over channels (the power
// merge of area 17 R5), plus two factors:
//   - amplitude: (2 / sum w)^2. A sine of peak amplitude A centred on bin k reads A^2, so the
//     display shows a sine at its dBFS level.
//   - psd: 2 / (fs * sum w^2), the one-sided Welch power spectral density (per Hz). White
//     noise of variance s^2 reads 2 s^2 / fs on average.
//
// prepare(), subscribe(), start() and stop() run on the main thread with the worker stopped.
// write() is the audio thread's only call. runPending() is the worker's loop body; tests call
// it directly.

#include "amq/analysis/audio_tap.hpp"
#include "amq/dsp/fft.hpp"

#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace amq::analysis {

enum class Source : uint8_t { Pre = 0, Post = 1, External = 2 };
inline constexpr int kSourceCount = 3;
[[nodiscard]] constexpr uint8_t sourceBit(Source s) noexcept { return static_cast<uint8_t>(1u << static_cast<unsigned>(s)); }
inline constexpr uint8_t kAllSources = 0x7;

enum class SizeChoice : uint8_t { Display, Highest };
enum class AnalyzerWindow : uint8_t { BlackmanHarris, Kaiser };

inline constexpr int kResolutionCount = 4;        // 2048 .. 16384 at 44.1 / 48 kHz (R2)
inline constexpr int kDefaultResolution = 1;      // PROVISIONAL: 4096 at 48 kHz
inline constexpr double kAnalyzerKaiserBeta = 12.0;  // PROVISIONAL (Phase 0 window choice)
inline constexpr double kTapSeconds = 0.5;        // ring length; a stalled worker drops beyond it

// 1 up to 50 kHz, 2 up to 100 kHz, 4 up to 200 kHz, 8 above.
[[nodiscard]] int analyzerRateFactor(double sampleRate) noexcept;
[[nodiscard]] int analyzerFftSize(int resolution, double sampleRate) noexcept;

struct WindowScale {
    double sum = 0.0;         // sum of w[n] (coherent gain times N)
    double sumSquares = 0.0;  // sum of w[n]^2 (energy)
    double amplitude = 0.0;   // (2 / sum)^2
    double psd = 0.0;         // 2 / (fs * sumSquares)
};

struct SpectrumFrame {
    Source source = Source::Pre;
    int fftSize = 0;
    int bins = 0;  // fftSize / 2 + 1
    double sampleRate = 0.0;
    uint32_t channels = 0;
    const float* power[AudioTap::kChannels] = {};  // |X[k]|^2 per channel
    const float* meanPower = nullptr;              // mean of power over channels
    const WindowScale* scale = nullptr;
    uint64_t endPosition = 0;  // tap position just after the frame's last sample
    uint64_t index = 0;        // frames of this source and size since prepare
};

class FrameConsumer {
public:
    virtual ~FrameConsumer() = default;
    // Worker thread. Must not keep the pointers beyond the call.
    virtual void onFrame(const SpectrumFrame& f) noexcept = 0;
    // Worker thread, once per runPending() pass, after every frame of the pass.
    virtual void onPassEnd() noexcept {}
};

struct AnalyzerSettings {
    int displayResolution = kDefaultResolution;
    AnalyzerWindow window = AnalyzerWindow::BlackmanHarris;  // PROVISIONAL (Phase 0 choice)
};

class Analyzer {
public:
    Analyzer() = default;
    ~Analyzer() { stop(); }
    Analyzer(const Analyzer&) = delete;
    Analyzer& operator=(const Analyzer&) = delete;

    // ---- Main thread, worker stopped ----
    // Consumers stay registered across prepare(); call prepare() afterwards to build framers.
    void subscribe(FrameConsumer* c, uint8_t sourceMask, SizeChoice size);
    bool prepare(double sampleRate, const AnalyzerSettings& s = {});
    void start();
    void stop();
    [[nodiscard]] bool running() const noexcept { return thread_.joinable(); }

    // ---- Audio thread ----
    void write(Source s, const float* const* x, uint32_t channels, uint32_t frames) noexcept {
        taps_[static_cast<size_t>(s)].write(x, channels, frames);
    }

    // ---- Worker (or a test thread while the worker is stopped) ----
    // Reads every tap dry and emits the frames that became complete. Returns frames read.
    uint64_t runPending() noexcept;

    // ---- Any thread ----
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] const AnalyzerSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] int fftSize(SizeChoice c) const noexcept {
        return c == SizeChoice::Display ? displaySize_ : highestSize_;
    }
    [[nodiscard]] uint64_t frameCount(Source s) const noexcept {
        return frames_[static_cast<size_t>(s)].load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t droppedFrames(Source s) const noexcept {
        return taps_[static_cast<size_t>(s)].droppedFrames();
    }

private:
    struct Subscription {
        FrameConsumer* consumer;
        uint8_t mask;
        SizeChoice size;
    };
    struct Framer {
        int n = 0, hop = 0, fill = 0;
        std::unique_ptr<dsp::RealFft> fft;
        std::vector<float> window, windowed;
        std::array<std::vector<float>, AudioTap::kChannels> buf, power;
        std::vector<float> mean;
        std::vector<std::complex<float>> spec;
        WindowScale scale;
        std::vector<FrameConsumer*> consumers;
        uint64_t index = 0;
    };
    struct Feed {
        std::vector<std::unique_ptr<Framer>> framers;
        std::array<std::vector<float>, AudioTap::kChannels> chunk;
    };

    void push(Source s, Framer& f, const float* l, const float* r, uint32_t n, uint32_t channels,
              uint64_t endPosition) noexcept;
    void emit(Source s, Framer& f, uint32_t channels, uint64_t endPosition) noexcept;

    double sampleRate_ = 0.0;
    AnalyzerSettings settings_;
    int displaySize_ = 0, highestSize_ = 0;
    std::vector<Subscription> subs_;
    std::vector<FrameConsumer*> consumers_;  // distinct, for onPassEnd
    std::array<AudioTap, kSourceCount> taps_;
    std::array<Feed, kSourceCount> feeds_;
    std::array<std::atomic<uint64_t>, kSourceCount> frames_{};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace amq::analysis
