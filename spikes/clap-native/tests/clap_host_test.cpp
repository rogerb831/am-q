// Minimal CLAP test host for the Am-Q spike (evidence for pass criteria 1 and 2).
//  1. Sample-accurate automation: a parameter event at offset k inside a block must change
//     the output starting exactly at sample k (compared with a run without the event).
//  2. Latency change: toggling the dummy-latency parameter must lead to request_restart,
//     latency.changed() during re-activation, latency.get() == 64 and an audio delay of 64.
// Usage: clap_host_test <path to AmQSpike.clap>
#include <clap/clap.h>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

namespace {

struct TestHost {
    clap_host host{};
    int latencyChanged = 0, restartRequests = 0, callbackRequests = 0, flushRequests = 0, rescans = 0;
    std::thread::id mainThread = std::this_thread::get_id();
    clap_host_latency latencyExt{};
    clap_host_thread_check threadExt{};
    clap_host_params paramsExt{};
    clap_host_log logExt{};

    TestHost() {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "amq-spike-test-host";
        host.vendor = "Am-Q";
        host.url = "";
        host.version = "0.0.1";
        host.get_extension = [](const clap_host* h, const char* id) -> const void* {
            auto* self = static_cast<TestHost*>(h->host_data);
            if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &self->latencyExt;
            if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &self->paramsExt;
            if (!std::strcmp(id, CLAP_EXT_LOG)) return &self->logExt;
            if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK)) return &self->threadExt;
            return nullptr;
        };
        host.request_restart = [](const clap_host* h) { ++static_cast<TestHost*>(h->host_data)->restartRequests; };
        host.request_process = [](const clap_host*) {};
        host.request_callback = [](const clap_host* h) { ++static_cast<TestHost*>(h->host_data)->callbackRequests; };
        latencyExt.changed = [](const clap_host* h) { ++static_cast<TestHost*>(h->host_data)->latencyChanged; };
        paramsExt.rescan = [](const clap_host* h, clap_param_rescan_flags) { ++static_cast<TestHost*>(h->host_data)->rescans; };
        threadExt.is_main_thread = [](const clap_host* h) {
            return std::this_thread::get_id() == static_cast<TestHost*>(h->host_data)->mainThread;
        };
        threadExt.is_audio_thread = [](const clap_host*) { return true; };  // this host processes on main too
        paramsExt.clear = [](const clap_host*, clap_id, clap_param_clear_flags) {};
        paramsExt.request_flush = [](const clap_host* h) { ++static_cast<TestHost*>(h->host_data)->flushRequests; };
        logExt.log = [](const clap_host*, clap_log_severity s, const char* msg) { std::printf("  [plugin log %d] %s\n", s, msg); };
    }
};

struct EventList {
    std::vector<clap_event_param_value> evs;
    clap_input_events in{};
    clap_output_events out{};
    EventList() {
        in.ctx = this;
        in.size = [](const clap_input_events* l) { return static_cast<uint32_t>(static_cast<EventList*>(l->ctx)->evs.size()); };
        in.get = [](const clap_input_events* l, uint32_t i) -> const clap_event_header* {
            return &static_cast<EventList*>(l->ctx)->evs[i].header;
        };
        out.ctx = this;
        out.try_push = [](const clap_output_events*, const clap_event_header*) { return true; };
    }
    void add(uint32_t time, clap_id id, double value) {
        clap_event_param_value e{};
        e.header = {sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        e.param_id = id;
        e.note_id = -1;
        e.port_index = -1;
        e.channel = -1;
        e.key = -1;
        e.value = value;
        evs.push_back(e);
    }
};

struct Instance {
    TestHost th;
    const clap_plugin* p = nullptr;
    const clap_plugin_latency* lat = nullptr;
    double sr = 48000.0;
    uint32_t block = 256;
    bool active = false;

