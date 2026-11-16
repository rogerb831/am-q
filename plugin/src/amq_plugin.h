#pragma once
// Am-Q CLAP plugin (Phase 1 skeleton). CLAP-native on clap-helpers; clap-wrapper builds the
// VST3 from the same static library (docs/plan/decisions.md D1).
//
// Threading rule (area 01): every callback a VST3 host can reach through clap-wrapper may be
// called on any thread. Such callbacks read only atomics or constant data, and anything that
// must happen on the main thread (host notifications, editor updates) is deferred to
// onMainThread().
//
// Scope so far: the full parameter table exported to the host (core/params), host automation
// written through the setter, a versioned id/value state chunk, stereo pass-through, zero
// latency and the editor shell. Block splitting with ramps (area 01, 09) and the DSP come next.

#include "host_checks.h"

#include "amq/engine/engine.hpp"
#include "amq/ui/undo_controller.hpp"
#include "amq/ui/edit_controller.hpp"
#include "gui_events.h"
#include "amq/params/parameter_table.hpp"

#include <vector>

#include <atomic>
#include <cstdint>
#include <memory>

namespace amq::ui {
class Editor;
}

namespace amq::plugin {

class AmqPlugin : public PluginBase, public ui::EditController {
public:
    static const clap_plugin_descriptor descriptor;

    explicit AmqPlugin(const clap_host* host);
    ~AmqPlugin() override;

    // ui::EditController (main thread): the editor's route to parameters and the host.
    const params::ParameterTable& table() const noexcept override { return table_; }
    void addListener(params::ParameterListener* l) override { table_.addListener(l); }
    void removeListener(params::ParameterListener* l) override { table_.removeListener(l); }
    void dispatchNotifications() override { table_.dispatchNotifications(); }
    const analysis::LevelSnapshot* pullLevels() noexcept override { return engine_.snapshots().pullLevels(); }
    const analysis::GainChangeSnapshot* pullGainChange() noexcept override { return engine_.snapshots().pullGainChange(); }
    const analysis::SpectrumSnapshot* pullSpectrum() noexcept override { return engine_.snapshots().pullSpectrum(); }
    void beginEdit(params::ParamIndex index) override;
    void performEdit(params::ParamIndex index, double plain) override;
    void endEdit(params::ParamIndex index) override;

protected:
    // clap_plugin
    bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames) noexcept override;
    void deactivate() noexcept override;
    void reset() noexcept override;
    clap_process_status process(const clap_process* process) noexcept override;
    void onMainThread() noexcept override;

    // audio ports: any thread (constant data)
    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override;
    bool audioPortsInfo(uint32_t index, bool isInput, clap_audio_port_info* info) const noexcept override;

    // params: count, info, value and text conversion on any thread (constant specs, atomics);
    // flush on the main or audio thread, never concurrently with process (CLAP)
    bool implementsParams() const noexcept override { return true; }
    uint32_t paramsCount() const noexcept override { return params::kParamCount; }
    bool paramsInfo(uint32_t index, clap_param_info* info) const noexcept override;
    bool paramsValue(clap_id id, double* value) noexcept override;
    bool paramsValueToText(clap_id id, double value, char* display, uint32_t size) noexcept override;
    bool paramsTextToValue(clap_id id, const char* display, double* value) noexcept override;
    void paramsFlush(const clap_input_events* in, const clap_output_events* out) noexcept override;

    // latency: any thread
    bool implementsLatency() const noexcept override { return true; }
    uint32_t latencyGet() const noexcept override { return latency_.load(std::memory_order_relaxed); }

    // state: any thread
    bool startProcessing() noexcept override {
        processing_.store(true);
        return true;
    }
    void stopProcessing() noexcept override {
        processing_.store(false);
        applyPendingState();  // a load that arrived while stopping is applied now
    }
    bool implementsState() const noexcept override { return true; }
    bool stateSave(const clap_ostream* stream) noexcept override;
    bool stateLoad(const clap_istream* stream) noexcept override;

