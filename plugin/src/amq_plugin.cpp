#include "amq_plugin.h"

#include "amq/ui/editor.h"

#include <clap/helpers/host-proxy.hxx>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE__)
#include <xmmintrin.h>
#define AMQ_HAS_SSE 1
#endif

namespace amq::plugin {

namespace {

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_EQUALIZER,
                                 CLAP_PLUGIN_FEATURE_STEREO, nullptr};

// State chunk (Parameter model and state; docs/params/parameter-table.md):
//   u32 magic "AMQS", u32 version, u32 pair count, then per pair u32 host id + f64 plain value.
// Little-endian. Version 1 was the skeleton (no pairs); version 2 adds the pairs. On load,
// unknown ids are ignored and missing ids take their defaults, so old sessions keep loading.
constexpr uint32_t kStateMagic = 0x53514D41;  // "AMQS"
constexpr uint32_t kStateVersion = 2;
constexpr size_t kHeaderBytes = 12;
constexpr size_t kPairBytes = 12;

void putU32(uint8_t* b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t getU32(const uint8_t* b) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b[i]) << (8 * i);
    return v;
}
void putF64(uint8_t* b, double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(u >> (8 * i));
}
double getF64(const uint8_t* b) {
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u |= static_cast<uint64_t>(b[i]) << (8 * i);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}

bool writeAll(const clap_ostream* s, const uint8_t* data, size_t n) {
    size_t done = 0;
    while (done < n) {
        const auto r = s->write(s, data + done, n - done);
        if (r <= 0) return false;
        done += static_cast<size_t>(r);
    }
    return true;
}
bool readAll(const clap_istream* s, uint8_t* data, size_t n) {
    size_t done = 0;
    while (done < n) {
        const auto r = s->read(s, data + done, n - done);
        if (r <= 0) return false;
        done += static_cast<size_t>(r);
    }
    return true;
}

void copyName(char* dst, size_t cap, const std::string& src) { std::snprintf(dst, cap, "%s", src.c_str()); }

// Flush denormals to zero for the duration of process() (restored on return).
struct FtzGuard {
#if AMQ_HAS_SSE
    unsigned int saved = _mm_getcsr();
    FtzGuard() { _mm_setcsr(saved | 0x8040); }  // FTZ (bit 15) | DAZ (bit 6)
    ~FtzGuard() { _mm_setcsr(saved); }
#endif
};

}  // namespace

// PROVISIONAL: the plugin id must never change after release. Product naming is an open
// decision (docs/plan/README.md), so this id and the names below are placeholders until then.
const clap_plugin_descriptor AmqPlugin::descriptor = {
    CLAP_VERSION,
    "dev.am-q.eq",
    "Am-Q",
    "Am-Q",
    "",
    "",
    "",
    "0.0.1",
    "24-band parametric EQ",
    kFeatures,
};

AmqPlugin::AmqPlugin(const clap_host* host) : PluginBase(&descriptor, host) {
    // Refined band designs (R12) come from a worker thread started in activate().
    engine_.setDesignMode(engine::DesignMode::Thread);
}

AmqPlugin::~AmqPlugin() = default;

bool AmqPlugin::onMainThreadNow() const noexcept {
    return !_host.canUseThreadCheck() || _host.isMainThread();
}

// ---------------------------------------------------------------------------
// Lifecycle and audio
// ---------------------------------------------------------------------------

bool AmqPlugin::activate(double sampleRate, uint32_t, uint32_t maxFrames) noexcept {
    // Main thread: allocation is allowed here, never in process().
    maxFrames_ = maxFrames;
    try {
        scratch_.assign(static_cast<size_t>(maxFrames) * 6, 0.0f);
    } catch (...) {
        return false;
    }
    engine_.prepare(sampleRate, maxFrames);  // unsupported rates give safe silence (area 02 R3)
    snapPending_.store(false);
    restartRequested_.store(false);
    // Latency may only change during activate (CLAP latency extension). The engine latches the
    // processing mode in prepare() and reports the latency of what it runs (area 02 R9, R12).
    const uint32_t wanted = engine_.latencySamples();
    if (latency_.exchange(wanted) != wanted && _host.canUseLatency()) _host.latencyChanged();
    return true;
}

// A processing-mode change changes the latency, which may only be reported during activate:
// ask the host to deactivate and reactivate the plugin (area 02 R13). clap-wrapper turns this
// into restartComponent(kLatencyChanged) for VST3. request_restart is thread-safe.
// PROVISIONAL: no fade out before the restart yet (area 02 R13 fades).
void AmqPlugin::requestRestartIfNeeded() noexcept {
    if (engine_.restartNeeded() && !restartRequested_.exchange(true)) _host.requestRestart();
}

