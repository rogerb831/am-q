#pragma once
// EQ Match coordinator (areas 16-20; area 19 task "Hook the EQ Match accumulators into the engine at
// highest resolution").
//
// Subscribed to the analyzer at its highest resolution for the Pre (input) and External (side-chain)
// sources: the input capture takes Pre frames, the reference capture takes side-chain frames when its
// source is Side Chain. A file reference is decoded, resampled and analysed on its own thread with
// the same framing (area 17 R9), while live reference frames are ignored. On the analysis worker the
// difference curve is rebuilt at least 10 times a second while capturing (area 20 R2) and published as
// one immutable snapshot (R8). Match copies the latest snapshot's curve and fits it on a fit thread
// with progress and cancel (area 16 R1, R16); the result waits in a mailbox for the GUI. A rate
// change (prepare at another rate) resets the live captures (area 17 R10).
//
// Threading: prepare() on the main thread with the analyzer stopped; onFrame/onPassEnd on the analysis
// worker; the controls and status from the GUI thread (atomics and sequence slots only).

#include "amq/analysis/analyzer.hpp"
#include "amq/analysis/audio_file.hpp"
#include "amq/analysis/match_curve.hpp"
#include "amq/engine/design_worker.hpp"
#include "amq/engine/eq_match_fit.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace amq::engine {

enum class MatchReferenceSource : uint8_t { SideChain, File };

struct MatchSnapshot {
    uint64_t seq = 0;
    bool inputReady = false, referenceReady = false;
    bool inputRecording = false, referenceRecording = false;
    double inputSeconds = 0.0, referenceSeconds = 0.0;
    uint64_t inputSkipped = 0, referenceSkipped = 0;
    analysis::MatchCurve curve;  // curve.valid when both captures are ready
};

class EqMatchStage final : public analysis::FrameConsumer {
public:
    EqMatchStage() = default;
    ~EqMatchStage() override;
    EqMatchStage(const EqMatchStage&) = delete;
    EqMatchStage& operator=(const EqMatchStage&) = delete;

    void prepare(double sampleRate, int highestFftSize);

    // analysis::FrameConsumer (analysis worker).
    void onFrame(const analysis::SpectrumFrame& f) noexcept override;
    void onPassEnd() noexcept override;

    // Controls (GUI thread). capture: 0 input, 1 reference.
    void setRecording(int capture, bool on) noexcept;
    void reset(int capture) noexcept;
    void setReferenceSource(MatchReferenceSource s) noexcept { refSource_.store(s, std::memory_order_relaxed); }
    [[nodiscard]] MatchReferenceSource referenceSource() const noexcept { return refSource_.load(std::memory_order_relaxed); }
    void setWindow(analysis::MatchCapture::Window w, double seconds) noexcept;
    void setSmoothing(analysis::MatchSmoothing s) noexcept { smoothing_.store(s, std::memory_order_relaxed); }
    void setLevelAlign(bool on) noexcept { align_.store(on, std::memory_order_relaxed); }

    // Status (GUI thread): the latest snapshot, if newer than seq.
    bool latest(MatchSnapshot& out) const noexcept { return snapshot_.tryRead(out); }

    // File reference: decodes, resamples and analyses on a thread. False with a message if it cannot
    // start (another file is being analysed).
    bool loadReferenceFile(const std::string& path, std::string* error);
    [[nodiscard]] float fileProgress() const noexcept { return fileProgress_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool fileBusy() const noexcept { return fileBusy_.load(std::memory_order_acquire); }
    [[nodiscard]] std::string fileError() const;

    // Fit (GUI thread). startFit takes the latest valid curve; false if none (area 16 R15).
    bool startFit(const FitSettings& base = {});
    void cancelFit() noexcept { cancel_.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool fitRunning() const noexcept { return fitRunning_.load(std::memory_order_acquire); }
    [[nodiscard]] float fitProgress() const noexcept { return progress_.load(std::memory_order_relaxed); }
    // The finished fit, once (and the curve it fitted); false if none is waiting.
    bool takeFit(FitResult& out, analysis::MatchCurve* fitted = nullptr);

    // Tests: build and publish the curve now (Manual mode).
    void updateNowForTests() noexcept { publish(); }
    [[nodiscard]] analysis::MatchCapture& input() noexcept { return input_; }
    [[nodiscard]] analysis::MatchCapture& reference() noexcept { return reference_; }

private:
    void publish() noexcept;
    void joinThreads();

    analysis::MatchCapture input_, reference_;
    double fs_ = 48000.0;
    int fftSize_ = 0;
    std::atomic<MatchReferenceSource> refSource_{MatchReferenceSource::SideChain};
    std::atomic<analysis::MatchSmoothing> smoothing_{analysis::MatchSmoothing::Sixth};
    std::atomic<bool> align_{true};
    uint64_t lastCurvePos_ = 0, pos_ = 0, seq_ = 0;
    std::unique_ptr<MatchSnapshot> scratch_ = std::make_unique<MatchSnapshot>();
    SeqSlot<MatchSnapshot> snapshot_;
    std::atomic_flag publishing_ = ATOMIC_FLAG_INIT;  // one writer at a time; a busy publish is skipped

    std::thread fileThread_;
    std::atomic<bool> fileBusy_{false}, fileCancel_{false};
    std::atomic<float> fileProgress_{0.0f};
    std::string fileError_;
    std::atomic<bool> fileErrorSet_{false};

    std::thread fitThread_;
    std::atomic<bool> fitRunning_{false}, fitDone_{false}, cancel_{false};
    std::atomic<float> progress_{0.0f};
    FitResult fitResult_;
    analysis::MatchCurve fitCurve_;
};

}  // namespace amq::engine