    explicit Instance(const clap_plugin_entry* entry) {
        auto* f = static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        const auto* d = f->get_plugin_descriptor(f, 0);
        p = f->create_plugin(f, &th.host, d->id);
        p->init(p);
        lat = static_cast<const clap_plugin_latency*>(p->get_extension(p, CLAP_EXT_LATENCY));
        activate();
    }
    ~Instance() {
        deactivate();
        p->destroy(p);
    }
    void activate() {
        p->activate(p, sr, 1, block);
        p->start_processing(p);
        active = true;
    }
    void deactivate() {
        if (!active) return;
        p->stop_processing(p);
        p->deactivate(p);
        active = false;
    }
    // Process one block of a 2-channel signal; returns left output.
    std::vector<float> run(const std::vector<float>& input, EventList& ev) {
        std::vector<float> inL(input), inR(input), outL(input.size()), outR(input.size());
        float* ins[2] = {inL.data(), inR.data()};
        float* outs[2] = {outL.data(), outR.data()};
        clap_audio_buffer ib{}, ob{};
        ib.data32 = ins;
        ib.channel_count = 2;
        ob.data32 = outs;
        ob.channel_count = 2;
        clap_process pr{};
        pr.steady_time = -1;
        pr.frames_count = static_cast<uint32_t>(input.size());
        pr.audio_inputs = &ib;
        pr.audio_outputs = &ob;
        pr.audio_inputs_count = 1;
        pr.audio_outputs_count = 1;
        pr.in_events = &ev.in;
        pr.out_events = &ev.out;
        p->process(p, &pr);
        return outL;
    }
};

std::vector<float> noise(size_t n, uint32_t seed) {
    std::vector<float> v(n);
    for (auto& x : v) {
        seed = seed * 1664525u + 1013904223u;
        x = static_cast<float>((seed >> 8) * (1.0 / 16777216.0) - 0.5);
    }
    return v;
}

// Renders 3 blocks; optionally one event (block 1, offset k). Returns the concatenated output.
// An optional preset event (block 0, offset 0) is sent in both runs.
std::vector<float> render(const clap_plugin_entry* entry, bool withEvent, clap_id id, double value, uint32_t k,
                          int presetId = -1, double presetValue = 0.0) {
    Instance inst(entry);
    std::vector<float> all;
    for (int b = 0; b < 3; ++b) {
        EventList ev;
        if (presetId >= 0 && b == 0) ev.add(0, static_cast<clap_id>(presetId), presetValue);
        if (withEvent && b == 1) ev.add(k, id, value);
        const auto out = inst.run(noise(inst.block, 1234u + b), ev);
        all.insert(all.end(), out.begin(), out.end());
    }
    return all;
}

int firstDifference(const std::vector<float>& a, const std::vector<float>& b) {
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return static_cast<int>(i);
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: clap_host_test <plugin.clap>\n");
        return 2;
    }
    HMODULE mod = LoadLibraryA(argv[1]);
    if (!mod) {
        std::printf("FAIL: cannot load %s\n", argv[1]);
        return 1;
    }
    auto* entry = reinterpret_cast<const clap_plugin_entry*>(GetProcAddress(mod, "clap_entry"));
    if (!entry || !entry->init(argv[1])) {
        std::printf("FAIL: no clap_entry\n");
        return 1;
    }
    int failures = 0;

    // 1. Sample-accurate automation: output gain and band gain steps inside a block.
    struct Step { const char* name; clap_id id; double value; uint32_t offset; int presetId; double presetValue; };
    const Step steps[] = {{"Output Gain -6 dB", 3, -6.0, 37, -1, 0.0},
                          {"Band Gain +12 dB", 1, 12.0, 101, -1, 0.0},
                          {"Freq 4 kHz (band gain +12 dB)", 0, std::log2(4000.0), 1, 1, 12.0},
                          {"Q 4 (band gain +12 dB)", 2, 0.69, 200, 1, 12.0},
                          {"Output Gain at offset 0", 3, 3.0, 0, -1, 0.0},
                          {"Output Gain at last sample", 3, -3.0, 255, -1, 0.0}};
    for (const auto& s : steps) {
        const auto ref = render(entry, false, s.id, s.value, s.offset, s.presetId, s.presetValue);
        const auto got = render(entry, true, s.id, s.value, s.offset, s.presetId, s.presetValue);
        const int expected = 256 + static_cast<int>(s.offset);
        const int first = firstDifference(ref, got);
        const bool ok = first == expected;
        failures += !ok;
        std::printf("[%s] automation step '%s' at block 1 offset %u: first changed sample %d, expected %d\n",
                    ok ? "PASS" : "FAIL", s.name, s.offset, first, expected);
    }