void AmqPlugin::deactivate() noexcept { engine_.release(); }

void AmqPlugin::reset() noexcept {}

clap_process_status AmqPlugin::process(const clap_process* p) noexcept {
    FtzGuard ftz;
    if (snapPending_.exchange(false)) engine_.snapToTable();  // restored state: no ramps (area 09 R15)
    drainGuiEvents(p->out_events);

    const uint32_t n = p->frames_count;
    const clap_input_events* evs = p->in_events;
    const uint32_t ne = evs ? evs->size(evs) : 0;

    // Channel pointers. 64-bit host buffers are converted to float at this edge (area 01 R4).
    const float* inCh[engine::kMaxChannels] = {};
    float* outCh[engine::kMaxChannels] = {};
    uint32_t numIn = 0, numOut = 0;
    double* const* out64 = nullptr;
    const bool fits = n <= maxFrames_;
    if (p->audio_inputs_count >= 1) {
        const auto& b = p->audio_inputs[0];
        numIn = std::min(b.channel_count, engine::kMaxChannels);
        for (uint32_t c = 0; c < numIn; ++c) {
            if (b.data32) {
                inCh[c] = b.data32[c];
            } else if (b.data64 && fits) {
                float* s = scratch_.data() + static_cast<size_t>(c) * maxFrames_;
                for (uint32_t i = 0; i < n; ++i) s[i] = static_cast<float>(b.data64[c][i]);
                inCh[c] = s;
            }
        }
        if (numIn && !inCh[0]) numIn = 0;
    }
    if (p->audio_outputs_count >= 1) {
        const auto& b = p->audio_outputs[0];
        numOut = std::min(b.channel_count, engine::kMaxChannels);
        for (uint32_t c = 0; c < numOut; ++c) {
            if (b.data32) {
                outCh[c] = b.data32[c];
            } else if (b.data64 && fits) {
                outCh[c] = scratch_.data() + static_cast<size_t>(2 + c) * maxFrames_;
                out64 = b.data64;
            }
        }
        if (numOut && !outCh[0]) numOut = 0;
    }
    // Side-chain bus (input port 1, area 01 R2). Hosts that feed nothing give no channels or
    // null buffers; then the engine gets no side chain. Never taken from extra main inputs.
    const float* scCh[engine::kMaxChannels] = {};
    uint32_t numSc = 0;
    if (p->audio_inputs_count >= 2) {
        const auto& b = p->audio_inputs[1];
        numSc = std::min(b.channel_count, engine::kMaxChannels);
        for (uint32_t c = 0; c < numSc; ++c) {
            if (b.data32) {
                scCh[c] = b.data32[c];
            } else if (b.data64 && b.data64[c] && fits) {
                float* s = scratch_.data() + static_cast<size_t>(4 + c) * maxFrames_;
                for (uint32_t i = 0; i < n; ++i) s[i] = static_cast<float>(b.data64[c][i]);
                scCh[c] = s;
            }
        }
        if (numSc && !scCh[0]) numSc = 0;
        if (numSc == 2 && !scCh[1]) numSc = 1;
    }

    // Sample-accurate changes (area 01 R3): split the block at every event offset. Events at
    // or before the piece start are written to the table first; the engine retargets its
    // ramps at the start of each piece.
    uint32_t pos = 0, ei = 0;
    while (pos < n) {
        while (ei < ne) {
            const clap_event_header* ev = evs->get(evs, ei);
            if (ev->time > pos) break;
            applyEvent(ev);
            ++ei;
        }
        uint32_t next = n;
        if (ei < ne) next = std::min(n, evs->get(evs, ei)->time);
        if (numOut) {
            const float* ip[engine::kMaxChannels] = {};
            const float* sp[engine::kMaxChannels] = {};
            float* op[engine::kMaxChannels] = {};
            for (uint32_t c = 0; c < numIn; ++c) ip[c] = inCh[c] + pos;
            for (uint32_t c = 0; c < numSc; ++c) sp[c] = scCh[c] + pos;
            for (uint32_t c = 0; c < numOut; ++c) op[c] = outCh[c] + pos;
            engine_.process(numIn ? ip : nullptr, numIn, op, numOut, next - pos, numSc ? sp : nullptr, numSc);
        }
        pos = next;
    }
    while (ei < ne) applyEvent(evs->get(evs, ei++));  // events at or after the block end
    if (ne) requestGuiRefresh();
    requestRestartIfNeeded();

    if (out64) {
        for (uint32_t c = 0; c < numOut; ++c)
            for (uint32_t i = 0; i < n; ++i) out64[c][i] = static_cast<double>(outCh[c][i]);
    }
    // Extra output channels (none with the stereo port) and outputs we could not fill are silent.
    if (p->audio_outputs_count >= 1) {
        const auto& b = p->audio_outputs[0];
        for (uint32_t c = numOut; c < b.channel_count; ++c) {
            if (b.data32 && b.data32[c]) std::fill(b.data32[c], b.data32[c] + n, 0.0f);
            else if (b.data64 && b.data64[c]) std::fill(b.data64[c], b.data64[c] + n, 0.0);
        }
    }
    return CLAP_PROCESS_CONTINUE;
}

