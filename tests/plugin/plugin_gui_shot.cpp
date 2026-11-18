// Editor screenshots for design review (Windows, desktop session): opens the editor in a real
// window, sets up bands like the design canvas's Main board, drives a few states with mouse
// messages and saves each as a PNG. Not a pass/fail test.
//
// Usage: amq_plugin_gui_shot <Am-Q.clap> <output dir>
#include <clap/clap.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

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

bool savePng(HWND w, const std::wstring& path) {
    RECT rc{};
    GetWindowRect(w, &rc);  // screen pixels (the process is per-monitor DPI aware)
    const int W = rc.right - rc.left, H = rc.bottom - rc.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, W, H, screen, rc.left, rc.top, SRCCOPY);  // the GPU swap chain as shown on screen
    SelectObject(mem, old);
    bool ok = false;
    IWICImagingFactory* f = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
        IWICStream* s = nullptr;
        IWICBitmapEncoder* enc = nullptr;
        IWICBitmapFrameEncode* fr = nullptr;
        if (SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(s, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
            SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(static_cast<UINT>(W), static_cast<UINT>(H)))) {
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGR;
            fr->SetPixelFormat(&fmt);
            ok = SUCCEEDED(fr->WritePixels(static_cast<UINT>(H), static_cast<UINT>(W * 4), static_cast<UINT>(W * H * 4),
                                           static_cast<BYTE*>(bits))) &&
                 SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
        }
        if (fr) fr->Release();
        if (enc) enc->Release();
        if (s) s->Release();
        f->Release();
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

