#include "amq_plugin.h"
#include "amq_version.h"

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
// An optional editor-state trailer follows the pairs: u32 magic "AMQE", u32 version, f64 zoom
// (editor-spec P-7); version 2 adds u32 length + UTF-8 bytes of the current preset's name, then
// of its path (ui/presets.hpp). Loaders that stop after the pairs ignore it; a chunk without it
// keeps the current zoom and preset.
constexpr uint32_t kStateMagic = 0x53514D41;  // "AMQS"
constexpr uint32_t kStateVersion = 2;
constexpr size_t kHeaderBytes = 12;
constexpr size_t kPairBytes = 12;
constexpr uint32_t kEditorMagic = 0x45514D41;  // "AMQE"
constexpr size_t kEditorBytes = 16;

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
    AMQ_VERSION_FULL,  // CMake's project version; VST3 reports the same (clap-wrapper reads it here)
    "24-band parametric EQ",
    kFeatures,
};

AmqPlugin::AmqPlugin(const clap_host* host) : PluginBase(&descriptor, host) {
    // Refined band designs (R12) come from a worker thread started in activate().
    engine_.setDesignMode(engine::DesignMode::Thread);
    table_.addListener(&presets_);  // marks the current preset edited
}

AmqPlugin::~AmqPlugin() { table_.removeListener(&presets_); }

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
// While processing, the engine first fades the output out (area 02 R13), so process() asks
// only once it is silent; flush and state load run without audio and ask at once.
void AmqPlugin::requestRestartIfNeeded(bool processing) noexcept {
    const bool due = processing ? engine_.restartReady() : engine_.restartNeeded();
    if (due && !restartRequested_.exchange(true)) _host.requestRestart();
}

void AmqPlugin::deactivate() noexcept { engine_.release(); }

void AmqPlugin::reset() noexcept {}

clap_process_status AmqPlugin::process(const clap_process* p) noexcept {
    FtzGuard ftz;
    if (statePending_.load(std::memory_order_acquire)) applyPendingState();  // the whole batch, at once
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
    requestRestartIfNeeded(true);
    requestTextRescanIfNeeded();

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
    if (!processing_.load()) applyPendingState();
    if (undoClearPending_.exchange(false)) undo_.clear();
    if (rescanPending_.exchange(false) && _host.canUseParams()) _host.paramsRescan(CLAP_PARAM_RESCAN_VALUES);
    rescanTextIfNeeded();
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
    *value = params::toHost(*params::specAt(*idx), currentValue(*idx));
    return true;
}

double AmqPlugin::currentValue(params::ParamIndex i) const noexcept {
    if (statePending_.load(std::memory_order_acquire)) {
        StateBatch b;
        if (pendingState_.tryRead(b)) return b.v[i];
    }
    return table_.value(i);
}

// Applies a pending state load as one batch (source Preset: no undo entries, area 21 R14), then
// jumps the ramps to it (no zipper) and has the host and the editor read the new values.
void AmqPlugin::applyPendingState() noexcept {
    if (!statePending_.exchange(false, std::memory_order_acq_rel)) return;
    StateBatch b;
    if (!pendingState_.tryRead(b)) {  // another load is writing: try again at the next chance
        statePending_.store(true, std::memory_order_release);
        return;
    }
    for (params::ParamIndex i = 0; i < params::kParamCount; ++i) table_.set(i, b.v[i], params::ParamSource::Preset);
    snapPending_.store(true);
    rescanPending_.store(true);
    undoClearPending_.store(true);  // undo steps from before the load no longer apply
    _host.requestCallback();
}

bool AmqPlugin::paramsValueToText(clap_id id, double value, char* display, uint32_t size) noexcept {
    const auto idx = params::findByHostId(id);
    if (!idx || !display || size == 0) return false;
    const auto& s = *params::specAt(*idx);
    copyName(display, size, params::formatPlain(s, params::fromHost(s, value), params::displayContext(table_)));
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
    requestRestartIfNeeded(false);
    rescanTextIfNeeded();
}

// The Output Pan text follows Pan Mode (area 04 R6): when the mode changes, tell the host to
// fetch the value text again. Off the main thread this asks for a main-thread callback.
void AmqPlugin::requestTextRescanIfNeeded() noexcept {
    if (params::displayContext(table_).panMidSide != textPanMidSide_.load(std::memory_order_relaxed) &&
        !textCallbackRequested_.exchange(true))
        _host.requestCallback();
}