void AmqPlugin::onMainThread() noexcept {
    refreshRequested_.store(false);
    if (rescanPending_.exchange(false) && _host.canUseParams()) _host.paramsRescan(CLAP_PARAM_RESCAN_VALUES);
    table_.dispatchNotifications();  // editor listeners, once the editor binds controls
}

// ---------------------------------------------------------------------------
// Parameters (core/params; docs/params/parameter-table.md)
// ---------------------------------------------------------------------------

void AmqPlugin::applyEvent(const clap_event_header* ev) noexcept {
    if (!ev || ev->space_id != CLAP_CORE_EVENT_SPACE_ID || ev->type != CLAP_EVENT_PARAM_VALUE) return;
    const auto* pv = reinterpret_cast<const clap_event_param_value*>(ev);
    if (const auto idx = params::findByHostId(pv->param_id))
        table_.setFromHost(*idx, pv->value, params::ParamSource::Host);
}

bool AmqPlugin::paramsInfo(uint32_t index, clap_param_info* info) const noexcept {
    const params::ParamSpec* s = params::specAt(index);
    if (!s || !info) return false;
    std::memset(info, 0, sizeof(*info));
    info->id = s->hostId;
    clap_param_info_flags f = 0;
    if (s->automatable) f |= CLAP_PARAM_IS_AUTOMATABLE;
    if (s->type != params::ParamType::Continuous) f |= CLAP_PARAM_IS_STEPPED;
    if (s->type == params::ParamType::Choice) f |= CLAP_PARAM_IS_ENUM;
    if (s->isBypass) f |= CLAP_PARAM_IS_BYPASS;
    info->flags = f;
    info->min_value = params::hostMin(*s);
    info->max_value = params::hostMax(*s);
    info->default_value = params::toHost(*s, s->def);
    copyName(info->name, sizeof(info->name), s->name);
    copyName(info->module, sizeof(info->module), s->group);
    return true;
}

bool AmqPlugin::paramsValue(clap_id id, double* value) noexcept {
    const auto idx = params::findByHostId(id);
    if (!idx || !value) return false;
    *value = params::toHost(*params::specAt(*idx), table_.value(*idx));
    return true;
}

bool AmqPlugin::paramsValueToText(clap_id id, double value, char* display, uint32_t size) noexcept {
    const auto idx = params::findByHostId(id);
    if (!idx || !display || size == 0) return false;
    const auto& s = *params::specAt(*idx);
    copyName(display, size, params::formatPlain(s, params::fromHost(s, value)));
    return true;
}

bool AmqPlugin::paramsTextToValue(clap_id id, const char* display, double* value) noexcept {
    const auto idx = params::findByHostId(id);
    if (!idx || !display || !value) return false;
    const auto& s = *params::specAt(*idx);
    const auto plain = params::parsePlain(s, display);
    if (!plain) return false;
    *value = params::toHost(s, *plain);
    return true;
}

void AmqPlugin::paramsFlush(const clap_input_events* in, const clap_output_events* out) noexcept {
    drainGuiEvents(out);
    if (!in) return;
    const uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; ++i) applyEvent(in->get(in, i));
    if (n) requestGuiRefresh();
    requestRestartIfNeeded();
}

// ---------------------------------------------------------------------------
// Editor edits (ui::EditController) and their path to the host
// ---------------------------------------------------------------------------

void AmqPlugin::requestHostFlush() noexcept {
    if (_host.canUseParams()) _host.paramsRequestFlush();
}

