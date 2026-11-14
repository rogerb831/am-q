#include "spike_plugin.h"

#include "spike_editor.h"

#include <clap/helpers/host-proxy.hxx>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <xmmintrin.h>
#include <cstring>

namespace amq::spike {

static const char* kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_EQUALIZER,
                                  CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor SpikePlugin::descriptor = {
    CLAP_VERSION,
    "dev.am-q.spike.clap-native",
    "Am-Q Spike",
    "Am-Q",
    "",
    "",
    "",
    "0.0.1",
    "Framework spike: one band, CLAP-native, Visage editor",
    kFeatures,
};

SpikePlugin::SpikePlugin(const clap_host* host) : PluginBase(&descriptor, host) {
    for (uint32_t i = 0; i < kParamCount; ++i) {
        audio_[i] = kParams[i].def;
        ui_[i].store(kParams[i].def);
    }
}

SpikePlugin::~SpikePlugin() = default;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool SpikePlugin::activate(double sampleRate, uint32_t, uint32_t) noexcept {
    sampleRate_ = sampleRate;
    filterDirty_ = true;
    redesign();
    reset();
    // Latency may only change during activate (CLAP latency extension).
    const uint32_t wanted = requestedLatency_.load();
    if (wanted != activeLatency_.load()) {
        activeLatency_.store(wanted);
        if (_host.canUseLatency()) _host.latencyChanged();
    }
    return true;
}

void SpikePlugin::deactivate() noexcept {}

void SpikePlugin::reset() noexcept {
    z1_[0] = z1_[1] = z2_[0] = z2_[1] = 0.0;
    for (auto& ch : delay_) ch.fill(0.0f);
    delayPos_ = 0;
}

void SpikePlugin::onMainThread() noexcept {
    // Latency change requested from the audio thread: restart if active, so the new value
    // is applied in activate() and announced to the host.
    // While inactive nothing is announced: activate() applies the value and calls
    // latency.changed() (the only place CLAP allows it).
    if (requestedLatency_.load() != activeLatency_.load() && isActive()) _host.requestRestart();
    if (rescanPending_.exchange(false) && _host.canUseParams()) _host.paramsRescan(CLAP_PARAM_RESCAN_VALUES);
    if (uiDirty_.exchange(false) && editor_) editor_->refreshFromPlugin();
}

bool SpikePlugin::onMainThreadNow() const noexcept {
    return !_host.canUseThreadCheck() || _host.isMainThread();
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

bool SpikePlugin::paramsInfo(uint32_t index, clap_param_info* info) const noexcept {
    if (index >= kParamCount) return false;
    const auto& d = kParams[index];
    std::memset(info, 0, sizeof(*info));
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE | (d.stepped ? CLAP_PARAM_IS_STEPPED : 0);
    info->min_value = d.min;
    info->max_value = d.max;
    info->default_value = d.def;
    std::snprintf(info->name, sizeof(info->name), "%s", d.name);
    std::snprintf(info->module, sizeof(info->module), "%s", index == kLatency ? "Test" : "Band 1");
    return true;
}

bool SpikePlugin::paramsValue(clap_id id, double* value) noexcept {
    if (id >= kParamCount) return false;
    *value = ui_[id].load();
    return true;
}

bool SpikePlugin::paramsValueToText(clap_id id, double value, char* display, uint32_t size) noexcept {
    if (id >= kParamCount) return false;
    formatParam(id, value, display, size);
    return true;
}

bool SpikePlugin::paramsTextToValue(clap_id id, const char* display, double* value) noexcept {
    if (id >= kParamCount) return false;
    if (id == kLatency) {  // "Off" / "64 samples" / numbers
        if (std::strstr(display, "Off") || std::strstr(display, "off")) { *value = 0.0; return true; }
        if (std::strstr(display, "64")) { *value = 1.0; return true; }
    }
    // Skip a unit prefix such as "Q " before the number.
    while (*display && !std::isdigit(static_cast<unsigned char>(*display)) && *display != '-' && *display != '+' &&
           *display != '.')
        ++display;
    char* end = nullptr;
    double v = std::strtod(display, &end);
    if (end == display) return false;
    switch (id) {
        case kFreq: {
            if (std::strstr(display, "k")) v *= 1000.0;
            v = std::log2(std::max(v, 1.0));
            break;
        }
        case kQ: v = normalisedFromQ(std::max(v, 0.025)); break;
        case kLatency: v = (std::strstr(display, "64") != nullptr || v >= 0.5) ? 1.0 : 0.0; break;
        default: break;
    }
    *value = std::clamp(v, kParams[id].min, kParams[id].max);
    return true;
}

void SpikePlugin::applyParam(clap_id id, double value) noexcept {
    if (id >= kParamCount) return;
    value = std::clamp(value, kParams[id].min, kParams[id].max);
    audio_[id] = value;
    ui_[id].store(value, std::memory_order_relaxed);
    switch (id) {
        case kFreq:
        case kGain:
        case kQ: filterDirty_ = true; break;
        case kOutGain: outGainLin_ = dsp::dbToLinear(value); break;
        case kLatency: {
            const uint32_t want = value >= 0.5 ? kDummyLatency : 0;
            if (requestedLatency_.exchange(want) != want) _host.requestCallback();
            break;
        }
        default: break;
    }
}

void SpikePlugin::handleEvent(const clap_event_header* ev) noexcept {
    if (ev->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    if (ev->type == CLAP_EVENT_PARAM_VALUE) {
        const auto* pv = reinterpret_cast<const clap_event_param_value*>(ev);
        applyParam(pv->param_id, pv->value);
        uiDirty_.store(true);
    }
}

void SpikePlugin::drainGuiQueue(const clap_output_events* out) noexcept {
    // A loaded state is applied first; GUI edits queued after it then win.
    if (stateDirty_.exchange(false)) {
        for (uint32_t i = 0; i < kParamCount; ++i) applyParam(i, ui_[i].load());
    }
    GuiMessage m;
    while (guiQueue_.pop(m)) {
        if (m.kind == GuiMessage::kValue) {
            applyParam(m.id, m.value);
            if (out) {
                clap_event_param_value e{};
                e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
                e.param_id = m.id;
                e.note_id = -1;
                e.port_index = -1;
                e.channel = -1;
                e.key = -1;
                e.value = m.value;
                out->try_push(out, &e.header);
            }
        } else if (out) {
            clap_event_param_gesture g{};
            g.header = {sizeof(g), 0, CLAP_CORE_EVENT_SPACE_ID,
                        static_cast<uint16_t>(m.kind == GuiMessage::kBegin ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                                           : CLAP_EVENT_PARAM_GESTURE_END),
                        0};
            g.param_id = m.id;
            out->try_push(out, &g.header);
        }
    }
}

void SpikePlugin::paramsFlush(const clap_input_events* in, const clap_output_events* out) noexcept {
    drainGuiQueue(out);
    const uint32_t n = in ? in->size(in) : 0;
    for (uint32_t i = 0; i < n; ++i) handleEvent(in->get(in, i));
    if (filterDirty_) redesign();
}

void SpikePlugin::guiBeginEdit(clap_id id) {
    guiQueue_.push({GuiMessage::kBegin, id, 0.0});
    _host.paramsRequestFlush();
}
void SpikePlugin::guiSetValue(clap_id id, double value) {
    value = std::clamp(value, kParams[id].min, kParams[id].max);
    ui_[id].store(value);
    guiQueue_.push({GuiMessage::kValue, id, value});
    _host.paramsRequestFlush();
}
void SpikePlugin::guiEndEdit(clap_id id) {
    guiQueue_.push({GuiMessage::kEnd, id, 0.0});
    _host.paramsRequestFlush();
}

// ---------------------------------------------------------------------------
// DSP
// ---------------------------------------------------------------------------

// SPIKE ONLY: the design runs on the audio thread and allocates (std::vector inside the
// prototype). Phase 1 moves design off the audio thread (R19). The three-point matched
// design is used because it costs about 1 us; the refined design costs milliseconds.
void SpikePlugin::redesign() noexcept {
    filterDirty_ = false;
    const dsp::BandSpec spec{dsp::Shape::Bell, std::exp2(audio_[kFreq]), audio_[kGain],
                             qFromNormalised(audio_[kQ]), dsp::Slope::Db12};
    const auto proto = dsp::designAnalogPrototype(spec);
    const auto d = dsp::designMatched(proto, sampleRate_, dsp::DesignMethod::MatchedThreePoint);
    bq_ = d.sections.empty() ? dsp::Biquad{} : d.sections[0];
}

void SpikePlugin::render(const clap_process* p, uint32_t from, uint32_t to) noexcept {
    if (from >= to) return;
    const auto& in = p->audio_inputs[0];
    const auto& out = p->audio_outputs[0];
    const uint32_t chans = std::min(in.channel_count, out.channel_count);
    const uint32_t lat = activeLatency_.load(std::memory_order_relaxed);
    for (uint32_t c = 0; c < chans && c < 2; ++c) {
        const float* x = in.data32[c];
        float* y = out.data32[c];
        double z1 = z1_[c], z2 = z2_[c];
        uint32_t dp = delayPos_;
        for (uint32_t i = from; i < to; ++i) {
            const double v = x[i];
            const double o = bq_.b0 * v + z1;
            z1 = bq_.b1 * v - bq_.a1 * o + z2;
            z2 = bq_.b2 * v - bq_.a2 * o;
            float s = static_cast<float>(o * outGainLin_);
            if (lat) {
                float& slot = delay_[c][dp];
                const float delayed = slot;
                slot = s;
                s = delayed;
                dp = (dp + 1) % kDummyLatency;
            }
            y[i] = s;
        }
        z1_[c] = z1;
        z2_[c] = z2;
        if (c + 1 == chans || c == 1) delayPos_ = dp;
    }
}

clap_process_status SpikePlugin::process(const clap_process* p) noexcept {
    // Flush denormals to zero for the duration of the call (restored on return).
    struct FtzGuard {
        unsigned int saved = _mm_getcsr();
        FtzGuard() { _mm_setcsr(saved | 0x8040); }  // FTZ (bit 15) | DAZ (bit 6)
        ~FtzGuard() { _mm_setcsr(saved); }
    } ftz;
    drainGuiQueue(p->out_events);
    if (filterDirty_) redesign();

    const uint32_t n = p->frames_count;
    const uint32_t evCount = p->in_events ? p->in_events->size(p->in_events) : 0;
    uint32_t ei = 0, pos = 0;
    // Sample-accurate: split the block at every event time (R3).
    while (pos < n) {
        while (ei < evCount) {
            const auto* ev = p->in_events->get(p->in_events, ei);
            if (ev->time > pos) break;
            handleEvent(ev);
            ++ei;
        }
        if (filterDirty_) redesign();
        uint32_t next = n;
        if (ei < evCount) next = std::min(n, p->in_events->get(p->in_events, ei)->time);
        render(p, pos, next);
        pos = next;
    }
    while (ei < evCount) handleEvent(p->in_events->get(p->in_events, ei++));  // events at time >= n

    if (uiDirty_.load()) _host.requestCallback();
    return CLAP_PROCESS_CONTINUE;
}

bool SpikePlugin::audioPortsInfo(uint32_t index, bool isInput, clap_audio_port_info* info) const noexcept {
    if (index != 0) return false;
    std::memset(info, 0, sizeof(*info));
    info->id = isInput ? 0 : 1;
    std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "Main In" : "Main Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = isInput ? 1 : 0;
    return true;
}

// ---------------------------------------------------------------------------
// State: version + raw doubles
// ---------------------------------------------------------------------------

bool SpikePlugin::stateSave(const clap_ostream* stream) noexcept {
    struct {
        uint32_t version;
        uint32_t reserved;  // explicit padding so the saved bytes are deterministic
        double v[kParamCount];
    } s{};
    s.version = 1;
    for (uint32_t i = 0; i < kParamCount; ++i) s.v[i] = ui_[i].load();
    const auto* bytes = reinterpret_cast<const uint8_t*>(&s);
    size_t written = 0;
    while (written < sizeof(s)) {
        const auto r = stream->write(stream, bytes + written, sizeof(s) - written);
        if (r <= 0) return false;
        written += static_cast<size_t>(r);
    }
    return true;
}

bool SpikePlugin::stateLoad(const clap_istream* stream) noexcept {
    struct {
        uint32_t version;
        uint32_t reserved;
        double v[kParamCount];
    } s{};
    auto* bytes = reinterpret_cast<uint8_t*>(&s);
    size_t got = 0;
    while (got < sizeof(s)) {
        const auto r = stream->read(stream, bytes + got, sizeof(s) - got);
        if (r <= 0) break;
        got += static_cast<size_t>(r);
    }
    if (got < sizeof(s) || s.version != 1) return false;
    // May be called off the main thread by wrapped VST3 hosts: no GUI-queue push (the editor is
    // its only producer), no editor access, and params.rescan deferred to the main thread.
    for (uint32_t i = 0; i < kParamCount; ++i)
        ui_[i].store(std::clamp(s.v[i], kParams[i].min, kParams[i].max));
    stateDirty_.store(true);
    _host.paramsRequestFlush();
    if (onMainThreadNow()) {
        if (_host.canUseParams()) _host.paramsRescan(CLAP_PARAM_RESCAN_VALUES);
        if (editor_) editor_->refreshFromPlugin();
    } else {
        rescanPending_.store(true);
        uiDirty_.store(true);
        _host.requestCallback();
    }
    return true;
}

// ---------------------------------------------------------------------------
// GUI
// ---------------------------------------------------------------------------

bool SpikePlugin::guiIsApiSupported(const char* api, bool isFloating) noexcept {
    return !isFloating && std::strcmp(api, CLAP_WINDOW_API_WIN32) == 0;
}

bool SpikePlugin::guiGetPreferredApi(const char** api, bool* isFloating) noexcept {
    *api = CLAP_WINDOW_API_WIN32;
    *isFloating = false;
    return true;
}

bool SpikePlugin::guiCreate(const char* api, bool isFloating) noexcept {
    if (!guiIsApiSupported(api, isFloating)) return false;
    if (!editor_) editor_ = std::make_unique<SpikeEditor>(*this);
    return true;
}

void SpikePlugin::guiDestroy() noexcept { editor_.reset(); }

bool SpikePlugin::guiSetParent(const clap_window* window) noexcept {
    if (!editor_) return false;
    editor_->attach(window->win32);
    return true;
}

bool SpikePlugin::guiGetSize(uint32_t* width, uint32_t* height) noexcept {
    if (!editor_ || !onMainThreadNow()) {  // fixed-size editor; never touch it off the main thread
        *width = SpikeEditor::kWidth;
        *height = SpikeEditor::kHeight;
        return true;
    }
    editor_->nativeSize(width, height);
    return true;
}

}  // namespace amq::spike