void AmqPlugin::rescanTextIfNeeded() noexcept {
    textCallbackRequested_.store(false);
    const bool ms = params::displayContext(table_).panMidSide;
    if (textPanMidSide_.exchange(ms) != ms && _host.canUseParams()) _host.paramsRescan(CLAP_PARAM_RESCAN_TEXT);
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
    if (table_.set(index, plain, editSource_) != params::SetResult::Changed) return;
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
// Audio ports: a main pair, plus an optional side-chain input (area 01 R2), all stereo or all
// mono. clap-wrapper turns the non-main input into a VST3 aux bus. Any thread.
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
        info->channel_count = scChannels_.load();
        info->port_type = info->channel_count == 1 ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    }
    info->id = isInput ? kMainInId : kMainOutId;
    std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "Main In" : "Main Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN | CLAP_AUDIO_PORT_SUPPORTS_64BITS;
    info->channel_count = mainChannels_.load();
    info->port_type = info->channel_count == 1 ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
    info->in_place_pair = isInput ? kMainOutId : kMainInId;
    return true;
}

// The two layouts a CLAP host can pick (audio-ports-config): id 0 stereo, id 1 mono.
bool AmqPlugin::audioPortsGetConfig(uint32_t index, clap_audio_ports_config* config) const noexcept {
    if (!config || index >= 2) return false;
    std::memset(config, 0, sizeof(*config));
    const bool mono = index == 1;
    config->id = index;
    std::snprintf(config->name, sizeof(config->name), "%s", mono ? "Mono" : "Stereo");
    config->input_port_count = 2;
    config->output_port_count = 1;
    config->has_main_input = config->has_main_output = true;
    config->main_input_channel_count = config->main_output_channel_count = mono ? 1 : 2;
    config->main_input_port_type = config->main_output_port_type = mono ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
    return true;
}

bool AmqPlugin::audioPortsSetConfig(clap_id configId) noexcept {
    if (isActive() || configId > 1) return false;
    const uint32_t ch = configId == 1 ? 1 : 2;
    mainChannels_ = ch, scChannels_ = ch;
    return true;
}

bool AmqPlugin::layoutFor(const clap_audio_port_configuration_request* requests, uint32_t count, uint32_t& main,
                          uint32_t& sc) const noexcept {
    uint32_t in = mainChannels_.load(), out = in;
    sc = scChannels_.load();
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = requests[i];
        if (r.channel_count < 1 || r.channel_count > 2) return false;
        if (r.port_type && std::strcmp(r.port_type, r.channel_count == 1 ? CLAP_PORT_MONO : CLAP_PORT_STEREO) != 0)
            return false;
        if (r.is_input && r.port_index == 0) in = r.channel_count;
        else if (r.is_input && r.port_index == 1) sc = r.channel_count;
        else if (!r.is_input && r.port_index == 0) out = r.channel_count;
        else return false;
    }
    if (in != out) return false;  // the engine runs the main pair at one width
    main = in;
    return true;
}

bool AmqPlugin::configurableAudioPortsCanApplyConfiguration(const clap_audio_port_configuration_request* requests,
                                                            uint32_t count) const noexcept {
    uint32_t main = 0, sc = 0;
    return !isActive() && layoutFor(requests, count, main, sc);
}

