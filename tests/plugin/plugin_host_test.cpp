// Minimal CLAP test host for the Am-Q plugin binary (Phase 1 skeleton checks).
//
//  1. Factory, descriptor, init.
//  2. Audio ports and latency.
//  3. Stereo pass-through is bit-exact (skeleton behaviour).
//  4. State save/load round trip; foreign data is rejected.
//  5. Parameters: 573 exposed, frozen ids, readable choices, automation, state round trip.
//  6. Off-main-thread calls a wrapped VST3 host may make (latency, audio ports, params, state,
//     GUI size) do not abort and return correct values (area 01, docs/plan/decisions.md D1).
//
// Usage: amq_plugin_host_test <Am-Q.clap>. Windows only for now (the macOS .clap is a bundle).
#include <clap/clap.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

namespace {

int gFailures = 0;

void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    gFailures += !ok;
}

struct TestHost {
    clap_host host{};
    clap_host_thread_check threadExt{};
    clap_host_latency latencyExt{};
    std::thread::id mainThread = std::this_thread::get_id();
    int latencyChanged = 0;

    TestHost() {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "amq-plugin-host-test";
        host.vendor = "Am-Q";
        host.url = "";
        host.version = "0.0.1";
        host.get_extension = [](const clap_host* h, const char* id) -> const void* {
            auto* self = static_cast<TestHost*>(h->host_data);
            if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK)) return &self->threadExt;
            if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &self->latencyExt;
            return nullptr;
        };
        host.request_restart = [](const clap_host*) {};
        host.request_process = [](const clap_host*) {};
        host.request_callback = [](const clap_host*) {};
        threadExt.is_main_thread = [](const clap_host* h) {
            return std::this_thread::get_id() == static_cast<TestHost*>(h->host_data)->mainThread;
        };
        threadExt.is_audio_thread = [](const clap_host*) { return true; };  // processes on main too
        latencyExt.changed = [](const clap_host* h) { ++static_cast<TestHost*>(h->host_data)->latencyChanged; };
    }
};

struct Mem {
    std::vector<uint8_t> b;
    size_t r = 0;
    clap_ostream os{this, [](const clap_ostream* s, const void* d, uint64_t n) -> int64_t {
                        auto* m = static_cast<Mem*>(s->ctx);
                        m->b.insert(m->b.end(), static_cast<const uint8_t*>(d), static_cast<const uint8_t*>(d) + n);
                        return static_cast<int64_t>(n);
                    }};
    clap_istream is{this, [](const clap_istream* s, void* d, uint64_t n) -> int64_t {
                        auto* m = static_cast<Mem*>(s->ctx);
                        const size_t k = static_cast<size_t>(std::min<uint64_t>(n, m->b.size() - m->r));
                        std::memcpy(d, m->b.data() + m->r, k);
                        m->r += k;
                        return static_cast<int64_t>(k);
                    }};
};

