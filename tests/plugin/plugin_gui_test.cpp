// End-to-end editor test in a real window (Windows): mouse messages to the Visage child
// window must reach the host as CLAP gesture and value events, and opening and closing the
// editor must not leak, and the editor must resize and keep its size across reopen
// (area 09 R4, area 12 R1/R16, area 13 R3/R9/R18/R22).
//
// Usage: amq_plugin_gui_test <Am-Q.clap>
#include <clap/clap.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

int gFailures = 0;
void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    gFailures += !ok;
}

int gFlushRequests = 0, gCallbackRequests = 0;
clap_host gHost{};
clap_host_params gParamsExt{};
HWND gChild = nullptr;

void pump(DWORD ms) {
    const DWORD t0 = GetTickCount();
    MSG m;
    do {
        while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
        Sleep(1);
    } while (GetTickCount() - t0 < ms);
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return DefWindowProcW(h, msg, wp, lp); }
BOOL CALLBACK findChild(HWND h, LPARAM) {
    gChild = h;
    return FALSE;
}

struct Events {
    std::vector<std::vector<uint8_t>> evs;
    clap_output_events out{this, [](const clap_output_events* l, const clap_event_header* e) {
                               auto* self = static_cast<Events*>(l->ctx);
                               const auto* b = reinterpret_cast<const uint8_t*>(e);
                               self->evs.emplace_back(b, b + e->size);
                               return true;
                           }};
    int count(uint16_t type) const {
        int n = 0;
        for (const auto& e : evs) n += reinterpret_cast<const clap_event_header*>(e.data())->type == type;
        return n;
    }
};

// Graph geometry of the editor (docs/design/editor-spec.md §1.1; ui/include/amq/ui/editor_layout.hpp
// and graph_model.hpp): the graph sits between the 44 px bars, its plot runs from the left edge to
// a 100 px gutter with 24 px top and bottom insets, over 10 Hz to 30 kHz and +/-12 dB.
double kW = 1280, kH = 720;  // logical editor size; changed by the resize test
constexpr double kBar = 44, kInset = 24, kGutter = 100, kRange = 12;
double xOfHz(double hz) { return (kW - kGutter) * std::log(hz / 10.0) / std::log(3000.0); }
double yOfDb(double db) {
    const double plotH = kH - 2 * kBar - 2 * kInset;
    return kBar + kInset + 0.5 * plotH - db / kRange * 0.5 * plotH;
}

// GPU memory this process holds on every adapter (DXGI 1.4 QueryVideoMemoryInfo, local and
// non-local segments): the GPU-object counter for the editor soak (area 12 R1, R8). Visage's GPU
// objects are internal; their memory is what a leak would show. 0 if DXGI 1.4 is unavailable.
uint64_t gpuBytes() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return 0;
    uint64_t total = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        IDXGIAdapter3* a3 = nullptr;
        if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3)))) {
            for (auto group : {DXGI_MEMORY_SEGMENT_GROUP_LOCAL, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL}) {
                DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, group, &info))) total += info.CurrentUsage;
            }
            a3->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return total;
}

