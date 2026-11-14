#pragma once
// Am-Q CLAP-native spike: one Bell band on core/dsp, CLAP via clap-helpers, wrapped to
// VST3 by clap-wrapper, Visage editor. Spike code: not production, not for core/.

#include "spike_params.h"

#include "amq/dsp/digital_design.hpp"

#include <clap/helpers/plugin.hh>

#include <array>
#include <atomic>
#include <memory>

namespace amq::spike {

class SpikeEditor;

// Host-contract checking. clap-helpers' thread checks call std::terminate() under Terminate,
// which kills the host (seen in Blue Cat's Axiom: VST3 allows getLatencySamples() off the main
// thread, clap-wrapper forwards it to latency.get, the main-thread check aborted the process).
// Under Ignore the checks still build a message and call host.log on the calling thread
// (allocation on the audio thread, not throttleable), so the default build turns them off.
// Configure with -DAMQ_STRICT_HOST_CHECKS=ON for development and contract testing.
#if defined(AMQ_STRICT_HOST_CHECKS) && AMQ_STRICT_HOST_CHECKS
using PluginBase = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                         clap::helpers::CheckingLevel::Maximal>;
#else
using PluginBase = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Ignore,
                                         clap::helpers::CheckingLevel::None>;
#endif

// Single-producer single-consumer queue for GUI -> audio parameter messages.
struct GuiMessage {
    enum Kind : uint8_t { kBegin, kValue, kEnd } kind;
    clap_id id;
    double value;
};

class SpscQueue {
public:
    bool push(const GuiMessage& m) noexcept {
        const auto h = head_.load(std::memory_order_relaxed);
        const auto next = (h + 1) % kSize;
        if (next == tail_.load(std::memory_order_acquire)) return false;
        buf_[h] = m;
        head_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(GuiMessage& m) noexcept {
        const auto t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        m = buf_[t];
        tail_.store((t + 1) % kSize, std::memory_order_release);
        return true;
    }

private:
    static constexpr size_t kSize = 1024;
    std::array<GuiMessage, kSize> buf_{};
    std::atomic<size_t> head_{0}, tail_{0};
};

class SpikePlugin : public PluginBase {
public:
    static const clap_plugin_descriptor descriptor;

    explicit SpikePlugin(const clap_host* host);
    ~SpikePlugin() override;

    // Called by the editor on the main thread.
    void guiBeginEdit(clap_id id);
    void guiSetValue(clap_id id, double value);
    void guiEndEdit(clap_id id);
    double uiValue(clap_id id) const { return ui_[id].load(std::memory_order_relaxed); }

protected:
    // clap_plugin
    bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames) noexcept override;
    void deactivate() noexcept override;
    void reset() noexcept override;
    clap_process_status process(const clap_process* process) noexcept override;
    void onMainThread() noexcept override;

    // audio ports
    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override { return 1; }
    bool audioPortsInfo(uint32_t index, bool isInput, clap_audio_port_info* info) const noexcept override;

    // params
    bool implementsParams() const noexcept override { return true; }
    uint32_t paramsCount() const noexcept override { return kParamCount; }
    bool paramsInfo(uint32_t index, clap_param_info* info) const noexcept override;
    bool paramsValue(clap_id id, double* value) noexcept override;
    bool paramsValueToText(clap_id id, double value, char* display, uint32_t size) noexcept override;
    bool paramsTextToValue(clap_id id, const char* display, double* value) noexcept override;
    void paramsFlush(const clap_input_events* in, const clap_output_events* out) noexcept override;

    // latency: any thread (wrapped VST3 hosts may ask from the audio or a worker thread)
    bool implementsLatency() const noexcept override { return true; }
    uint32_t latencyGet() const noexcept override { return activeLatency_.load(std::memory_order_relaxed); }

    // state
    bool implementsState() const noexcept override { return true; }
    bool stateSave(const clap_ostream* stream) noexcept override;
    bool stateLoad(const clap_istream* stream) noexcept override;

    // render
    bool implementsRender() const noexcept override { return true; }
    bool renderHasHardRealtimeRequirement() noexcept override { return false; }
    bool renderSetMode(clap_plugin_render_mode) noexcept override { return true; }

    // gui
    bool implementsGui() const noexcept override { return true; }
    bool guiIsApiSupported(const char* api, bool isFloating) noexcept override;
    bool guiGetPreferredApi(const char** api, bool* isFloating) noexcept override;
    bool guiCreate(const char* api, bool isFloating) noexcept override;
    void guiDestroy() noexcept override;
    bool guiSetParent(const clap_window* window) noexcept override;
    bool guiSetScale(double scale) noexcept override { return false; }
    bool guiShow() noexcept override { return true; }
    bool guiHide() noexcept override { return true; }
    bool guiCanResize() const noexcept override { return false; }
    bool guiGetSize(uint32_t* width, uint32_t* height) noexcept override;
    bool guiAdjustSize(uint32_t* width, uint32_t* height) noexcept override { return guiGetSize(width, height); }
    bool guiSetSize(uint32_t width, uint32_t height) noexcept override { return true; }

private:
    void applyParam(clap_id id, double value) noexcept;  // audio thread
    void handleEvent(const clap_event_header* ev) noexcept;
    void drainGuiQueue(const clap_output_events* out) noexcept;
    void redesign() noexcept;
    void render(const clap_process* p, uint32_t from, uint32_t to) noexcept;
    bool onMainThreadNow() const noexcept;  // true if the host can't tell

    double sampleRate_ = 48000.0;
    std::array<double, kParamCount> audio_{};               // audio-thread values
    std::array<std::atomic<double>, kParamCount> ui_{};     // main-thread mirror
    double outGainLin_ = 1.0;
    bool filterDirty_ = true;

    // One designed band (Bell, 12 dB/oct = one biquad). Biquad DF2T, double state.
    dsp::Biquad bq_{};
    double z1_[2] = {0, 0}, z2_[2] = {0, 0};

    // Dummy latency: a real 64-sample delay so the reported value is true.
    std::atomic<uint32_t> activeLatency_{0};  // written in activate(), read from any thread
    std::atomic<uint32_t> requestedLatency_{0};
    std::atomic<bool> stateDirty_{false};     // stateLoad -> audio thread: apply ui_ values
    std::atomic<bool> rescanPending_{false};  // stateLoad off main thread -> onMainThread
    std::array<std::array<float, kDummyLatency>, 2> delay_{};
    uint32_t delayPos_ = 0;

    SpscQueue guiQueue_;
    std::atomic<bool> uiDirty_{false};
    std::unique_ptr<SpikeEditor> editor_;
};

}  // namespace amq::spike