void AmqPlugin::beginEdit(params::ParamIndex index) {
    if (const auto* s = params::specAt(index)) {
        guiEvents_.push({GuiEvent::Begin, s->hostId, 0.0});
        requestHostFlush();
    }
}

void AmqPlugin::performEdit(params::ParamIndex index, double plain) {
    const auto* s = params::specAt(index);
    if (!s) return;
    if (table_.set(index, plain, params::ParamSource::Gui) != params::SetResult::Changed) return;
    guiEvents_.push({GuiEvent::Value, s->hostId, params::toHost(*s, table_.value(index))});
    requestHostFlush();
}

void AmqPlugin::endEdit(params::ParamIndex index) {
    if (const auto* s = params::specAt(index)) {
        guiEvents_.push({GuiEvent::End, s->hostId, 0.0});
        requestHostFlush();
    }
}

void AmqPlugin::drainGuiEvents(const clap_output_events* out) noexcept {
    GuiEvent e;
    while (guiEvents_.pop(e)) {
        if (!out) continue;
        if (e.kind == GuiEvent::Value) {
            clap_event_param_value v{};
            v.header = {sizeof(v), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            v.param_id = e.id;
            v.note_id = -1;
            v.port_index = v.channel = v.key = -1;
            v.value = e.value;
            out->try_push(out, &v.header);
        } else {
            clap_event_param_gesture g{};
            g.header = {sizeof(g), 0, CLAP_CORE_EVENT_SPACE_ID,
                        static_cast<uint16_t>(e.kind == GuiEvent::Begin ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                                        : CLAP_EVENT_PARAM_GESTURE_END),
                        0};
            g.param_id = e.id;
            out->try_push(out, &g.header);
        }
    }
}

// Host automation changed the table: have the editor refresh within a frame (area 13 R22).
// One callback request at a time; onMainThread() dispatches the notifications.
void AmqPlugin::requestGuiRefresh() noexcept {
    if (editorOpen_.load(std::memory_order_relaxed) && table_.hasPendingNotifications() &&
        !refreshRequested_.exchange(true))
        _host.requestCallback();
}

// ---------------------------------------------------------------------------
// Audio ports: a stereo main pair, plus an optional stereo side-chain input (area 01 R2).
// clap-wrapper turns the non-main input into a VST3 aux bus. Any thread: constant answers.
// ---------------------------------------------------------------------------

namespace {
constexpr clap_id kMainInId = 0, kMainOutId = 1, kSidechainId = 2;
}

uint32_t AmqPlugin::audioPortsCount(bool isInput) const noexcept { return isInput ? 2 : 1; }

bool AmqPlugin::audioPortsInfo(uint32_t index, bool isInput, clap_audio_port_info* info) const noexcept {
    if (!info || index >= audioPortsCount(isInput)) return false;
    std::memset(info, 0, sizeof(*info));
    if (isInput && index == 1) {
        info->id = kSidechainId;
        std::snprintf(info->name, sizeof(info->name), "%s", "Sidechain");
        info->flags = CLAP_AUDIO_PORT_SUPPORTS_64BITS;  // not main: optional, never in place
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    }
    info->id = isInput ? kMainInId : kMainOutId;
    std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "Main In" : "Main Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN | CLAP_AUDIO_PORT_SUPPORTS_64BITS;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = isInput ? kMainOutId : kMainInId;
    return true;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// Any thread. Fixed-size stack buffers only: no allocation.
bool AmqPlugin::stateSave(const clap_ostream* stream) noexcept {
    uint8_t header[kHeaderBytes];
    putU32(header, kStateMagic);
    putU32(header + 4, kStateVersion);
    putU32(header + 8, params::kParamCount);
    if (!writeAll(stream, header, kHeaderBytes)) return false;
    const auto& specs = params::parameterSpecs();
    uint8_t pair[kPairBytes];
    for (params::ParamIndex i = 0; i < params::kParamCount; ++i) {
        putU32(pair, specs[i].hostId);
        putF64(pair + 4, table_.value(i));
        if (!writeAll(stream, pair, kPairBytes)) return false;
    }
    return true;
}

// Any thread. Values go through the setter with source Preset; the host rescan and editor
// notifications happen on the main thread (deferred when called elsewhere).
bool AmqPlugin::stateLoad(const clap_istream* stream) noexcept {
    uint8_t header[kHeaderBytes];
    if (!readAll(stream, header, kHeaderBytes) || getU32(header) != kStateMagic) return false;
    const uint32_t version = getU32(header + 4);
    const uint32_t count = getU32(header + 8);
    if (version == 0) return false;
    // Missing ids take defaults: start from defaults, then apply what the chunk holds.
    std::array<double, params::kParamCount> next{};
    const auto& specs = params::parameterSpecs();
    for (params::ParamIndex i = 0; i < params::kParamCount; ++i) next[i] = specs[i].def;
    if (version >= 2) {
        uint8_t pair[kPairBytes];
        for (uint32_t k = 0; k < count; ++k) {
            if (!readAll(stream, pair, kPairBytes)) return false;
            if (const auto idx = params::findByHostId(getU32(pair)))  // unknown ids are ignored
                next[*idx] = getF64(pair + 4);
        }
    }
    // PROVISIONAL: values are applied one by one. Applying them as one batch before the next
    // block (area 09 R15, area 21 R14) is the batch-load task.
    for (params::ParamIndex i = 0; i < params::kParamCount; ++i) table_.set(i, next[i], params::ParamSource::Preset);
    snapPending_.store(true);  // the audio thread jumps its ramps to the restored values
    requestRestartIfNeeded();  // a restored state may hold another processing mode
    if (onMainThreadNow()) {
        if (_host.canUseParams()) _host.paramsRescan(CLAP_PARAM_RESCAN_VALUES);
        table_.dispatchNotifications();
    } else {
        rescanPending_.store(true);
        _host.requestCallback();
    }
    return true;
}

// ---------------------------------------------------------------------------
// GUI
// ---------------------------------------------------------------------------

bool AmqPlugin::guiIsApiSupported(const char* api, bool isFloating) noexcept {
    return !isFloating && api && std::strcmp(api, ui::Editor::windowApi()) == 0;
}

bool AmqPlugin::guiGetPreferredApi(const char** api, bool* isFloating) noexcept {
    *api = ui::Editor::windowApi();
    *isFloating = false;
    return true;
}

bool AmqPlugin::guiCreate(const char* api, bool isFloating) noexcept {
    if (!guiIsApiSupported(api, isFloating)) return false;
    if (!editor_) {
        editor_ = std::make_unique<ui::Editor>(*this, guiLogicalW_, guiLogicalH_);
        editor_->onSizeChanged = [this](uint32_t w, uint32_t h, double scale) {
            guiHostW_.store(w);
            guiHostH_.store(h);
            guiScale_.store(scale);
            guiLogicalW_ = static_cast<uint32_t>(std::lround(editor_ ? editor_->logicalWidth() : w / scale));
            guiLogicalH_ = static_cast<uint32_t>(std::lround(editor_ ? editor_->logicalHeight() : h / scale));
        };
        editor_->onRequestResize = [this](uint32_t w, uint32_t h) {
            return _host.canUseGui() && _host.guiRequestResize(w, h);
        };
        uint32_t w = 0, h = 0;
        editor_->hostSize(&w, &h);
        editor_->onSizeChanged(w, h, editor_->hostScale());
    }
    editorOpen_.store(true);
    return true;
}

void AmqPlugin::guiDestroy() noexcept {
    editorOpen_.store(false);
    editor_.reset();
}

bool AmqPlugin::guiSetParent(const clap_window* window) noexcept {
    if (!editor_ || !window) return false;
    editor_->attach(window->ptr);
    return true;
}

bool AmqPlugin::guiGetSize(uint32_t* width, uint32_t* height) noexcept {
    // Any thread: never touch the editor off the main thread.
    if (editor_ && onMainThreadNow()) return editor_->hostSize(width, height);
    *width = guiHostW_.load();
    *height = guiHostH_.load();
    return true;
}

static_assert(ui::Editor::kDefaultWidth == 1180 && ui::Editor::kDefaultHeight == 672,
              "keep the size defaults in amq_plugin.h in step with ui::Editor");

bool AmqPlugin::guiGetResizeHints(clap_gui_resize_hints_t* hints) noexcept {
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = false;
    hints->aspect_ratio_width = hints->aspect_ratio_height = 0;
    return true;
}

// Any thread: pure clamp at the last known scale.
bool AmqPlugin::guiAdjustSize(uint32_t* width, uint32_t* height) noexcept {
    ui::Editor::constrainHostSize(width, height, guiScale_.load());
    return true;
}

bool AmqPlugin::guiSetSize(uint32_t width, uint32_t height) noexcept {
    if (!editor_ || !onMainThreadNow()) return false;
    ui::Editor::constrainHostSize(&width, &height, editor_->hostScale());
    editor_->setHostSize(width, height);
    return true;
}

}  // namespace amq::plugin
