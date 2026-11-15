#include "amq/engine/design_worker.hpp"

#include "amq/engine/band_response.hpp"

#include "amq/params/mappings.hpp"

#include <chrono>
#include <cmath>

namespace amq::engine {

bool designRefinedCascade(const DesignInputs& in, dsp::SvfCascade& out) {
    const auto proto = dsp::designAnalogPrototype(bandSpecFrom(in));
    if (!proto.supported) return false;
    // The area 01 default: joint-refined magnitude match with the conditional correction
    // section. All Pass keeps its bilinear design (area 02 R15).
    const auto d = dsp::designDigital(proto, in.sampleRate, dsp::DesignMethod::MagnitudeMatched);
    dsp::SvfCascade c;
    for (const auto& b : d.sections) {
        dsp::SvfParams p;
        if (!dsp::svfFromBiquad(b, p)) return false;
        c.sections.push_back(p);
    }
    if (c.sections.overflowed() || c.sections.empty()) return false;
    out = c;
    return true;
}

int DesignWorker::runPending() {
    int count = 0;
    for (int b = 0; b < params::kBandCount; ++b) {
        DesignRequest r;
        if (!requests_[static_cast<size_t>(b)].tryRead(r) || r.id == 0 || r.id == done_[static_cast<size_t>(b)]) continue;
        DesignResult res;
        res.id = r.id;
        res.ok = designRefinedCascade(r.in, res.cascade);
        results_[static_cast<size_t>(b)].write(res);
        readyIds_[static_cast<size_t>(b)].store(res.id, std::memory_order_release);
        done_[static_cast<size_t>(b)] = r.id;
        ++count;
    }
    return count;
}

void DesignWorker::start() {
    if (thread_.joinable()) return;
    stop_.store(false);
    thread_ = std::thread([this] {
        while (!stop_.load(std::memory_order_relaxed)) {
            // Polling keeps the audio side to plain atomic stores (no wake-up call).
            if (runPending() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
}

void DesignWorker::stop() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    thread_.join();
}

}  // namespace amq::engine