    // render: offline renders use the refined designs at once, so they are exact and repeatable
    bool implementsRender() const noexcept override { return true; }
    bool renderHasHardRealtimeRequirement() noexcept override { return false; }
    bool renderSetMode(clap_plugin_render_mode mode) noexcept override {
        engine_.setOfflineRender(mode == CLAP_RENDER_OFFLINE);
        return true;
    }

    // gui: create/destroy/parent on the main thread; size queries on any thread
    bool implementsGui() const noexcept override { return true; }
    bool guiIsApiSupported(const char* api, bool isFloating) noexcept override;
    bool guiGetPreferredApi(const char** api, bool* isFloating) noexcept override;
    bool guiCreate(const char* api, bool isFloating) noexcept override;
    void guiDestroy() noexcept override;
    bool guiSetParent(const clap_window* window) noexcept override;
    // Declined: Visage reads the per-monitor DPI itself on Windows; macOS sizes are points.
    bool guiSetScale(double) noexcept override { return false; }
    bool guiShow() noexcept override { return true; }
    bool guiHide() noexcept override { return true; }
    bool guiCanResize() const noexcept override { return true; }
    bool guiGetResizeHints(clap_gui_resize_hints_t* hints) noexcept override;
    bool guiGetSize(uint32_t* width, uint32_t* height) noexcept override;
    bool guiAdjustSize(uint32_t* width, uint32_t* height) noexcept override;
    bool guiSetSize(uint32_t width, uint32_t height) noexcept override;

private:
    bool onMainThreadNow() const noexcept;  // true if the host can't tell
    void applyEvent(const clap_event_header* ev) noexcept;
    void drainGuiEvents(const clap_output_events* out) noexcept;  // process() / flush()
    void requestGuiRefresh() noexcept;                             // audio thread
    void requestHostFlush() noexcept;                              // main thread
    void requestRestartIfNeeded(bool processing) noexcept;         // any thread
    void requestTextRescanIfNeeded() noexcept;                     // any thread
    void applyPendingState() noexcept;                             // audio thread, or any while not processing
    [[nodiscard]] double currentValue(params::ParamIndex i) const noexcept;  // a pending state load counts
    void rescanTextIfNeeded() noexcept;                            // main thread

    params::ParameterTable table_;
    engine::Engine engine_{table_};
    std::vector<float> scratch_;              // 64-bit I/O conversion: 2 in, 2 out, 2 side chain
    uint32_t maxFrames_ = 0;
    std::atomic<bool> snapPending_{false};    // state loaded: jump ramps at the next block
    std::atomic<bool> rescanPending_{false};  // state loaded off the main thread
    // Batch state load (area 09 R15, area 21 R14): a load writes every value here; the audio thread
    // applies them all at the start of its next block (or the loading thread at once while not
    // processing), so no block ever runs on half a preset.
    struct StateBatch {
        std::array<double, params::kParamCount> v{};
    };
    engine::SeqSlot<StateBatch> pendingState_;
    std::atomic<bool> statePending_{false};
    std::atomic<bool> processing_{false};
    GuiEventQueue guiEvents_;                 // editor gestures -> host
    std::atomic<bool> editorOpen_{false};
    std::atomic<bool> refreshRequested_{false};
    std::atomic<uint32_t> latency_{0};        // written on the main thread in activate()
    std::atomic<bool> restartRequested_{false};  // one request_restart per mode change
    std::atomic<bool> textPanMidSide_{false};    // Pan Mode the host's value text was last rescanned for
    std::atomic<bool> textCallbackRequested_{false};
    // Undo (area 12): the editor edits through this wrapper, so GUI edits are undoable; host
    // automation and state loads are not recorded, and a state load clears the history.
    ui::UndoController undo_{*this};
    std::atomic<bool> undoClearPending_{false};
    std::unique_ptr<ui::Editor> editor_;
    // Editor size, for size queries off the main thread and for reopening at the same size.
    // Written on the main thread after every editor layout.
    std::atomic<uint32_t> guiHostW_{1180}, guiHostH_{672};  // = ui::Editor defaults (checked in the .cpp)
    std::atomic<double> guiScale_{1.0};
    uint32_t guiLogicalW_ = 1180, guiLogicalH_ = 672;  // main thread
};

}  // namespace amq::plugin