bool AmqPlugin::configurableAudioPortsApplyConfiguration(const clap_audio_port_configuration_request* requests,
                                                         uint32_t count) noexcept {
    uint32_t main = 0, sc = 0;
    if (isActive() || !layoutFor(requests, count, main, sc)) return false;
    mainChannels_ = main, scChannels_ = sc;
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
        putF64(pair + 4, currentValue(i));
        if (!writeAll(stream, pair, kPairBytes)) return false;
    }
    uint8_t trailer[kEditorBytes];
    putU32(trailer, kEditorMagic);
    putU32(trailer + 4, 2);
    putF64(trailer + 8, guiZoom_.load());
    if (!writeAll(stream, trailer, kEditorBytes)) return false;
    std::string name, path;
    presets_.current(name, path);
    for (const std::string* t : {&name, &path}) {
        uint8_t len[4];
        putU32(len, static_cast<uint32_t>(std::min<size_t>(t->size(), 4096)));
        if (!writeAll(stream, len, 4) ||
            !writeAll(stream, reinterpret_cast<const uint8_t*>(t->data()), std::min<size_t>(t->size(), 4096)))
            return false;
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
        uint8_t trailer[kEditorBytes];
        if (readAll(stream, trailer, kEditorBytes) && getU32(trailer) == kEditorMagic && getU32(trailer + 4) >= 1) {
            const double z = getF64(trailer + 8);
            for (double k : ui::Editor::kZooms)
                if (std::abs(z - k) < 1e-6) guiZoom_.store(k);
            if (getU32(trailer + 4) >= 2 && !presets_.loading) {  // a host recall restores the preset name
                std::array<std::string, 2> text;
                bool ok = true;
                for (auto& t : text) {
                    uint8_t len[4];
                    ok = ok && readAll(stream, len, 4);
                    const uint32_t n = ok ? getU32(len) : 0;
                    if (!ok || n > 4096) {
                        ok = false;
                        break;
                    }
                    t.resize(n);
                    ok = n == 0 || readAll(stream, reinterpret_cast<uint8_t*>(t.data()), n);
                }
                if (ok) presets_.setCurrent(text[0], text[1], false);
            }
        }
    }
    // One batch (area 09 R15, area 21 R14): the audio thread applies every value at the start of its
    // next block; while not processing, it is applied here at once.
    StateBatch batch;
    batch.v = next;
    pendingState_.write(batch);
    statePending_.store(true, std::memory_order_release);
    if (!processing_.load()) applyPendingState();
    requestRestartIfNeeded(false);  // a restored state may hold another processing mode
    if (onMainThreadNow()) {
        if (_host.canUseParams()) _host.paramsRescan(CLAP_PARAM_RESCAN_VALUES);
        rescanTextIfNeeded();
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
        editor_ = std::make_unique<ui::Editor>(undo_, guiLogicalW_, guiLogicalH_, guiZoom_.load());
        editor_->onZoomChanged = [this](double z) { guiZoom_.store(z); };
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

static_assert(ui::Editor::kDefaultWidth == 1280 && ui::Editor::kDefaultHeight == 720,
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
    ui::Editor::constrainHostSize(width, height, guiScale_.load(), guiZoom_.load());
    return true;
}

bool AmqPlugin::guiSetSize(uint32_t width, uint32_t height) noexcept {
    if (!editor_ || !onMainThreadNow()) return false;
    ui::Editor::constrainHostSize(&width, &height, editor_->hostScale(), editor_->zoom());
    editor_->setHostSize(width, height);
    return true;
}

// ---------------------------------------------------------------------------
// Presets (main thread)
// ---------------------------------------------------------------------------

namespace {
struct VecOut {
    std::vector<uint8_t> bytes;
    clap_ostream s{this, [](const clap_ostream* o, const void* d, uint64_t n) -> int64_t {
                       auto* self = static_cast<VecOut*>(o->ctx);
                       const auto* b = static_cast<const uint8_t*>(d);
                       self->bytes.insert(self->bytes.end(), b, b + n);
                       return static_cast<int64_t>(n);
                   }};
};
struct VecIn {
    const std::vector<uint8_t>* bytes = nullptr;
    size_t pos = 0;
    clap_istream s{this, [](const clap_istream* i, void* d, uint64_t n) -> int64_t {
                       auto* self = static_cast<VecIn*>(i->ctx);
                       const size_t k = std::min<size_t>(static_cast<size_t>(n), self->bytes->size() - self->pos);
                       std::memcpy(d, self->bytes->data() + self->pos, k);
                       self->pos += k;
                       return static_cast<int64_t>(k);
                   }};
};
std::string u8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}
}  // namespace

bool AmqPlugin::PresetBridge::loadPreset(const ui::PresetEntry& e, std::string* error) {
    std::vector<uint8_t> bytes;
    if (e.builtIn()) {  // Init: every parameter at its default
        bytes.resize(kHeaderBytes + params::kParamCount * kPairBytes);
        putU32(bytes.data(), kStateMagic);
        putU32(bytes.data() + 4, kStateVersion);
        putU32(bytes.data() + 8, params::kParamCount);
        const auto& specs = params::parameterSpecs();
        for (params::ParamIndex i = 0; i < params::kParamCount; ++i) {
            uint8_t* q = bytes.data() + kHeaderBytes + static_cast<size_t>(i) * kPairBytes;
            putU32(q, specs[i].hostId);
            putF64(q + 4, specs[i].def);
        }
    } else if (!ui::readFileBytes(e.path, bytes, error)) {
        return false;
    }
    const double zoom = p_.guiZoom_.load();  // a preset never changes the interface zoom
    VecIn in;
    in.bytes = &bytes;
    loading = true;
    const bool ok = p_.stateLoad(&in.s);
    loading = false;
    p_.guiZoom_.store(zoom);
    if (!ok) {
        if (error) *error = "Not an Am-Q preset: " + u8(e.path.filename()) + ".";
        return false;
    }
    p_.table_.dispatchNotifications();  // the load's own notifications, before the edited flag resets
    setCurrent(e.name, e.builtIn() ? std::string() : u8(e.path), false);
    return true;
}

