#include "amq/engine/eq_match.hpp"

namespace amq::engine {

EqMatchStage::~EqMatchStage() {
    fileCancel_.store(true);
    cancel_.store(true);
    joinThreads();
}

void EqMatchStage::joinThreads() {
    if (fileThread_.joinable()) fileThread_.join();
    if (fitThread_.joinable()) fitThread_.join();
}

void EqMatchStage::prepare(double sampleRate, int highestFftSize) {
    fileCancel_.store(true);
    joinThreads();
    fileCancel_.store(false);
    const bool rateChanged = sampleRate != fs_ || highestFftSize != fftSize_;
    fs_ = sampleRate;
    fftSize_ = highestFftSize;
    if (rateChanged || input_.fftSize() == 0) {  // a rate change resets the captures (area 17 R10)
        input_.prepare(sampleRate, highestFftSize);
        reference_.prepare(sampleRate, highestFftSize);
    }
    lastCurvePos_ = pos_ = 0;
}

void EqMatchStage::onFrame(const analysis::SpectrumFrame& f) noexcept {
    if (f.fftSize != fftSize_ || !f.meanPower) return;
    pos_ = std::max(pos_, f.endPosition);
    if (f.source == analysis::Source::Pre) input_.addFrame(f.meanPower, f.bins);
    else if (f.source == analysis::Source::External && refSource_.load(std::memory_order_relaxed) == MatchReferenceSource::SideChain &&
             !fileBusy_.load(std::memory_order_acquire))
        reference_.addFrame(f.meanPower, f.bins);
}

void EqMatchStage::onPassEnd() noexcept {
    // At least 10 updates a second while frames arrive (area 20 R2): every 100 ms of audio.
    if (pos_ < lastCurvePos_ + static_cast<uint64_t>(0.1 * fs_)) return;
    lastCurvePos_ = pos_;
    publish();
}

void EqMatchStage::publish() noexcept {
    if (fileBusy_.load(std::memory_order_acquire)) return;  // the file thread owns the reference
    // The worker and the file thread may both publish: try-lock, never wait (the other one is
    // publishing the same state).
    if (publishing_.test_and_set(std::memory_order_acquire)) return;
    struct Release {
        std::atomic_flag& f;
        ~Release() { f.clear(std::memory_order_release); }
    } release{publishing_};
    auto& s = *scratch_;
    s.seq = ++seq_;
    s.inputReady = input_.ready();
    s.referenceReady = reference_.ready();
    s.inputRecording = input_.recording();
    s.referenceRecording = reference_.recording();
    s.inputSeconds = input_.countedSeconds();
    s.referenceSeconds = reference_.countedSeconds();
    s.inputSkipped = input_.skippedFrames();
    s.referenceSkipped = reference_.skippedFrames();
    analysis::MatchCurveSettings cs;
    cs.levelAlign = align_.load(std::memory_order_relaxed);
    cs.smoothing = smoothing_.load(std::memory_order_relaxed);
    analysis::computeMatchCurve(input_, reference_, cs, s.curve);
    snapshot_.write(s);
}

void EqMatchStage::setRecording(int capture, bool on) noexcept {
    (capture == 0 ? input_ : reference_).setRecording(on);
}

void EqMatchStage::reset(int capture) noexcept {
    auto& c = capture == 0 ? input_ : reference_;
    c.requestReset();
    if (capture == 1) c.setFileComplete(false);
}

void EqMatchStage::setWindow(analysis::MatchCapture::Window w, double seconds) noexcept {
    input_.setWindow(w, seconds);
    reference_.setWindow(w, seconds);
}

std::string EqMatchStage::fileError() const { return fileErrorSet_.load(std::memory_order_acquire) ? fileError_ : std::string(); }

bool EqMatchStage::loadReferenceFile(const std::string& path, std::string* error) {
    if (fileBusy_.load()) {
        if (error) *error = "A file is already being analysed.";
        return false;
    }
    if (fileThread_.joinable()) fileThread_.join();
    fileErrorSet_.store(false);
    fileProgress_.store(0.0f);
    refSource_.store(MatchReferenceSource::File);
    fileBusy_.store(true, std::memory_order_release);
    fileThread_ = std::thread([this, path] {
        analysis::AudioFile f;
        std::string err;
        if (!analysis::readWav(path, f, &err)) {
            fileError_ = err;
            fileErrorSet_.store(true, std::memory_order_release);
            fileBusy_.store(false, std::memory_order_release);
            return;
        }
        analysis::resample(f, fs_);  // to the session rate (area 17 R9)
        reference_.requestReset();
        reference_.setFileComplete(false);
        std::vector<const float*> ch;
        for (const auto& c : f.channels) ch.push_back(c.data());
        const bool ok = reference_.analyseSignal(ch.data(), static_cast<int>(ch.size()), f.frames(), &fileProgress_, &fileCancel_);
        reference_.setFileComplete(ok);
        if (!ok) {
            fileError_ = "File analysis cancelled.";
            fileErrorSet_.store(true, std::memory_order_release);
        }
        fileBusy_.store(false, std::memory_order_release);
        publish();
    });
    return true;
}

bool EqMatchStage::startFit(const FitSettings& base) {
    if (fitRunning_.load()) return false;
    MatchSnapshot s;
    if (!snapshot_.tryRead(s) || !s.curve.valid) return false;
    if (fitThread_.joinable()) fitThread_.join();
    fitCurve_ = s.curve;
    FitSettings fs = base;
    fs.sampleRate = s.curve.sampleRate;
    cancel_.store(false);
    progress_.store(0.0f);
    fitDone_.store(false);
    fitRunning_.store(true, std::memory_order_release);
    fitThread_ = std::thread([this, fs] {
        fitResult_ = fitMatch(fitCurve_.target, fitCurve_.weight, fs, &progress_, &cancel_);
        fitDone_.store(!fitResult_.cancelled, std::memory_order_release);
        fitRunning_.store(false, std::memory_order_release);
    });
    return true;
}

bool EqMatchStage::takeFit(FitResult& out, analysis::MatchCurve* fitted) {
    if (fitRunning_.load(std::memory_order_acquire) || !fitDone_.exchange(false, std::memory_order_acq_rel)) return false;
    if (fitThread_.joinable()) fitThread_.join();
    out = fitResult_;
    if (fitted) *fitted = fitCurve_;
    return true;
}

}  // namespace amq::engine