    // 2. Latency change notification and real delay.
    {
        Instance inst(entry);
        const uint32_t before = inst.lat ? inst.lat->get(inst.p) : 9999;
        EventList ev;
        ev.add(10, 4, 1.0);  // dummy latency on
        inst.run(std::vector<float>(inst.block, 0.0f), ev);
        const int cbBefore = inst.th.callbackRequests;
        inst.p->on_main_thread(inst.p);  // host services request_callback on the main thread
        const int restarts = inst.th.restartRequests;
        if (restarts > 0) {  // host honours the restart: deactivate + activate
            inst.deactivate();
            inst.activate();
        }
        const uint32_t after = inst.lat ? inst.lat->get(inst.p) : 9999;
        // Impulse test: the output peak must sit exactly `after` samples late.
        std::vector<float> imp(inst.block, 0.0f);
        imp[0] = 1.0f;
        EventList none;
        none.add(0, 1, 0.0);  // band gain 0 dB: the band is flat, so only the delay remains
        auto out = inst.run(imp, none);
        int peak = 0;
        for (int i = 1; i < static_cast<int>(out.size()); ++i)
            if (std::abs(out[i]) > std::abs(out[peak])) peak = i;
        const bool ok = before == 0 && cbBefore > 0 && restarts == 1 && inst.th.latencyChanged == 1 && after == 64 && peak == 64;
        failures += !ok;
        std::printf("[%s] latency: before %u, request_callback %d, request_restart %d, latency.changed %d, after %u, impulse peak at %d\n",
                    ok ? "PASS" : "FAIL", before, cbBefore, restarts, inst.th.latencyChanged, after, peak);
    }

    // 3. Calls a wrapped VST3 host may make off the main thread (VST3 allows e.g.
    //    getLatencySamples() from any thread; Blue Cat's Axiom does this). They must not abort,
    //    and state loaded off the main thread must still reach the audio and the host.
    {
        Instance inst(entry);
        const auto* params = static_cast<const clap_plugin_params*>(inst.p->get_extension(inst.p, CLAP_EXT_PARAMS));
        const auto* state = static_cast<const clap_plugin_state*>(inst.p->get_extension(inst.p, CLAP_EXT_STATE));
        const auto* ports = static_cast<const clap_plugin_audio_ports*>(inst.p->get_extension(inst.p, CLAP_EXT_AUDIO_PORTS));
        const auto* gui = static_cast<const clap_plugin_gui*>(inst.p->get_extension(inst.p, CLAP_EXT_GUI));
        struct Mem { std::vector<uint8_t> b; size_t r = 0; } mem;
        clap_ostream os{&mem, [](const clap_ostream* s, const void* d, uint64_t n) -> int64_t {
            auto* m = static_cast<Mem*>(s->ctx);
            m->b.insert(m->b.end(), static_cast<const uint8_t*>(d), static_cast<const uint8_t*>(d) + n);
            return static_cast<int64_t>(n);
        }};
        clap_istream is{&mem, [](const clap_istream* s, void* d, uint64_t n) -> int64_t {
            auto* m = static_cast<Mem*>(s->ctx);
            const size_t k = std::min<size_t>(n, m->b.size() - m->r);
            std::memcpy(d, m->b.data() + m->r, k);
            m->r += k;
            return static_cast<int64_t>(k);
        }};
        uint32_t latOff = 9999, w = 0, h = 0;
        bool okCalls = true;
        double gainAfter = 0.0;
        std::thread t([&] {
            latOff = inst.lat->get(inst.p);
            double v = 0;
            char txt[64];
            okCalls &= params->get_value(inst.p, 1, &v);
            okCalls &= params->value_to_text(inst.p, 1, v, txt, sizeof(txt));
            okCalls &= params->text_to_value(inst.p, 1, "6", &v);
            clap_audio_port_info pi{};
            okCalls &= ports->count(inst.p, true) == 1 && ports->get(inst.p, 0, true, &pi);
            okCalls &= gui->get_size(inst.p, &w, &h);
            okCalls &= state->save(inst.p, &os);
            // Patch the saved gain (param 1) to +6 dB and load it back, all off the main thread.
            double six = 6.0;
            std::memcpy(mem.b.data() + 8 + 1 * sizeof(double), &six, sizeof(double));
            okCalls &= state->load(inst.p, &is);
        });
        t.join();
        const int rescansBefore = inst.th.rescans;
        inst.p->on_main_thread(inst.p);  // deferred params.rescan happens here
        EventList none;
        inst.run(std::vector<float>(inst.block, 0.0f), none);  // audio thread applies the state
        params->get_value(inst.p, 1, &gainAfter);
        const bool ok = okCalls && latOff == 0 && w > 0 && h > 0 && rescansBefore == 0 && inst.th.rescans == 1 &&
                        gainAfter == 6.0;
        failures += !ok;
        std::printf("[%s] off-main-thread calls: latency.get %u, gui size %ux%u, calls ok %d, rescan deferred %d->%d, gain after state load %.1f dB\n",
                    ok ? "PASS" : "FAIL", latOff, w, h, okCalls, rescansBefore, inst.th.rescans, gainAfter);
    }

    entry->deinit();
    FreeLibrary(mod);
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
