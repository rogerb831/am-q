// GUI test host for the Am-Q spike (evidence for pass criterion 4 in our own host).
// Embeds the CLAP editor in a Win32 window and measures, through the editor's own counters:
//  A. idle: no redraws after the first frame;
//  B. hover over the handle: only the handle frame redraws (dirty-region redraw);
//  C. a 3 s handle drag driven by synthetic mouse messages at ~1 kHz: curve redraw rate
//     versus the display refresh rate, and input-to-draw latency.
// Usage: clap_gui_test <path to AmQSpike.clap>
#include <clap/clap.h>

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const clap_plugin* gPlugin = nullptr;
const clap_plugin_params* gParams = nullptr;
bool gCallback = false, gFlush = false;

struct NullEvents {
    clap_input_events in{};
    clap_output_events out{};
    NullEvents() {
        in.size = [](const clap_input_events*) -> uint32_t { return 0; };
        in.get = [](const clap_input_events*, uint32_t) -> const clap_event_header* { return nullptr; };
        out.try_push = [](const clap_output_events*, const clap_event_header*) { return true; };
    }
} gNull;

clap_host_gui gHostGui{
    [](const clap_host*) {},
    [](const clap_host*, uint32_t, uint32_t) { return true; },
    [](const clap_host*) { return true; },
    [](const clap_host*) { return true; },
    [](const clap_host*, bool) {},
};
clap_host_params gHostParams{
    [](const clap_host*, clap_param_rescan_flags) {},
    [](const clap_host*, clap_id, clap_param_clear_flags) {},
    [](const clap_host*) { gFlush = true; },
};

clap_host gHost{CLAP_VERSION, nullptr, "amq-spike-gui-test", "Am-Q", "", "0.0.1",
                [](const clap_host*, const char* id) -> const void* {
                    if (!std::strcmp(id, CLAP_EXT_GUI)) return &gHostGui;
                    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &gHostParams;
                    return nullptr;
                },
                [](const clap_host*) {}, [](const clap_host*) {}, [](const clap_host*) { gCallback = true; }};

void service() {
    if (gCallback) {
        gCallback = false;
        gPlugin->on_main_thread(gPlugin);
    }
    if (gFlush && gParams) {  // the plugin is not processing: the host flushes params on the main thread
        gFlush = false;
        gParams->flush(gPlugin, &gNull.in, &gNull.out);
    }
}

void pump(int ms) {
    const DWORD end = GetTickCount() + ms;
    while (static_cast<int>(end - GetTickCount()) > 0) {
        MSG m;
        while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
        service();
        Sleep(1);
    }
}

HWND gChild = nullptr;
BOOL CALLBACK findChild(HWND h, LPARAM) {
    gChild = h;
    return FALSE;
}

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

std::string stats(HMODULE mod) {
    using Fn = const char* (*)();
    auto fn = reinterpret_cast<Fn>(GetProcAddress(mod, "amq_spike_editor_stats"));
    return fn ? fn() : "{}";
}
void resetStats(HMODULE mod) {
    using Fn = void (*)();
    if (auto fn = reinterpret_cast<Fn>(GetProcAddress(mod, "amq_spike_editor_reset_stats"))) fn();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HMODULE mod = LoadLibraryA(argv[1]);
    auto* entry = mod ? reinterpret_cast<const clap_plugin_entry*>(GetProcAddress(mod, "clap_entry")) : nullptr;
    if (!entry || !entry->init(argv[1])) { std::printf("FAIL: load\n"); return 1; }
    auto* f = static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    gPlugin = f->create_plugin(f, &gHost, f->get_plugin_descriptor(f, 0)->id);
    gPlugin->init(gPlugin);
    gParams = static_cast<const clap_plugin_params*>(gPlugin->get_extension(gPlugin, CLAP_EXT_PARAMS));
    auto* gui = static_cast<const clap_plugin_gui*>(gPlugin->get_extension(gPlugin, CLAP_EXT_GUI));

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AmQSpikeGuiTest";
    RegisterClassW(&wc);
    HWND top = CreateWindowW(wc.lpszClassName, L"Am-Q spike GUI test", WS_OVERLAPPEDWINDOW, 100, 100, 1220, 740,
                             nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(top, SW_SHOW);

    if (!gui->create(gPlugin, CLAP_WINDOW_API_WIN32, false)) { std::printf("FAIL: gui create\n"); return 1; }
    uint32_t w = 0, h = 0;
    gui->get_size(gPlugin, &w, &h);
    clap_window win{CLAP_WINDOW_API_WIN32, {top}};
    gui->set_parent(gPlugin, &win);
    gui->show(gPlugin);
    RECT rc{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    SetWindowPos(top, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
    pump(1500);  // first frames
    EnumChildWindows(top, findChild, 0);
    const double scale = GetDpiForWindow(top) / 96.0;
    std::printf("editor size %u x %u (native px), DPI scale %.2f, child HWND %p\n", w, h, scale, (void*)gChild);
    std::printf("after open: %s\n", stats(mod).c_str());

    auto post = [&](UINT msg, WPARAM wp, double x, double y) {
        const int px = static_cast<int>(std::lround(x * scale)), py = static_cast<int>(std::lround(y * scale));
        PostMessageW(gChild, msg, wp, MAKELPARAM(px, py));
    };
    const double hx = 102.0 + (1120.0 - 102.0) / 3.0 * std::log10(1000.0 / 20.0), hy = 336.0;  // 1 kHz, 0 dB

    // A. idle
    resetStats(mod);
    pump(1000);
    std::printf("A idle 1 s: %s\n", stats(mod).c_str());

    // B. hover the handle (enter), then leave
    resetStats(mod);
    post(WM_MOUSEMOVE, 0, hx, hy);
    pump(300);
    post(WM_MOUSEMOVE, 0, hx + 200, hy + 150);
    pump(300);
    std::printf("B hover in/out: %s\n", stats(mod).c_str());

    // C. drag for 3 s at ~1 kHz input rate
    resetStats(mod);
    post(WM_MOUSEMOVE, 0, hx, hy);
    pump(50);
    post(WM_LBUTTONDOWN, MK_LBUTTON, hx, hy);
    const DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 3000) {
        const double t = (GetTickCount() - t0) / 1000.0;
        const double x = hx + 250.0 * std::sin(2.0 * 3.14159265 * 0.5 * t);
        const double y = hy - 120.0 * std::sin(2.0 * 3.14159265 * 0.7 * t);
        post(WM_MOUSEMOVE, MK_LBUTTON, x, y);
        pump(1);
    }
    post(WM_LBUTTONUP, 0, hx, hy);
    pump(300);
    std::printf("C drag 3 s: %s\n", stats(mod).c_str());

    double v = 0;
    gParams->get_value(gPlugin, 0, &v);
    std::printf("Freq after drag: %.1f Hz (plugin parameter, written through the GUI setter)\n", std::exp2(v));

    gui->destroy(gPlugin);
    DestroyWindow(top);
    gPlugin->destroy(gPlugin);
    entry->deinit();
    FreeLibrary(mod);
    return 0;
}