bool AmqPlugin::PresetBridge::savePreset(const std::filesystem::path& path, std::string* error) {
    std::filesystem::path p = path;
    if (p.extension() != ui::kPresetExtension) p += ui::kPresetExtension;
    setCurrent(u8(p.stem()), u8(p), false);  // the saved file carries its own name
    VecOut out;
    if (!p_.stateSave(&out.s)) {
        if (error) *error = "Could not save the preset.";
        return false;
    }
    return ui::writeFileBytes(p, out.bytes, error);
}

std::string AmqPlugin::PresetBridge::currentName() const {
    std::lock_guard<std::mutex> l(m_);
    return name_;
}

std::filesystem::path AmqPlugin::PresetBridge::currentPath() const {
    std::lock_guard<std::mutex> l(m_);
    const std::u8string s(path_.begin(), path_.end());
    return std::filesystem::path(s);
}

void AmqPlugin::PresetBridge::setCurrent(const std::string& name, const std::string& pathUtf8, bool edited) {
    std::lock_guard<std::mutex> l(m_);
    name_ = name.empty() ? std::string(ui::kInitPresetName) : name;
    path_ = pathUtf8;
    edited_ = edited;
}

void AmqPlugin::PresetBridge::current(std::string& name, std::string& pathUtf8) const {
    std::lock_guard<std::mutex> l(m_);
    name = name_, pathUtf8 = path_;
}

void AmqPlugin::PresetBridge::parameterChanged(params::ParamIndex i, double, params::ParamSource source) {
    if (loading || source == params::ParamSource::Preset) return;
    const auto* s = params::specAt(i);
    if (s && !s->automatable) return;  // display settings (analyzer, display range) are not "edits"
    edited_ = true;
}

// ---------------------------------------------------------------------------
// EQ Match bridge (main thread)
// ---------------------------------------------------------------------------

bool AmqPlugin::MatchBridge::status(ui::MatchStatus& o) {
    auto& m = e_.eqMatch();
    m.latest(snap_);  // keeps the last one when nothing new arrived
    o.inputReady = snap_.inputReady, o.referenceReady = snap_.referenceReady;
    o.inputRecording = snap_.inputRecording, o.referenceRecording = snap_.referenceRecording;
    o.inputSeconds = snap_.inputSeconds, o.referenceSeconds = snap_.referenceSeconds;
    o.curveValid = snap_.curve.valid;
    o.levelOffsetDb = snap_.curve.levelOffsetDb;
    o.target = snap_.curve.target, o.weight = snap_.curve.weight;
    o.fitting = m.fitRunning();
    o.fitProgress = m.fitProgress();
    o.fileBusy = m.fileBusy();
    o.fileProgress = m.fileProgress();
    o.fileError = m.fileError();
    o.referenceIsFile = m.referenceSource() == engine::MatchReferenceSource::File;
    const double fs = e_.sampleRate() > 0.0 ? e_.sampleRate() : 48000.0;
    o.inputDroppedSeconds = static_cast<double>(e_.analyzer().droppedFrames(analysis::Source::Pre)) / fs;
    o.referenceDroppedSeconds = static_cast<double>(e_.analyzer().droppedFrames(analysis::Source::External)) / fs;
    o.lastSeconds = lastSeconds_, o.windowSeconds = seconds_, o.smoothing = smoothing_, o.levelAlign = align_;
    return true;
}

void AmqPlugin::MatchBridge::setReferenceFromFile(bool file) {
    e_.eqMatch().setReferenceSource(file ? engine::MatchReferenceSource::File : engine::MatchReferenceSource::SideChain);
    e_.eqMatch().reset(1);
}

bool AmqPlugin::MatchBridge::loadReferenceFile(const std::string& path, std::string* error) {
    return e_.eqMatch().loadReferenceFile(path, error);
}

void AmqPlugin::MatchBridge::setWindow(bool lastSeconds, double seconds) {
    lastSeconds_ = lastSeconds, seconds_ = seconds;
    e_.eqMatch().setWindow(lastSeconds ? analysis::MatchCapture::Window::LastSeconds : analysis::MatchCapture::Window::Whole,
                           seconds);
}

void AmqPlugin::MatchBridge::setSmoothing(int index) {
    smoothing_ = std::clamp(index, 0, 3);
    e_.eqMatch().setSmoothing(static_cast<analysis::MatchSmoothing>(smoothing_));
}

void AmqPlugin::MatchBridge::setLevelAlign(bool on) {
    align_ = on;
    e_.eqMatch().setLevelAlign(on);
}

bool AmqPlugin::MatchBridge::takeFit(std::vector<ui::MatchBand>& out) {
    engine::FitResult r;
    if (!e_.eqMatch().takeFit(r)) return false;
    out.clear();
    for (const auto& b : r.bands) out.push_back({b.shape, b.slope, b.hz, b.gainDb, b.q});
    return true;
}

}  // namespace amq::plugin