struct ParamEvents {
    std::vector<clap_event_param_value> evs;
    clap_input_events in{this, [](const clap_input_events* l) { return static_cast<uint32_t>(static_cast<const ParamEvents*>(l->ctx)->evs.size()); },
                         [](const clap_input_events* l, uint32_t i) {
                             return &static_cast<const ParamEvents*>(l->ctx)->evs[i].header;
                         }};
    void add(clap_id id, double v) {
        clap_event_param_value e{};
        e.header.size = sizeof(e);
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.param_id = id;
        e.note_id = e.port_index = e.channel = e.key = -1;
        e.value = v;
        evs.push_back(e);
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::string out = argv[2];
    const std::string trigger = out + "/shot-request.txt";
    _putenv_s("AMQ_GUI_SHOT", trigger.c_str());  // before the plugin (and its C runtime) loads
    HMODULE mod = LoadLibraryA(argv[1]);
    auto* entry = mod ? reinterpret_cast<const clap_plugin_entry*>(GetProcAddress(mod, "clap_entry")) : nullptr;
    if (!entry || !entry->init(argv[1])) return 1;
    gHost.clap_version = CLAP_VERSION;
    gHost.name = "amq-gui-shot";
    gHost.vendor = gHost.url = "";
    gHost.version = "0";
    gHost.get_extension = [](const clap_host*, const char* id) -> const void* {
        return std::strcmp(id, CLAP_EXT_PARAMS) == 0 ? &gParamsExt : nullptr;
    };
    gHost.request_restart = gHost.request_process = gHost.request_callback = [](const clap_host*) {};
    gParamsExt.rescan = [](const clap_host*, clap_param_rescan_flags) {};
    gParamsExt.clear = [](const clap_host*, clap_id, clap_param_clear_flags) {};
    gParamsExt.request_flush = [](const clap_host*) {};
    auto* f = static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    const clap_plugin* p = f->create_plugin(f, &gHost, f->get_plugin_descriptor(f, 0)->id);
    p->init(p);
    p->activate(p, 48000.0, 1, 512);
    p->start_processing(p);
    auto* params = static_cast<const clap_plugin_params*>(p->get_extension(p, CLAP_EXT_PARAMS));
    auto* gui = static_cast<const clap_plugin_gui*>(p->get_extension(p, CLAP_EXT_GUI));
    std::map<std::string, clap_id> ids;
    for (uint32_t i = 0; i < params->count(p); ++i) {
        clap_param_info info{};
        params->get_info(p, i, &info);
        ids[info.name] = info.id;
    }
    auto id = [&](int band, const char* field) { return ids["Band " + std::to_string(band) + " " + field]; };
    std::mt19937 rng(3);
    std::normal_distribution<float> g(0.0f, 1.0f);
    std::vector<float> l(512), r(512), ol(512), orr(512);
    float b[3] = {};
    auto process = [&](ParamEvents* ev, int blocks) {
        for (int k = 0; k < blocks; ++k) {
            for (size_t i = 0; i < 512; ++i) {  // brown-ish noise, so the analyzer has a slope
                b[0] = 0.98f * b[0] + 0.1f * g(rng);
                l[i] = r[i] = 0.25f * b[0] + 0.02f * g(rng);
            }
            float* ins[2] = {l.data(), r.data()};
            float* outs[2] = {ol.data(), orr.data()};
            clap_audio_buffer ib{}, ob{};
            ib.data32 = ins, ib.channel_count = 2, ob.data32 = outs, ob.channel_count = 2;
            ParamEvents none;
            clap_output_events oe{nullptr, [](const clap_output_events*, const clap_event_header*) { return true; }};
            clap_process pr{};
            pr.steady_time = -1, pr.frames_count = 512;
            pr.audio_inputs = &ib, pr.audio_outputs = &ob, pr.audio_inputs_count = pr.audio_outputs_count = 1;
            pr.in_events = ev && k == 0 ? &ev->in : &none.in;
            pr.out_events = &oe;
            p->process(p, &pr);
        }
    };
    // The Main board's five bands.
    ParamEvents ev;
    struct B {
        int shape;
        double hz, gain, q;
        int placement;
        bool dyn;
        double range;
    };
    const B bands[] = {{2, 32, 0, 0.71, 0, false, 0}, {0, 180, -3.5, 1.4, 3, true, -3},  {0, 1820, 4.5, 0.71, 0, true, 3},
                       {0, 3400, -2.5, 4.0, 4, true, -4}, {3, 9000, 3.0, 0.71, 0, false, 0}};
    for (int i = 0; i < 5; ++i) {
        const auto& bb = bands[i];
        ev.add(id(i + 1, "Used"), 1);
        ev.add(id(i + 1, "Shape"), bb.shape);
        ev.add(id(i + 1, "Frequency"), std::log2(bb.hz));
        ev.add(id(i + 1, "Gain"), bb.gain);
        ev.add(id(i + 1, "Q"), std::log(bb.q / 0.025) / std::log(1600.0));
        ev.add(id(i + 1, "Placement"), bb.placement);
        ev.add(id(i + 1, "Dynamic"), bb.dyn ? 1 : 0);
        ev.add(id(i + 1, "Dynamic Range"), bb.range);
        if (i == 0) ev.add(id(i + 1, "Slope"), 3);
    }
    ev.add(ids["Processing Mode"], 1);
    ev.add(ids["Analyzer External"], 0);
    process(&ev, 1);
    process(nullptr, 300);
    p->on_main_thread(p);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AmQGuiShot";
    RegisterClassW(&wc);
    HWND top = CreateWindowW(wc.lpszClassName, L"Am-Q", WS_OVERLAPPEDWINDOW, 60, 60, 1400, 900, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(top, SW_SHOW);
    gui->create(p, CLAP_WINDOW_API_WIN32, false);
    const double scale = GetDpiForWindow(top) / 96.0;
    std::printf("dpi scale %.2f%c", scale, 10);
    SetWindowPos(top, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    double W = 1280, H = 760;
    uint32_t w = static_cast<uint32_t>(std::lround(W * scale)), h = static_cast<uint32_t>(std::lround(H * scale));
    gui->set_size(p, w, h);
    clap_window win{CLAP_WINDOW_API_WIN32, {top}};
    gui->set_parent(p, &win);
    gui->show(p);
    pump(600);
    EnumChildWindows(top, findChild, 0);
    if (!gChild) return 1;
    auto post = [&](UINT msg, WPARAM wp, double x, double y) {
        PostMessageW(gChild, msg, wp, MAKELPARAM(static_cast<int>(std::lround(x * scale)), static_cast<int>(std::lround(y * scale))));
    };
    auto click = [&](double x, double y, bool right = false) {
        post(WM_MOUSEMOVE, 0, x, y);
        pump(30);
        post(right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, right ? MK_RBUTTON : MK_LBUTTON, x, y);
        post(right ? WM_RBUTTONUP : WM_LBUTTONUP, 0, x, y);
        pump(120);
    };
    auto xOf = [&](double hz) { return (W - 100) * std::log(hz / 10.0) / std::log(3000.0); };
    auto yOf = [&](double db) { return 44 + 24 + (H - 136) / 2 - db / 12.0 * (H - 136) / 2; };
    auto shot = [&](const char* name) {
        for (int k = 0; k < 10; ++k) {
            process(nullptr, 4);
            p->on_main_thread(p);
            pump(25);
        }
        const std::string file = out + "\\" + name + ".png";
        std::FILE* t = nullptr;
        if (fopen_s(&t, trigger.c_str(), "wb") == 0 && t) {
            std::fwrite(file.data(), 1, file.size(), t);
            std::fclose(t);
        }
        for (int k = 0; k < 40 && GetFileAttributesA(trigger.c_str()) != INVALID_FILE_ATTRIBUTES; ++k) pump(25);
        std::printf("saved %s\n", name);
    };
    auto escape = [&] {
        const LPARAM sc = static_cast<LPARAM>(MapVirtualKeyW(VK_ESCAPE, MAPVK_VK_TO_VSC)) << 16;
        PostMessageW(gChild, WM_KEYDOWN, VK_ESCAPE, sc | 1);
        PostMessageW(gChild, WM_KEYUP, VK_ESCAPE, sc | 1 | (LPARAM(3) << 30));
        pump(100);
    };

    post(WM_MOUSEMOVE, 0, xOf(400), yOf(8));
    shot("01-no-selection");
    click(xOf(1820), yOf(4.5));
    post(WM_MOUSEMOVE, 0, xOf(1820), yOf(4.5) - 120);
    shot("02-main-dynamic");
    click(xOf(9000), yOf(3.0));
    shot("03-main-static");
    click(xOf(32), yOf(0));
    shot("04-low-cut");
    click(xOf(1820), yOf(4.5), true);
    pump(100);
    post(WM_MOUSEMOVE, 0, xOf(1820) + 60, yOf(4.5) + 26 + 4 * 30 + 9 + 15);
    pump(100);
    shot("05-band-menu");
    escape();
    shot("05b-after-escape");
    click(560, 22);
    // Status bar: Processing select (~x 160..300), Analyzer select, Output pan chip.
    click(200, H - 22);
    shot("06-processing");
    click(560, 22);
    click(W - 24 - 28 - 16 - 28 - 16 - 64 - 16 - 20, H - 22);
    shot("07-output");
    click(560, 22);
    click(W - 24 - 28 - 8 - 14, 22);  // Settings (gear)
    shot("08-settings");
    click(560, 22);
    click(60, H - 22);  // Bands
    shot("09-band-list");
    click(60, H - 22);
    click(W - 24 - 14, 22);  // Help: hint mode
    shot("10-hint-mode");
    click(W - 24 - 14, 22);
    click(W - 24 - 28 - 8 - 28 - 8 - 60, 22);  // EQ Match
    shot("11-eq-match");
    click(W - 24 - 50, 22);  // EQ Match (in EQ Match mode the button sits at the right edge)
    // Tooltip on the Settings button after the 600 ms delay.
    post(WM_MOUSEMOVE, 0, 600, 300);
    pump(60);
    post(WM_MOUSEMOVE, 0, W - 24 - 28 - 8 - 14, 22);
    pump(900);
    shot("11b-tooltip");
    post(WM_MOUSEMOVE, 0, 400, 300);
    pump(100);
    // Smaller sizes.
    for (auto [ww, hh, name] : {std::tuple{1180.0, 560.0, "12-compact"}, std::tuple{640.0, 360.0, "13-minimum"}}) {
        W = ww, H = hh;
        w = static_cast<uint32_t>(std::lround(W * scale)), h = static_cast<uint32_t>(std::lround(H * scale));
        SetWindowPos(top, nullptr, 0, 0, static_cast<int>(w) + 40, static_cast<int>(h) + 80, SWP_NOMOVE | SWP_NOZORDER);
        gui->set_size(p, w, h);
        pump(300);
        click(xOf(1820), yOf(4.5));
        shot(name);
    }
    gui->destroy(p);
    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    entry->deinit();
    return 0;
}
