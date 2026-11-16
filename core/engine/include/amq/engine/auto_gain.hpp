#pragma once
// Auto Gain stage (area 03 R6-R9; area 05 R9-R13; area 04 R12, R14 caches).
//
// The engine posts the active bands' design inputs (the same request as Natural Phase; gains after
// Gain Scale, R6) whenever a band changes, at most every 10 ms. A worker evaluates each band's
// analog response on the shared 512-point log grid (log_grid.hpp) through a per-band cache, so a
// one-band edit recomputes one band (area 03 caches task, R14 grid). Per grid point it forms the
// combined 2x2 response (L/R stage, then M/S, as in area 03) and the output power of each channel
// for uncorrelated, equal-power left and right inputs; the make-up gain follows dsp/auto_gain.hpp.
// The result is published through atomics; the engine ramps to it (R8) and applies it in the output
// node before Level (area 03 design). Off, the gain is exactly 1 (R7).

#include "amq/dsp/log_grid.hpp"
#include "amq/engine/natural_phase.hpp"

#include <array>
#include <atomic>
#include <complex>
#include <memory>
#include <thread>

namespace amq::engine {

inline constexpr double kAutoGainRampMs = 50.0;  // PROVISIONAL: make-up gain smoothing (whole length)

class AutoGainStage {
public:
    AutoGainStage();
    ~AutoGainStage() { stopWorker(); }
    AutoGainStage(const AutoGainStage&) = delete;
    AutoGainStage& operator=(const AutoGainStage&) = delete;

    void prepare();  // main thread: clears caches and results
    void startWorker();
    void stopWorker();
    void post(const CorrectionRequest& r) noexcept { request_.write(r); }
    int runPending();  // worker or test thread
    // Computes a request now on the calling thread (offline rendering; tests). Returns the gain in dB.
    double computeNow(const CorrectionRequest& r) noexcept;

    // Audio thread: the latest result, if newer than `seen`.
    bool result(uint64_t& seen, double& db) const noexcept {
        const uint64_t id = id_.load(std::memory_order_acquire);
        if (id == seen) return false;
        db = db_.load(std::memory_order_relaxed);
        seen = id;
        return true;
    }
    [[nodiscard]] uint64_t recomputes() const noexcept { return recomputes_.load(std::memory_order_relaxed); }

private:
    struct Entry {
        bool valid = false;
        DesignInputs in;
        std::array<std::complex<double>, dsp::kLogGridPoints> h{};
    };
    double compute(const CorrectionRequest& r, std::array<Entry, params::kBandCount>& cache) noexcept;

    std::unique_ptr<std::array<Entry, params::kBandCount>> workerCache_, audioCache_;
    std::unique_ptr<CorrectionRequest> workerRequest_;
    SeqSlot<CorrectionRequest> request_;
    uint64_t builtId_ = 0;
    std::atomic<uint64_t> id_{0};
    std::atomic<double> db_{0.0};
    std::atomic<uint64_t> recomputes_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace amq::engine
