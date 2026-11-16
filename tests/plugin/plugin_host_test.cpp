// Minimal CLAP test host for the Am-Q plugin binary (Phase 1 skeleton checks).
//
//  1. Factory, descriptor, init.
//  2. Audio ports and latency.
//  3. Stereo pass-through is bit-exact (skeleton behaviour).
//  4. State save/load round trip; foreign data is rejected.
//  5. Parameters: 573 exposed, frozen ids, readable choices, automation, state round trip,
//     shape ids pinned by a frozen state chunk (area 15 R7).
//  6. Off-main-thread calls a wrapped VST3 host may make (latency, audio ports, params, state,
//     GUI size) do not abort and return correct values (area 01, docs/plan/decisions.md D1).
//  7. Processing mode: switching to Natural Phase requests one restart; after it the latency
//     is 320, latency.changed() was called in activate, and a flat impulse comes out 320 samples
//     late; switching back restores 0 (area 02 R9, R12, R13).
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
#include <fstream>
#include <iterator>
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
    clap_host_params paramsExt{};
    int textRescans = 0;
    std::thread::id mainThread = std::this_thread::get_id();
    int latencyChanged = 0;
    int restartRequests = 0;

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
            if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &self->paramsExt;
            return nullptr;
        };
        host.request_restart = [](const clap_host* h) { ++static_cast<TestHost*>(h->host_data)->restartRequests; };
        host.request_process = [](const clap_host*) {};
        host.request_callback = [](const clap_host*) {};
        threadExt.is_main_thread = [](const clap_host* h) {
            return std::this_thread::get_id() == static_cast<TestHost*>(h->host_data)->mainThread;
        };
        threadExt.is_audio_thread = [](const clap_host*) { return true; };  // processes on main too
        latencyExt.changed = [](const clap_host* h) { ++static_cast<TestHost*>(h->host_data)->latencyChanged; };
        paramsExt.rescan = [](const clap_host* h, clap_param_rescan_flags flags) {
            if (flags & CLAP_PARAM_RESCAN_TEXT) ++static_cast<TestHost*>(h->host_data)->textRescans;
        };
        paramsExt.clear = [](const clap_host*, clap_id, clap_param_clear_flags) {};
        paramsExt.request_flush = [](const clap_host*) {};
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
    check(ports && ports->count(p, true) == 2 && ports->get(p, 0, true, &pi) && pi.channel_count == 2 &&
              (pi.flags & CLAP_AUDIO_PORT_IS_MAIN) && pi.id == 0,
          "two inputs: the first is the stereo main input");
    {
        clap_audio_port_info sc{};
        check(ports && ports->get(p, 1, true, &sc) && sc.channel_count == 2 && !(sc.flags & CLAP_AUDIO_PORT_IS_MAIN) &&
                  sc.in_place_pair == CLAP_INVALID_ID && sc.id != 0 && sc.id != 1 &&
                  std::strcmp(sc.port_type, CLAP_PORT_STEREO) == 0,
              "the second input is a stereo, non-main side-chain bus (area 01 R2)");
        check(ports && !ports->get(p, 2, true, &sc), "no third input");
    }
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

    // Output Pan text follows Pan Mode (area 04 R6); a mode change asks the host to rescan text.
    clap_id panId = 0, panModeId = 0;
    for (uint32_t i = 0; i < params->count(p); ++i) {
        clap_param_info info{};
        params->get_info(p, i, &info);
        if (!std::strcmp(info.name, "Output Pan")) panId = info.id;
        if (!std::strcmp(info.name, "Pan Mode")) panModeId = info.id;
    }
    check(panId && panModeId && params->value_to_text(p, panId, 50.0, text, sizeof(text)) && !std::strcmp(text, "50.0 R"),
          "Output Pan text in L/R mode: 50.0 R");
    pe.param_id = panModeId;
    pe.value = 1.0;
    pr.in_events = &oneEv;
    p->process(p, &pr);
    pr.in_events = &inEv;
    const int rescansBefore = th.textRescans;
    p->on_main_thread(p);
    check(th.textRescans == rescansBefore + 1 && params->value_to_text(p, panId, 50.0, text, sizeof(text)) &&
              !std::strcmp(text, "50.0 S"),
          "Pan Mode M/S: one text rescan, Output Pan reads 50.0 S");
    p->on_main_thread(p);
    check(th.textRescans == rescansBefore + 1, "no further text rescans while Pan Mode is unchanged");
    pe.value = 0.0;
    pr.in_events = &oneEv;
    p->process(p, &pr);
    pr.in_events = &inEv;
    p->on_main_thread(p);
    check(th.textRescans == rescansBefore + 2 && params->value_to_text(p, panId, 50.0, text, sizeof(text)) &&
              !std::strcmp(text, "50.0 R"),
          "Pan Mode back to L/R: another text rescan");
    pe.param_id = gainId;
    Mem withGain;
    state->save(p, &withGain.os);
    {  // A load while processing is one batch (area 21 R14): the host sees it at once, the audio
       // thread applies it whole at the next block.
        pe.param_id = gainId;
        pe.value = -3.0;
        pr.in_events = &oneEv;
        p->process(p, &pr);
        pr.in_events = &inEv;
        Mem again;
        again.b = withGain.b;
        check(state->load(p, &again.is) && params->get_value(p, gainId, &v) && v == 7.5,
              "state load while processing: the host reads the loaded value at once");
        p->process(p, &pr);
        Mem after;
        state->save(p, &after.os);
        check(after.b == withGain.b, "the next block applied the whole state");
        p->on_main_thread(p);
    }
    const clap_plugin* p2 = f->create_plugin(f, &th.host, d->id);
    p2->init(p2);
    const auto* state2 = ext<clap_plugin_state>(p2, CLAP_EXT_STATE);
    const auto* params2 = ext<clap_plugin_params>(p2, CLAP_EXT_PARAMS);
    check(state2->load(p2, &withGain.is) && params2->get_value(p2, gainId, &v) && v == 7.5,
          "state round trip into a new instance keeps Band 1 Gain");
    p2->destroy(p2);

    // 5a. Shape ids are pinned (area 15 R7). The frozen chunk tests/plugin/fixtures/
    //     shape_ids_v2.amqs holds shape id k - 1 on band k (1..10) and nothing for bands 11..24.
    //     Every later build must load each id as the same shape, and save it back unchanged.
    {
        static const char* const kShapeNames[] = {"Bell",      "Low Shelf", "Low Cut",    "High Shelf", "High Cut",
                                                  "Notch",     "Band Pass", "Tilt Shelf", "Flat Tilt",  "All Pass"};
        Mem fixture;
        {
            std::ifstream in(AMQ_SHAPE_FIXTURE, std::ios::binary);
            fixture.b.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        check(fixture.b.size() == 12 + 10 * 12, "shape-id fixture read (132 bytes)");
        const clap_plugin* p3 = f->create_plugin(f, &th.host, d->id);
        p3->init(p3);
        const auto* state3 = ext<clap_plugin_state>(p3, CLAP_EXT_STATE);
        const auto* params3 = ext<clap_plugin_params>(p3, CLAP_EXT_PARAMS);
        check(state3->load(p3, &fixture.is), "shape-id fixture loads");
        std::vector<clap_id> shapeIds;
        bool rangeOk = true;
        for (uint32_t i = 0; i < params3->count(p3); ++i) {
            clap_param_info info{};
            params3->get_info(p3, i, &info);
            const size_t len = std::strlen(info.name);
            if (len > 6 && !std::strcmp(info.name + len - 6, " Shape") && !std::strncmp(info.name, "Band ", 5)) {
                shapeIds.push_back(info.id);
                // Shapes may be appended later, never removed or reordered.
                rangeOk &= info.min_value == 0.0 && info.max_value >= 9.0 && info.default_value == 0.0 &&
                           (info.flags & CLAP_PARAM_IS_STEPPED);
            }
        }
        check(shapeIds.size() == 24 && rangeOk, "24 stepped band Shape parameters, ids 0..9 or more, default Bell");
        bool valuesOk = shapeIds.size() == 24, namesOk = valuesOk;
        for (size_t b = 0; valuesOk && b < 24; ++b) {
            const double want = b < 10 ? static_cast<double>(b) : 0.0;
            double got = -1;
            char name[64] = {};
            valuesOk &= params3->get_value(p3, shapeIds[b], &got) && got == want;
            namesOk &= params3->value_to_text(p3, shapeIds[b], got, name, sizeof(name)) &&
                       !std::strcmp(name, kShapeNames[static_cast<size_t>(want)]);
            if (!namesOk || !valuesOk) std::printf("  band %zu: value %g, text \"%s\"\n", b + 1, got, name);
        }
        check(valuesOk, "fixture: band k loads shape id k - 1; absent bands load Bell");
        check(namesOk, "fixture: each shape id names the same shape (Bell .. All Pass)");
        Mem resaved;
        state3->save(p3, &resaved.os);
        int found = 0;
        bool sameValues = true;
        for (size_t k = 0; k < 10; ++k) {
            const uint8_t* want = fixture.b.data() + 12 + 12 * k;
            for (size_t off = 12; off + 12 <= resaved.b.size(); off += 12)
                if (!std::memcmp(resaved.b.data() + off, want, 4)) {
                    ++found;
                    sameValues &= !std::memcmp(resaved.b.data() + off + 4, want + 4, 8);
                }
        }
        check(found == 10 && sameValues, "fixture: saving writes each shape pair back bit-identical");
        p3->destroy(p3);
    }

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

    // 5d. Side chain feeds only the side chain (area 01 acceptance test): with dynamics off, a
    // loud side chain leaves the main output bit-exact, whether 32-bit, 64-bit, mono or absent.
    {
        std::vector<float> sl(n), sr(n), o1(n), o2(n);
        std::vector<double> dsl(n), dsr(n);
        for (uint32_t i = 0; i < n; ++i) sl[i] = sr[i] = 0.99f * ((i & 1) ? 1.0f : -1.0f), dsl[i] = dsr[i] = sl[i];
        float* scs[2] = {sl.data(), sr.data()};
        double* dscs[2] = {dsl.data(), dsr.data()};
        float* outs2[2] = {o1.data(), o2.data()};
        auto runWith = [&](clap_audio_buffer sc) {
            clap_audio_buffer bufs[2] = {ib, sc};
            clap_audio_buffer ob2{};
            ob2.data32 = outs2;
            ob2.channel_count = 2;
            clap_process sp = pr;
            sp.audio_inputs = bufs;
            sp.audio_inputs_count = 2;
            sp.audio_outputs = &ob2;
            std::fill(o1.begin(), o1.end(), 9.0f);
            std::fill(o2.begin(), o2.end(), 9.0f);
            return p->process(p, &sp) == CLAP_PROCESS_CONTINUE && o1 == inL && o2 == inR;
        };
        clap_audio_buffer s32{}, s64{}, smono{}, sempty{}, snull{};
        s32.data32 = scs;
        s32.channel_count = 2;
        s64.data64 = dscs;
        s64.channel_count = 2;
        smono.data32 = scs;
        smono.channel_count = 1;
        snull.channel_count = 2;  // inactive bus: no buffers
        check(runWith(s32), "32-bit side chain: main output unchanged");
        check(runWith(s64), "64-bit side chain: main output unchanged");
        check(runWith(smono), "mono side chain: main output unchanged");
        check(runWith(sempty) && runWith(snull), "absent or inactive side chain: main output unchanged");
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
    check(latOff == 0 && countOff == 2 && paramCountOff == 573 && paramOff && saveOff && loadOff && sizeOff && w > 0 &&
              h > 0,
          "off-main-thread latency, audio-ports, params, state and gui-size calls");
    p->on_main_thread(p);  // deferred params.rescan
    check(params->get_value(p, gainId, &v) && v == 7.5, "off-thread state load kept the values");

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);

    // 7. Processing mode and latency (area 02 R9, R12, R13).
    {
        TestHost mh;
        const clap_plugin* q = f->create_plugin(f, &mh.host, d->id);
        q->init(q);
        const auto* qlat = ext<clap_plugin_latency>(q, CLAP_EXT_LATENCY);
        const auto* qparams = ext<clap_plugin_params>(q, CLAP_EXT_PARAMS);
        clap_id modeId = 0;
        for (uint32_t i = 0; i < qparams->count(q); ++i) {
            clap_param_info info{};
            qparams->get_info(q, i, &info);
            if (!std::strcmp(info.name, "Processing Mode")) modeId = info.id;
        }
        double npValue = -1;
        check(modeId && qparams->text_to_value(q, modeId, "Natural Phase", &npValue), "Processing Mode found; \"Natural Phase\" parses");
        q->activate(q, 48000.0, 1, 4096);
        q->start_processing(q);
        // Runs one 1024-sample block with an optional mode event, returns the left output.
        auto block = [&](const clap_event_param_value* ev, bool impulse) {
            std::vector<float> l(1024, 0.0f), r(1024, 0.0f), ol(1024), orr(1024);
            if (impulse) l[0] = 1.0f;
            float* qi[2] = {l.data(), r.data()};
            float* qo[2] = {ol.data(), orr.data()};
            clap_audio_buffer qib{}, qob{};
            qib.data32 = qi;
            qib.channel_count = 2;
            qob.data32 = qo;
            qob.channel_count = 2;
            One e1{ev ? &ev->header : nullptr};
            clap_input_events evIn{&e1, [](const clap_input_events* l2) -> uint32_t { return static_cast<const One*>(l2->ctx)->e ? 1u : 0u; },
                                   [](const clap_input_events* l2, uint32_t) { return static_cast<const One*>(l2->ctx)->e; }};
            clap_process qp{};
            qp.steady_time = -1;
            qp.frames_count = 1024;
            qp.audio_inputs = &qib;
            qp.audio_outputs = &qob;
            qp.audio_inputs_count = 1;
            qp.audio_outputs_count = 1;
            qp.in_events = &evIn;
            qp.out_events = &outEv;
            q->process(q, &qp);
            return ol;
        };
        auto modeEvent = [&](double value) {
            clap_event_param_value mv{};
            mv.header = {sizeof(mv), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            mv.param_id = modeId;
            mv.note_id = -1;
            mv.port_index = mv.channel = mv.key = -1;
            mv.value = value;
            return mv;
        };
        auto restart = [&] {
            q->stop_processing(q);
            q->deactivate(q);
            q->activate(q, 48000.0, 1, 4096);
            q->start_processing(q);
        };
        check(qlat->get(q) == 0, "Zero Latency: latency 0");
        const auto toNp = modeEvent(npValue);
        block(&toNp, false);
        block(nullptr, false);
        check(mh.restartRequests == 1, "switching to Natural Phase requests exactly one restart");
        const int changedBefore = mh.latencyChanged;
        restart();
        check(qlat->get(q) == 320 && mh.latencyChanged == changedBefore + 1,
              "after the restart: latency 320, latency.changed() called in activate");
        block(nullptr, false);
        const auto y = block(nullptr, true);
        int peak = 0;
        for (int i = 1; i < 1024; ++i)
            if (std::abs(y[static_cast<size_t>(i)]) > std::abs(y[static_cast<size_t>(peak)])) peak = i;
        if (peak != 320 || std::abs(y[320] - 1.0f) > 1e-6f) std::printf("  impulse peak at %d, value %g\n", peak, y[static_cast<size_t>(peak)]);
        check(peak == 320 && std::abs(y[320] - 1.0f) <= 1e-6f, "Natural Phase, bands flat: impulse exactly 320 samples late");
        block(nullptr, false);
        check(mh.restartRequests == 1, "no further restart requests while the mode is unchanged");

        // Host bypass (area 01 R6, area 09 R7): the CLAP bypass parameter crossfades to the dry
        // input delayed by the latency. Output Level at 0.25 changes the processed signal only.
        clap_id bypassId = 0, levelId2 = 0;
        for (uint32_t i = 0; i < qparams->count(q); ++i) {
            clap_param_info info{};
            qparams->get_info(q, i, &info);
            if (info.flags & CLAP_PARAM_IS_BYPASS) bypassId = info.id;
            if (!std::strcmp(info.name, "Output Level")) levelId2 = info.id;
        }
        auto event = [&](clap_id id, double value) {
            auto ev = modeEvent(value);
            ev.param_id = id;
            return ev;
        };
        const auto quiet = event(levelId2, 0.25);
        block(&quiet, false);
        block(nullptr, false);
        const auto wetImp = block(nullptr, true);
        const auto bypassOn = event(bypassId, 1.0);
        block(&bypassOn, false);
        block(nullptr, false);
        const auto dryImp = block(nullptr, true);
        bool dryExact = true;
        for (int i = 0; i < 1024; ++i) dryExact &= dryImp[static_cast<size_t>(i)] == (i == 320 ? 1.0f : 0.0f);
        check(bypassId && std::abs(wetImp[320]) < 0.5f && dryExact,
              "host bypass: the output becomes the input delayed by 320 samples, bit for bit");
        const auto bypassOff = event(bypassId, 0.0);
        block(&bypassOff, false);
        block(nullptr, false);
        check(std::abs(block(nullptr, true)[320] - wetImp[320]) < 1e-6f, "bypass off: the processed signal returns");
        check(mh.restartRequests == 1, "bypass never asks for a restart");
        const auto toZl = modeEvent(0.0);
        block(&toZl, false);
        restart();
        check(mh.restartRequests == 2 && qlat->get(q) == 0, "switching back: one more restart, latency 0");
        q->stop_processing(q);
        q->deactivate(q);
        q->destroy(q);
    }

    entry->deinit();
    FreeLibrary(mod);
    std::printf("%s (%d failure%s)\n", gFailures ? "FAILED" : "ALL PASSED", gFailures, gFailures == 1 ? "" : "s");
    return gFailures ? 1 : 0;
}