size_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return pmc.PrivateUsage;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HMODULE mod = LoadLibraryA(argv[1]);
    auto* entry = mod ? reinterpret_cast<const clap_plugin_entry*>(GetProcAddress(mod, "clap_entry")) : nullptr;
    if (!entry || !entry->init(argv[1])) {
        std::printf("FAIL: load\n");
        return 1;
    }
    gHost.clap_version = CLAP_VERSION;
    gHost.name = "amq-plugin-gui-test";
    gHost.vendor = gHost.url = "";
    gHost.version = "0";
    gHost.get_extension = [](const clap_host*, const char* id) -> const void* {
        return std::strcmp(id, CLAP_EXT_PARAMS) == 0 ? &gParamsExt : nullptr;
    };
    gHost.request_restart = gHost.request_process = [](const clap_host*) {};
    gHost.request_callback = [](const clap_host*) { ++gCallbackRequests; };
    gParamsExt.rescan = [](const clap_host*, clap_param_rescan_flags) {};
    gParamsExt.clear = [](const clap_host*, clap_id, clap_param_clear_flags) {};
    gParamsExt.request_flush = [](const clap_host*) { ++gFlushRequests; };

    auto* f = static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    const clap_plugin* p = f->create_plugin(f, &gHost, f->get_plugin_descriptor(f, 0)->id);
    p->init(p);
    p->activate(p, 48000.0, 1, 512);
    p->start_processing(p);
    auto* params = static_cast<const clap_plugin_params*>(p->get_extension(p, CLAP_EXT_PARAMS));
    auto* gui = static_cast<const clap_plugin_gui*>(p->get_extension(p, CLAP_EXT_GUI));
    clap_id used1 = 0, freq1 = 0, gain1 = 0;
    for (uint32_t i = 0; i < params->count(p); ++i) {
        clap_param_info info{};
        params->get_info(p, i, &info);
        if (!std::strcmp(info.name, "Band 1 Used")) used1 = info.id;
        if (!std::strcmp(info.name, "Band 1 Frequency")) freq1 = info.id;
        if (!std::strcmp(info.name, "Band 1 Gain")) gain1 = info.id;
    }

    // One process block, collecting the plugin's output events.
    std::vector<float> l(512), r(512), ol(512), orr(512);
    float* ins[2] = {l.data(), r.data()};
    float* outs[2] = {ol.data(), orr.data()};
    auto processOnce = [&](Events& ev, const clap_input_events* in = nullptr) {
        clap_audio_buffer ib{}, ob{};
        ib.data32 = ins;
        ib.channel_count = 2;
        ob.data32 = outs;
        ob.channel_count = 2;
        clap_input_events empty{nullptr, [](const clap_input_events*) -> uint32_t { return 0; },
                                [](const clap_input_events*, uint32_t) -> const clap_event_header* { return nullptr; }};
        clap_process pr{};
        pr.steady_time = -1;
        pr.frames_count = 512;
        pr.audio_inputs = &ib;
        pr.audio_outputs = &ob;
        pr.audio_inputs_count = pr.audio_outputs_count = 1;
        pr.in_events = in ? in : &empty;
        pr.out_events = &ev.out;
        p->process(p, &pr);
    };

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AmQGuiTest";
    RegisterClassW(&wc);
    HWND top = CreateWindowW(wc.lpszClassName, L"Am-Q GUI test", WS_OVERLAPPEDWINDOW, 100, 100, 1340, 800, nullptr,
                             nullptr, wc.hInstance, nullptr);
    ShowWindow(top, SW_SHOW);
    check(gui->create(p, CLAP_WINDOW_API_WIN32, false), "gui.create");
    clap_window win{CLAP_WINDOW_API_WIN32, {top}};
    gui->set_parent(p, &win);
    gui->show(p);
    pump(800);
    EnumChildWindows(top, findChild, 0);
    check(gChild != nullptr, "editor child window exists");
    const double scale = GetDpiForWindow(top) / 96.0;
    auto post = [&](UINT msg, WPARAM wp, double x, double y) {
        PostMessageW(gChild, msg, wp, MAKELPARAM(static_cast<int>(std::lround(x * scale)), static_cast<int>(std::lround(y * scale))));
    };

    // 1. Double-click at 1 kHz, +6 dB creates band 1 there, as one gesture.
    const double cx = xOfHz(1000.0), cy = yOfDb(6.0);
    post(WM_MOUSEMOVE, 0, cx, cy);
    pump(30);
    post(WM_LBUTTONDOWN, MK_LBUTTON, cx, cy);
    post(WM_LBUTTONUP, 0, cx, cy);
    pump(30);
    post(WM_LBUTTONDOWN, MK_LBUTTON, cx, cy);
    post(WM_LBUTTONUP, 0, cx, cy);
    pump(200);
    Events e1;
    processOnce(e1);
    double used = 0, hz = 0, db = 0;
    params->get_value(p, used1, &used);
    params->get_value(p, freq1, &hz);
    params->get_value(p, gain1, &db);
    hz = std::exp2(hz);
    std::printf("  after double-click: used %.0f, %.1f Hz, %+.2f dB; events: %d begin, %d value, %d end; %d flush requests\n",
                used, hz, db, e1.count(CLAP_EVENT_PARAM_GESTURE_BEGIN), e1.count(CLAP_EVENT_PARAM_VALUE),
                e1.count(CLAP_EVENT_PARAM_GESTURE_END), gFlushRequests);
    check(used == 1.0 && std::abs(hz / 1000.0 - 1.0) < 0.02 && std::abs(db - 6.0) < 0.3,
          "double-click creates band 1 at the pointer (1 kHz, +6 dB)");
    check(e1.count(CLAP_EVENT_PARAM_GESTURE_BEGIN) == 23 && e1.count(CLAP_EVENT_PARAM_GESTURE_END) == 23 &&
              e1.count(CLAP_EVENT_PARAM_VALUE) >= 3 && gFlushRequests > 0,
          "band creation reaches the host as one gesture over all 23 fields");

    // 2. Drag the node to 3 kHz, -3 dB: frequency and gain gestures, values in between.
    const double tx = xOfHz(3000.0), ty = yOfDb(-3.0);
    post(WM_MOUSEMOVE, 0, cx, cy);
    pump(30);
    post(WM_LBUTTONDOWN, MK_LBUTTON, cx, cy);
    pump(30);
    for (int i = 1; i <= 20; ++i) {
        post(WM_MOUSEMOVE, MK_LBUTTON, cx + (tx - cx) * i / 20.0, cy + (ty - cy) * i / 20.0);
        pump(5);
    }
    post(WM_LBUTTONUP, 0, tx, ty);
    pump(200);
    Events e2;
    processOnce(e2);
    params->get_value(p, freq1, &hz);
    params->get_value(p, gain1, &db);
    hz = std::exp2(hz);
    std::printf("  after drag: %.1f Hz, %+.2f dB; events: %d begin, %d value, %d end\n", hz, db,
                e2.count(CLAP_EVENT_PARAM_GESTURE_BEGIN), e2.count(CLAP_EVENT_PARAM_VALUE),
                e2.count(CLAP_EVENT_PARAM_GESTURE_END));
    check(std::abs(hz / 3000.0 - 1.0) < 0.02 && std::abs(db + 3.0) < 0.3, "drag moves band 1 to 3 kHz, -3 dB");
    check(e2.count(CLAP_EVENT_PARAM_GESTURE_BEGIN) == 2 && e2.count(CLAP_EVENT_PARAM_GESTURE_END) == 2 &&
              e2.count(CLAP_EVENT_PARAM_VALUE) >= 10,
          "drag is one begin and one end per parameter (frequency, gain) with values in between");

    // 3. Host automation while the editor is open asks for a main-thread refresh.
    clap_event_param_value pv{};
    pv.header = {sizeof(pv), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
    pv.param_id = gain1;
    pv.note_id = -1;
    pv.port_index = pv.channel = pv.key = -1;
    pv.value = -9.0;
    struct One { const clap_event_header* e; } one{&pv.header};
    clap_input_events in{&one, [](const clap_input_events*) -> uint32_t { return 1; },
                         [](const clap_input_events* list, uint32_t) { return static_cast<const One*>(list->ctx)->e; }};
    const int before = gCallbackRequests;
    Events e3;
    processOnce(e3, &in);
    check(gCallbackRequests == before + 1, "host automation with the editor open requests one GUI refresh");
    p->on_main_thread(p);
    pump(100);

    // 3b. The EQ Match panel opens lazily from its panel button (area 12) and draws its overlay; the
    // editor keeps running (no crash, still one live editor) and closes it again.
    {
        // The top bar's EQ Match button (spec §2.1); in EQ Match mode it sits at the right edge.
        const double bx = kW - 24 - 28 - 8 - 28 - 8 - 50, by = 22, bx2 = kW - 24 - 40;
        post(WM_LBUTTONDOWN, MK_LBUTTON, bx, by);
        post(WM_LBUTTONUP, 0, bx, by);
        pump(200);
        check(IsWindow(gChild) != 0, "EQ Match panel opens without disturbing the editor");
        post(WM_LBUTTONDOWN, MK_LBUTTON, bx2, by);
        post(WM_LBUTTONUP, 0, bx2, by);
        pump(100);
        check(IsWindow(gChild) != 0, "EQ Match panel closes again");
    }

    // 4. Resizing (area 12 R16): limits, set_size, layout follows, size kept across reopen.
    {
        check(gui->can_resize(p), "gui.can_resize");
        uint32_t w = 10, h = 10;
        gui->adjust_size(p, &w, &h);
        const uint32_t minW = static_cast<uint32_t>(std::lround(640 * scale)), minH = static_cast<uint32_t>(std::lround(360 * scale));
        std::printf("  adjust 10x10 -> %ux%u (min %ux%u at scale %.2f)\n", w, h, minW, minH, scale);
        check(w == minW && h == minH, "adjust_size clamps to the minimum size");
        w = 100000, h = 100000;
        gui->adjust_size(p, &w, &h);
        check(w == static_cast<uint32_t>(std::lround(3840 * scale)) && h == static_cast<uint32_t>(std::lround(2160 * scale)),
              "adjust_size clamps to the maximum size");
        w = static_cast<uint32_t>(std::lround(900 * scale));
        h = static_cast<uint32_t>(std::lround(500 * scale));
        gui->adjust_size(p, &w, &h);
        SetWindowPos(top, nullptr, 0, 0, static_cast<int>(w) + 40, static_cast<int>(h) + 80, SWP_NOMOVE | SWP_NOZORDER);
        check(gui->set_size(p, w, h), "gui.set_size 900x500 logical");
        pump(200);
        uint32_t gw = 0, gh = 0;
        gui->get_size(p, &gw, &gh);
        RECT rc{};
        GetClientRect(gChild, &rc);
        std::printf("  set %ux%u -> get_size %ux%u, child window %ldx%ld\n", w, h, gw, gh, rc.right, rc.bottom);
        check(gw == w && gh == h && rc.right == static_cast<LONG>(w) && rc.bottom == static_cast<LONG>(h),
              "get_size and the child window follow set_size");
        // The graph lays out at the new size: a double-click at 200 Hz, -6 dB lands there.
        kW = 900;
        kH = 500;
        clap_id freq2 = 0, gain2 = 0;
        for (uint32_t i = 0; i < params->count(p); ++i) {
            clap_param_info info{};
            params->get_info(p, i, &info);
            if (!std::strcmp(info.name, "Band 2 Frequency")) freq2 = info.id;
            if (!std::strcmp(info.name, "Band 2 Gain")) gain2 = info.id;
        }
        // Clear the selection first: while a band is selected the floating band panel covers the
        // lower plot (editor-spec §1.1).
        post(WM_LBUTTONDOWN, MK_LBUTTON, xOfHz(5000.0), yOfDb(10.0));
        post(WM_LBUTTONUP, 0, xOfHz(5000.0), yOfDb(10.0));
        pump(600);
        const double x2 = xOfHz(200.0), y2 = yOfDb(-6.0);
        post(WM_MOUSEMOVE, 0, x2, y2);
        pump(30);
        post(WM_LBUTTONDOWN, MK_LBUTTON, x2, y2);
        post(WM_LBUTTONUP, 0, x2, y2);
        pump(30);
        post(WM_LBUTTONDOWN, MK_LBUTTON, x2, y2);
        post(WM_LBUTTONUP, 0, x2, y2);
        pump(200);
        Events e4;
        processOnce(e4);
        double hz2 = 0, db2 = 0;
        params->get_value(p, freq2, &hz2);
        params->get_value(p, gain2, &db2);
        hz2 = std::exp2(hz2);
        std::printf("  after resize, double-click: band 2 at %.1f Hz, %+.2f dB\n", hz2, db2);
        check(std::abs(hz2 / 200.0 - 1.0) < 0.03 && std::abs(db2 + 6.0) < 0.4,
              "after resize, the graph maps the pointer at the new size (200 Hz, -6 dB)");
        gui->destroy(p);
        pump(50);
        gui->get_size(p, &gw, &gh);
        check(gw == w && gh == h, "get_size without an editor reports the last size");
        gui->create(p, CLAP_WINDOW_API_WIN32, false);
        gui->set_parent(p, &win);
        gui->show(p);
        pump(100);
        gui->get_size(p, &gw, &gh);
        std::printf("  reopened at %ux%u\n", gw, gh);
        check(gw == w && gh == h, "the editor reopens at its last size");
        gui->set_size(p, static_cast<uint32_t>(std::lround(1280 * scale)), static_cast<uint32_t>(std::lround(720 * scale)));
        pump(50);
        kW = 1280;
        kH = 720;
    }
    gui->destroy(p);
    pump(100);

    // 5. Idle CPU (area 12 R7): editor open, no edits, analyzer off. First with no audio, then
    // with silent audio at real-time block rate, so snapshots keep arriving at 60 Hz.
    {
        auto cpuSeconds = [] {
            FILETIME c, e, k, u;
            GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
            auto t = [](FILETIME f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32 | f.dwLowDateTime) * 1e-7; };
            return t(k) + t(u);
        };
        // The test host's own message loop (1 ms polling) costs CPU too: measure it without an
        // editor and report the editor's share as the difference.
        auto measure = [&](DWORD ms) {
            const double c = cpuSeconds();
            const DWORD t = GetTickCount();
            pump(ms);
            return (cpuSeconds() - c) / ((GetTickCount() - t) * 1e-3) * 100.0;
        };
        const double base = measure(10000);
        gui->create(p, CLAP_WINDOW_API_WIN32, false);
        gui->set_parent(p, &win);
        gui->show(p);
        pump(1000);
        const double open = measure(10000);
        const double idle = open - base;
        std::printf("  host loop alone %.2f %%, with the editor open %.2f %%\n", base, open);
        double c0 = cpuSeconds();
        DWORD t0 = GetTickCount();
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        c0 = cpuSeconds();
        t0 = GetTickCount();
        DWORD next = t0;
        while (GetTickCount() - t0 < 5000) {
            Events ev;
            processOnce(ev);  // 512 samples at 48 kHz: one block every 10.7 ms
            next += 11;
            while (static_cast<int>(next - GetTickCount()) > 0) pump(1);
        }
        const double silent = (cpuSeconds() - c0) / ((GetTickCount() - t0) * 1e-3) * 100.0 - base;
        std::printf("  idle CPU of the editor: %.2f %% of one core (no audio), %.2f %% (silent audio, incl. processing)\n",
                    idle, silent);
        // R7's target is under 1 %. A 10 s sample through GetProcessTimes is too coarse to prove
        // it (repeat runs vary by about 0.8 %), so this is a regression guard; the 60 s idle-CPU
        // benchmark (area 12, Phase 3) checks the target itself.
        check(idle < 2.0, "idle editor CPU regression guard: under 2 % of one core");
        gui->destroy(p);
        pump(100);
    }

    // 6. Open/close soak (area 12 R1): no growth in GDI/USER objects or memory.
    auto handles = [] {
        return GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) + GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    };
    //
    // Memory is measured in two parts, because the graphics driver dominates the raw numbers.
    // Visage releases the whole GPU device when the last editor closes (R8) and creates a new
    // one on the next open. A bare Direct3D 11 device + swapchain create/destroy loop, with no
    // Am-Q or Visage code, grows private memory by about 30 MB over the first 200-300 cycles and
    // then swings by +-5 MB (measured 2026-11-15). The editor alone shows the same curve, with
    // swings up to 10 MB, so its raw numbers cannot tell a leak from the driver.
    //   6a. Editor without a window: everything Am-Q builds per editor (frame tree, model,
    //       accessible tree, listeners), no GPU.
    //   6b. Attached and shown, while a second instance keeps its editor open, as when a host
    //       shows two Am-Q editors. The GPU device then lives through the soak, so each cycle
    //       creates and releases only the per-editor resources: window, swapchain, layers,
    //       textures, frame caches and the editor itself.
    // The device teardown itself (R8) is exercised by every other open/close in this test.
    constexpr double kMB = 1048576.0;
    auto cycles = [&](int n, bool attach) {
        for (int i = 0; i < n; ++i) {
            gui->create(p, CLAP_WINDOW_API_WIN32, false);
            if (attach) {
                gui->set_parent(p, &win);
                gui->show(p);
            }
            pump(10);
            gui->destroy(p);
        }
        pump(300);
    };

    // 6a.
    cycles(20, false);
    const size_t d0 = privateBytes();
    cycles(100, false);
    const size_t d1 = privateBytes();
    cycles(100, false);
    const size_t d2 = privateBytes();
    std::printf("  soak without a window: private memory %.2f -> %.2f -> %.2f MB\n", d0 / kMB, d1 / kMB, d2 / kMB);
    // The first batch may still fill allocator caches (up to 0.3 MB seen); the second must not grow.
    check(d2 < d1 + 128 * 1024, "second 100 create/destroy cycles without a window: memory growth under 128 KB");

    // 6b.
    const clap_plugin* holder = f->create_plugin(f, &gHost, f->get_plugin_descriptor(f, 0)->id);
    holder->init(holder);
    auto* holderGui = static_cast<const clap_plugin_gui*>(holder->get_extension(holder, CLAP_EXT_GUI));
    HWND holderTop = CreateWindowW(wc.lpszClassName, L"Am-Q GUI test (second editor)", WS_OVERLAPPEDWINDOW, 140, 140,
                                   900, 560, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(holderTop, SW_SHOWNOACTIVATE);
    clap_window holderWin{CLAP_WINDOW_API_WIN32, {holderTop}};
    holderGui->create(holder, CLAP_WINDOW_API_WIN32, false);
    holderGui->set_parent(holder, &holderWin);
    holderGui->show(holder);
    pump(300);
    // Warm-up: with the device held, memory rises about 1.4 MB and GDI/USER objects by 3 over the
    // first 250 cycles, then both stay flat (measured over a further 1000 cycles: within 0.5 MB,
    // objects unchanged). A leak keeps rising after it.
    const DWORD hw = handles();
    const size_t aw = privateBytes();
    cycles(250, true);
    const DWORD h0 = handles();
    const size_t a0 = privateBytes();
    const uint64_t g0 = gpuBytes();
    cycles(100, true);
    const size_t a1 = privateBytes();
    cycles(100, true);
    const size_t a2 = privateBytes();
    const uint64_t g2 = gpuBytes();
    const DWORD h1 = handles();
    std::printf("  soak attached (second editor open): warm-up %.2f -> %.2f MB, objects %lu -> %lu; then %.2f -> %.2f -> "
                "%.2f MB, objects %lu -> %lu\n",
                aw / kMB, a0 / kMB, hw, h0, a0 / kMB, a1 / kMB, a2 / kMB, h0, h1);
    check(h0 <= hw + 8, "attached warm-up (250 cycles): GDI/USER objects bounded (+8 at most)");
    check(h1 <= h0, "200 attached open/close cycles after warm-up: no GDI/USER object growth");
    check(a2 < a0 + 1024 * 1024, "200 attached open/close cycles after warm-up: memory growth under 1 MB (5 KB per cycle)");
    // GPU objects: each attached cycle creates and releases a swap chain, layers and textures, so a
    // leaked per-editor object keeps growing GPU memory by its size (a 1280x720 back buffer is
    // 3.5 MB) on every cycle, about 700 MB over 200 cycles. The driver allocates in steps (a 7.5 MB
    // step was seen in one run of four, either way), so the bound is 16 MB.
    std::printf("  GPU memory (DXGI, this process): %.2f -> %.2f MB over 200 attached cycles\n", g0 / kMB, g2 / kMB);
    if (g0 == 0 && g2 == 0) std::printf("  (DXGI 1.4 video memory info unavailable: GPU check skipped)\n");
    else check(g2 < g0 + 16 * 1024 * 1024, "200 attached open/close cycles after warm-up: GPU memory growth under 16 MB");
    holderGui->destroy(holder);
    holder->destroy(holder);
    DestroyWindow(holderTop);
    pump(100);
    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    DestroyWindow(top);
    entry->deinit();
    std::printf("%s (%d failure%s)\n", gFailures ? "FAILED" : "ALL PASSED", gFailures, gFailures == 1 ? "" : "s");
    return gFailures ? 1 : 0;
}