template <typename T>
const T* ext(const clap_plugin* p, const char* id) {
    return static_cast<const T*>(p->get_extension(p, id));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: amq_plugin_host_test <Am-Q.clap>\n");
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

    // 1. Factory, descriptor, init.
    auto* f = static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    check(f && f->get_plugin_count(f) == 1, "factory has one plugin");
    const auto* d = f->get_plugin_descriptor(f, 0);
    check(d && d->id && std::strlen(d->id) > 0, "descriptor has an id");
    TestHost th;
    const clap_plugin* p = f->create_plugin(f, &th.host, d->id);
    check(p && p->init(p), "create and init");

    // 2. Audio ports and latency.
    const auto* ports = ext<clap_plugin_audio_ports>(p, CLAP_EXT_AUDIO_PORTS);
    const auto* lat = ext<clap_plugin_latency>(p, CLAP_EXT_LATENCY);
    const auto* state = ext<clap_plugin_state>(p, CLAP_EXT_STATE);
    const auto* gui = ext<clap_plugin_gui>(p, CLAP_EXT_GUI);
    clap_audio_port_info pi{};
    check(ports && ports->count(p, true) == 1 && ports->get(p, 0, true, &pi) && pi.channel_count == 2,
          "one stereo main input");
    check(ports && ports->count(p, false) == 1 && ports->get(p, 0, false, &pi) && pi.channel_count == 2,
          "one stereo main output");
    check(lat && state && gui, "latency, state and gui extensions present");

    // 3. Pass-through, separate buffers, odd block size.
    const uint32_t n = 333;
    check(p->activate(p, 48000.0, 1, 4096) && p->start_processing(p), "activate and start processing");
    check(lat->get(p) == 0, "latency 0 after activate");
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<float> inL(n), inR(n), outL(n, 9.0f), outR(n, 9.0f);
    for (uint32_t i = 0; i < n; ++i) inL[i] = u(rng), inR[i] = u(rng);
    float* ins[2] = {inL.data(), inR.data()};
    float* outs[2] = {outL.data(), outR.data()};
    clap_audio_buffer ib{}, ob{};
    ib.data32 = ins;
    ib.channel_count = 2;
    ob.data32 = outs;
    ob.channel_count = 2;
    clap_input_events inEv{nullptr, [](const clap_input_events*) -> uint32_t { return 0; },
                           [](const clap_input_events*, uint32_t) -> const clap_event_header* { return nullptr; }};
    clap_output_events outEv{nullptr, [](const clap_output_events*, const clap_event_header*) { return true; }};
    clap_process pr{};
    pr.steady_time = -1;
    pr.frames_count = n;
    pr.audio_inputs = &ib;
    pr.audio_outputs = &ob;
    pr.audio_inputs_count = 1;
    pr.audio_outputs_count = 1;
    pr.in_events = &inEv;
    pr.out_events = &outEv;
    check(p->process(p, &pr) == CLAP_PROCESS_CONTINUE && inL == outL && inR == outR, "stereo pass-through is bit-exact");

    // 4. State round trip and rejection of foreign data.
    Mem saved;
    check(state->save(p, &saved.os) && saved.b.size() >= 12, "state save");
    Mem copy;
    copy.b = saved.b;
    check(state->load(p, &copy.is), "state load of our own chunk");
    Mem junk;
    junk.b = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    check(!state->load(p, &junk.is), "state load rejects foreign data");

    // 5. Parameters: table shape, frozen ids, readable choices, host automation, state round trip.
    const auto* params = ext<clap_plugin_params>(p, CLAP_EXT_PARAMS);
    check(params && params->count(p) == 573, "573 parameters exposed (24 bands x 23 fields + 21 globals)");
    bool idsUnique = true, rangesOk = true;
    std::vector<clap_id> ids;
    int bypassCount = 0;
    for (uint32_t i = 0; params && i < params->count(p); ++i) {
        clap_param_info info{};
        if (!params->get_info(p, i, &info)) { idsUnique = false; break; }
        if (std::find(ids.begin(), ids.end(), info.id) != ids.end() || info.id >= 0x80000000u) idsUnique = false;
        ids.push_back(info.id);
        rangesOk &= info.min_value < info.max_value && info.default_value >= info.min_value &&
                    info.default_value <= info.max_value;
        bypassCount += (info.flags & CLAP_PARAM_IS_BYPASS) != 0;
    }
    check(idsUnique && rangesOk && bypassCount == 1, "ids unique and below 2^31, ranges valid, one bypass parameter");
    constexpr clap_id kBand01Freq = 1279620887u;  // tests/params/host_ids.golden.txt
    double v = 0;
    check(params->get_value(p, kBand01Freq, &v) && std::abs(v - std::log2(1000.0)) < 1e-12, "band01.freq id frozen; default 1 kHz");
    clap_id shapeId = 0, gainId = 0;
    for (uint32_t i = 0; i < params->count(p); ++i) {
        clap_param_info info{};
        params->get_info(p, i, &info);
        if (!std::strcmp(info.name, "Band 1 Shape")) shapeId = info.id;
        if (!std::strcmp(info.name, "Band 1 Gain")) gainId = info.id;
    }
    char text[64] = {};
    check(shapeId && params->value_to_text(p, shapeId, 0.0, text, sizeof(text)) && !std::strcmp(text, "Bell"),
          "choice values are readable (Band 1 Shape 0 = Bell)");
    check(params->text_to_value(p, gainId, "+6 dB", &v) && v == 6.0, "text to value: +6 dB");
    clap_event_param_value pe{};
    pe.header = {sizeof(pe), 10, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
    pe.param_id = gainId;
    pe.note_id = -1;
    pe.port_index = pe.channel = pe.key = -1;
    pe.value = 7.5;
    struct One { const clap_event_header* e; } one{&pe.header};
    clap_input_events oneEv{&one, [](const clap_input_events*) -> uint32_t { return 1; },
                            [](const clap_input_events* l, uint32_t) { return static_cast<const One*>(l->ctx)->e; }};
    pr.in_events = &oneEv;
    p->process(p, &pr);
    pr.in_events = &inEv;
    check(params->get_value(p, gainId, &v) && v == 7.5, "host automation event reaches the table");
    Mem withGain;
    state->save(p, &withGain.os);
    const clap_plugin* p2 = f->create_plugin(f, &th.host, d->id);
    p2->init(p2);
    const auto* state2 = ext<clap_plugin_state>(p2, CLAP_EXT_STATE);
    const auto* params2 = ext<clap_plugin_params>(p2, CLAP_EXT_PARAMS);
    check(state2->load(p2, &withGain.is) && params2->get_value(p2, gainId, &v) && v == 7.5,
          "state round trip into a new instance keeps Band 1 Gain");
    p2->destroy(p2);

    // 5b. Sample-accurate automation through the CLAP event queue (area 01 R3, area 09):
    //     an Output Level change at offset k must first change the output at sample k.
    clap_id levelId = 0;
    for (uint32_t i = 0; i < params->count(p); ++i) {
        clap_param_info info{};
        params->get_info(p, i, &info);
        if (!std::strcmp(info.name, "Output Level")) levelId = info.id;
    }
    auto renderWith = [&](bool withEvent, uint32_t k) {
        const clap_plugin* q = f->create_plugin(f, &th.host, d->id);
        q->init(q);
        q->activate(q, 48000.0, 1, 4096);
        q->start_processing(q);
        std::vector<float> l(512), r(512);
        std::vector<float> xl = inL, xr = inR;
        xl.resize(512, 0.25f);
        xr.resize(512, -0.25f);
        float* qi[2] = {xl.data(), xr.data()};
        float* qo[2] = {l.data(), r.data()};
        clap_audio_buffer qib{}, qob{};
        qib.data32 = qi;
        qib.channel_count = 2;
        qob.data32 = qo;
        qob.channel_count = 2;
        clap_event_param_value lv{};
        lv.header = {sizeof(lv), k, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        lv.param_id = levelId;
        lv.note_id = -1;
        lv.port_index = lv.channel = lv.key = -1;
        lv.value = 0.5;  // host position 0.5 = about -3.6 dB
        One ev1{&lv.header};
        clap_input_events evIn{&ev1, [](const clap_input_events*) -> uint32_t { return 1; },
                               [](const clap_input_events* l2, uint32_t) { return static_cast<const One*>(l2->ctx)->e; }};
        clap_process qp{};
        qp.steady_time = -1;
        qp.frames_count = 512;
        qp.audio_inputs = &qib;
        qp.audio_outputs = &qob;
        qp.audio_inputs_count = 1;
        qp.audio_outputs_count = 1;
        qp.in_events = withEvent ? &evIn : &inEv;
        qp.out_events = &outEv;
        q->process(q, &qp);
        q->stop_processing(q);
        q->deactivate(q);
        q->destroy(q);
        return l;
    };
    bool exact = levelId != 0;
    for (uint32_t k : {0u, 1u, 37u, 255u, 511u}) {
        const auto a = renderWith(false, k), b = renderWith(true, k);
        int first = -1;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i] != b[i]) { first = static_cast<int>(i); break; }
        if (first != static_cast<int>(k)) {
            std::printf("  offset %u: first changed sample %d\n", k, first);
            exact = false;
        }
    }
    check(exact, "Output Level automation lands on the exact sample (offsets 0, 1, 37, 255, 511)");

    // 5c. 64-bit host buffers are converted at the edge and give the float result (area 01 R4).
    {
        const clap_plugin* q = f->create_plugin(f, &th.host, d->id);
        q->init(q);
        q->activate(q, 48000.0, 1, 4096);
        q->start_processing(q);
        std::vector<double> dl(n), dr(n), ol(n), orr(n);
        for (uint32_t i = 0; i < n; ++i) dl[i] = inL[i], dr[i] = inR[i];
        double* di[2] = {dl.data(), dr.data()};
        double* dout[2] = {ol.data(), orr.data()};
        clap_audio_buffer dib{}, dob{};
        dib.data64 = di;
        dib.channel_count = 2;
        dob.data64 = dout;
        dob.channel_count = 2;
        clap_process dp{};
        dp.steady_time = -1;
        dp.frames_count = n;
        dp.audio_inputs = &dib;
        dp.audio_outputs = &dob;
        dp.audio_inputs_count = 1;
        dp.audio_outputs_count = 1;
        dp.in_events = &inEv;
        dp.out_events = &outEv;
        q->process(q, &dp);
        bool same = true;
        for (uint32_t i = 0; i < n; ++i) same &= ol[i] == static_cast<double>(inL[i]) && orr[i] == static_cast<double>(inR[i]);
        check(same, "64-bit I/O pass-through equals the float path");
        q->stop_processing(q);
        q->deactivate(q);
        q->destroy(q);
    }

    // 6. Off-main-thread calls (VST3 hosts may make these through clap-wrapper).
    uint32_t latOff = 99, w = 0, h = 0, countOff = 0, paramCountOff = 0;
    bool saveOff = false, loadOff = false, sizeOff = false, paramOff = false;
    std::thread t([&] {
        latOff = lat->get(p);
        countOff = ports->count(p, true);
        paramCountOff = params->count(p);
        clap_param_info info{};
        double val = 0;
        char txt[64];
        paramOff = params->get_info(p, 3, &info) && params->get_value(p, info.id, &val) &&
                   params->value_to_text(p, info.id, val, txt, sizeof(txt)) &&
                   params->text_to_value(p, info.id, txt, &val);
        Mem m;
        saveOff = state->save(p, &m.os);
        loadOff = state->load(p, &m.is);
        sizeOff = gui->get_size(p, &w, &h);
    });
    t.join();
    check(latOff == 0 && countOff == 1 && paramCountOff == 573 && paramOff && saveOff && loadOff && sizeOff && w > 0 &&
              h > 0,
          "off-main-thread latency, audio-ports, params, state and gui-size calls");
    p->on_main_thread(p);  // deferred params.rescan
    check(params->get_value(p, gainId, &v) && v == 7.5, "off-thread state load kept the values");

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    entry->deinit();
    FreeLibrary(mod);
    std::printf("%s (%d failure%s)\n", gFailures ? "FAILED" : "ALL PASSED", gFailures, gFailures == 1 ? "" : "s");
    return gFailures ? 1 : 0;
}
