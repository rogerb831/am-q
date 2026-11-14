#include "spike_editor.h"

#include "spike_plugin.h"

#include <visage/app.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace amq::spike {

// ---------------------------------------------------------------------------
// Design tokens (docs/design/spike-reference/tokens.json). ARGB.
// ---------------------------------------------------------------------------
namespace tok {
constexpr uint32_t surface100 = 0xff17161a;  // graph and window background
constexpr uint32_t surface200 = 0xff201f24;  // floating band panel
constexpr uint32_t surface300 = 0xff2b2a30;  // readout chips
constexpr uint32_t lineGrid = 0xff26252b;
constexpr uint32_t lineHairline = 0xff3d3c43;
constexpr uint32_t lineZero = 0xff8f8d96;
constexpr uint32_t ink = 0xffeceaf0;
constexpr uint32_t inkMuted = 0xffa5a3ad;
constexpr uint32_t inkAxis = 0xff8c8a93;
constexpr uint32_t accent = 0xffffb81c;
constexpr uint32_t accentHover = 0xffffc94d;
constexpr uint32_t band1 = 0xffc4ee3c;
constexpr float sizeHandle = 18.0f;         // 22 while selected
constexpr float radiusLg = 16.0f;
constexpr float radiusSm = 4.0f;
constexpr float space2 = 8.0f, space4 = 16.0f, space8 = 32.0f;
}  // namespace tok

// Graph geometry from editor-main.dc.html: 20 Hz at x = 102, 20 kHz at x = 1120
// (339.33 px per decade); 0 dB at y = 336; 17.333 px per dB (+/-18 dB in 24..648).
namespace geo {
constexpr float x20 = 102.0f, pxPerDecade = (1120.0f - 102.0f) / 3.0f;
constexpr float y0dB = 336.0f, pxPerDb = (336.0f - 24.0f) / 18.0f;
inline float xOfHz(double hz) { return x20 + pxPerDecade * static_cast<float>(std::log10(hz / 20.0)); }
inline double hzOfX(float x) { return 20.0 * std::pow(10.0, (x - x20) / pxPerDecade); }
inline float yOfDb(double db) { return y0dB - pxPerDb * static_cast<float>(db); }
inline double dbOfY(float y) { return (y0dB - y) / pxPerDb; }
}  // namespace geo

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------
namespace {
using Clock = std::chrono::steady_clock;
std::mutex gStatsMutex;
EditorStats gStats;
std::vector<double> gCurveTimes;                 // ms since epoch of each curve draw
std::vector<double> gPendingInput;               // input events awaiting a handle draw
std::vector<double> gInputToDraw;
float gAppW = -1, gAppH = -1, gGraphW = -1, gGraphH = -1;
int gResizes = 0;
double nowMs() {
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

visage::Font makeFont(float size, bool semibold) {
#ifdef _WIN32
    // System Segoe UI at runtime (the tokens' fallback for Inter); nothing is redistributed.
    const char* path = semibold ? "C:/Windows/Fonts/seguisb.ttf" : "C:/Windows/Fonts/segoeui.ttf";
#else
    const char* path = "";
#endif
    return visage::Font(size, std::string(path));
}
}  // namespace

EditorStats& SpikeEditor::stats() { return gStats; }

void SpikeEditor::resetStats() {
    std::lock_guard<std::mutex> l(gStatsMutex);
    gStats = {};
    gCurveTimes.clear();
    gPendingInput.clear();
    gInputToDraw.clear();
}

std::string SpikeEditor::statsJson() {
    std::lock_guard<std::mutex> l(gStatsMutex);
    EditorStats s = gStats;
    std::vector<double> iv;
    for (size_t i = 1; i < gCurveTimes.size(); ++i) iv.push_back(gCurveTimes[i] - gCurveTimes[i - 1]);
    if (!iv.empty()) {
        double sum = 0;
        for (double v : iv) sum += v;
        s.curveIntervalMeanMs = sum / static_cast<double>(iv.size());
        auto sorted = iv;
        std::sort(sorted.begin(), sorted.end());
        s.curveIntervalP95Ms = sorted[sorted.size() * 95 / 100];
    }
    if (!gInputToDraw.empty()) {
        double sum = 0, mx = 0;
        for (double v : gInputToDraw) { sum += v; mx = std::max(mx, v); }
        s.inputToDrawMeanMs = sum / static_cast<double>(gInputToDraw.size());
        s.inputToDrawMaxMs = mx;
    }
#ifdef _WIN32
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) s.refreshHz = dm.dmDisplayFrequency;
#endif
    char buf[768];
    std::snprintf(buf, sizeof buf,
                  "{\"graphDraws\":%llu,\"curveDraws\":%llu,\"handleDraws\":%llu,\"panelDraws\":%llu,"
                  "\"mouseEvents\":%llu,\"curveIntervalMeanMs\":%.3f,\"curveIntervalP95Ms\":%.3f,"
                  "\"inputToDrawMeanMs\":%.3f,\"inputToDrawMaxMs\":%.3f,\"refreshHz\":%.1f,"
                  "\"appW\":%.0f,\"appH\":%.0f,\"graphW\":%.0f,\"graphH\":%.0f,\"resizes\":%d}",
                  (unsigned long long)s.graphDraws, (unsigned long long)s.curveDraws,
                  (unsigned long long)s.handleDraws, (unsigned long long)s.panelDraws,
                  (unsigned long long)s.mouseEvents, s.curveIntervalMeanMs, s.curveIntervalP95Ms,
                  s.inputToDrawMeanMs, s.inputToDrawMaxMs, s.refreshHz, gAppW, gAppH, gGraphW, gGraphH, gResizes);
    return buf;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

// Static ground: background, grid, 0 dB line, axis labels. Cached, drawn once.
class GraphFrame : public visage::Frame {
public:
    GraphFrame() : font_(makeFont(12.0f, false)) { setCached(true); }
    std::function<void(double)> onWheelQ;

    void draw(visage::Canvas& c) override {
        { std::lock_guard<std::mutex> l(gStatsMutex); ++gStats.graphDraws; }
        c.setColor(tok::surface100);
        c.fill(0, 0, width(), height());
        c.setColor(tok::lineGrid);
        const double decades[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
        for (double f : decades) c.fill(std::round(geo::xOfHz(f)), 24.0f, 1.0f, 624.0f);
        for (int db = 18; db >= -18; db -= 3) c.fill(0.0f, std::round(geo::yOfDb(db)), width(), 1.0f);
        c.setColor(tok::lineZero);
        c.fill(0.0f, geo::y0dB, width(), 1.0f);
        // Axis labels (ink-axis) and the EQ gain scale (accent).
        const char* names[] = {"20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k"};
        c.setColor(tok::inkAxis);
        for (int i = 0; i < 10; ++i)
            c.text(visage::String(names[i]), font_, visage::Font::kCenter, geo::xOfHz(decades[i]) - 20.0f, 652.0f,
                   40.0f, 16.0f);
        c.setColor(tok::accent);
        for (int db = 18; db >= -18; db -= 6) {
            char b[8];
            std::snprintf(b, sizeof b, db > 0 ? "+%d" : "%d", db);
            c.text(visage::String(b), font_, visage::Font::kRight, width() - 44.0f, geo::yOfDb(db) - 8.0f, 36.0f,
                   16.0f);
        }
    }
    bool mouseWheel(const visage::MouseEvent& e) override {
        if (onWheelQ) onWheelQ(e.precise_wheel_delta_y);
        return true;
    }

private:
    visage::Font font_;
};

// The amber summed response (one band here), from the same design the audio uses.
class CurveFrame : public visage::Frame {
public:
    explicit CurveFrame(SpikePlugin& p) : plugin_(p) { setIgnoresMouseEvents(true, false); }
    void draw(visage::Canvas& c) override {
        {
            std::lock_guard<std::mutex> l(gStatsMutex);
            ++gStats.curveDraws;
            gCurveTimes.push_back(nowMs());
        }
        const dsp::BandSpec spec{dsp::Shape::Bell, std::exp2(plugin_.uiValue(kFreq)), plugin_.uiValue(kGain),
                                 qFromNormalised(plugin_.uiValue(kQ)), dsp::Slope::Db12};
        const auto proto = dsp::designAnalogPrototype(spec);
        const auto d = dsp::designMatched(proto, 48000.0, dsp::DesignMethod::MatchedThreePoint);
        visage::Path path;
        for (int x = 0; x <= static_cast<int>(width()); x += 2) {
            const double hz = std::min(geo::hzOfX(static_cast<float>(x)), 0.499 * 48000.0);
            const double db = dsp::linearToDb(std::abs(d.responseHz(hz)));
            const float y = std::clamp(geo::yOfDb(db), 0.0f, height());
            if (x == 0) path.moveTo(0.0f, y);
            else path.lineTo(static_cast<float>(x), y);
        }
        c.setColor(tok::accent);
        c.stroke(path, 0.0f, 0.0f, width(), height(), 2.0f);
    }

private:
    SpikePlugin& plugin_;
};

// The draggable band handle (band-1 colour, ground-coloured ring; 22 px with ink ring while held).
class HandleFrame : public visage::Frame {
public:
    static constexpr float kBox = 34.0f;
    std::function<void()> onBegin, onEnd;
    std::function<void(float, float)> onDrag;  // window coordinates of the handle centre
    bool held = false, hover = false;

    void draw(visage::Canvas& c) override {
        {
            std::lock_guard<std::mutex> l(gStatsMutex);
            ++gStats.handleDraws;
            const double t = nowMs();
            for (double t0 : gPendingInput) gInputToDraw.push_back(t - t0);
            gPendingInput.clear();
        }
        const float d = held ? 22.0f : tok::sizeHandle;
        const float cx = kBox * 0.5f, cy = kBox * 0.5f;
        if (held) {
            c.setColor(tok::surface100);
            c.circle(cx - d * 0.5f - 5.0f, cy - d * 0.5f - 5.0f, d + 10.0f);
            c.setColor(tok::ink);
            c.circle(cx - d * 0.5f - 3.0f, cy - d * 0.5f - 3.0f, d + 6.0f);
        } else {
            c.setColor(tok::surface100);
            c.circle(cx - d * 0.5f - 2.0f, cy - d * 0.5f - 2.0f, d + 4.0f);
        }
        c.setColor(hover && !held ? 0xffd4f86c : tok::band1);
        c.circle(cx - d * 0.5f, cy - d * 0.5f, d);
    }
    void mouseEnter(const visage::MouseEvent&) override { hover = true; redraw(); }
    void mouseExit(const visage::MouseEvent&) override { hover = false; redraw(); }
    void mouseDown(const visage::MouseEvent& e) override {
        held = true;
        grabOffset_ = e.relativePosition();
        grabOffset_.x -= kBox * 0.5f;
        grabOffset_.y -= kBox * 0.5f;
        if (onBegin) onBegin();
        redraw();
    }
    void mouseDrag(const visage::MouseEvent& e) override {
        {
            std::lock_guard<std::mutex> l(gStatsMutex);
            ++gStats.mouseEvents;
            gPendingInput.push_back(nowMs());
        }
        const auto w = e.windowPosition();
        if (onDrag) onDrag(w.x - grabOffset_.x, w.y - grabOffset_.y);
    }
    void mouseUp(const visage::MouseEvent&) override {
        held = false;
        if (onEnd) onEnd();
        redraw();
    }

private:
    visage::Point grabOffset_{0, 0};
};

// Small floating panel: band dot, "Band #1", and the Freq / Gain / Q readouts.
class PanelFrame : public visage::Frame {
public:
    explicit PanelFrame(SpikePlugin& p) : plugin_(p), label_(makeFont(13.0f, true)), cap_(makeFont(12.0f, false)) {}
    void draw(visage::Canvas& c) override {
        { std::lock_guard<std::mutex> l(gStatsMutex); ++gStats.panelDraws; }
        c.setColor(tok::lineHairline);
        c.roundedRectangle(0.0f, 0.0f, width(), height(), tok::radiusLg);
        c.setColor(tok::surface200);
        c.roundedRectangle(1.0f, 1.0f, width() - 2.0f, height() - 2.0f, tok::radiusLg - 1.0f);
        const float pad = tok::space4;
        c.setColor(tok::band1);
        c.circle(pad, height() * 0.5f - 5.0f, 10.0f);
        c.setColor(tok::ink);
        c.text(visage::String("Band #1"), label_, visage::Font::kLeft, pad + 18.0f, 0.0f, 80.0f, height());
        const clap_id ids[] = {kFreq, kGain, kQ};
        const char* caps[] = {"FREQ", "GAIN", "Q"};
        float x = pad + 104.0f;
        for (int i = 0; i < 3; ++i) {
            char v[32];
            formatParam(ids[i], plugin_.uiValue(ids[i]), v, sizeof v);
            const float w = 104.0f;
            c.setColor(tok::inkMuted);
            c.text(visage::String(caps[i]), cap_, visage::Font::kLeft, x, 8.0f, w, 14.0f);
            c.setColor(tok::surface300);
            c.roundedRectangle(x, 26.0f, w - tok::space2, 22.0f, tok::radiusSm);
            c.setColor(tok::accent);
            c.text(visage::String(v), label_, visage::Font::kCenter, x, 26.0f, w - tok::space2, 22.0f);
            x += w;
        }
    }

private:
    SpikePlugin& plugin_;
    visage::Font label_, cap_;
};

// ---------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------

SpikeEditor::SpikeEditor(SpikePlugin& plugin) : plugin_(plugin) {
    app_ = std::make_unique<visage::ApplicationWindow>();
    app_->setWindowDimensions(static_cast<float>(kWidth), static_cast<float>(kHeight));

    graph_ = std::make_unique<GraphFrame>();
    curve_ = std::make_unique<CurveFrame>(plugin_);
    handle_ = std::make_unique<HandleFrame>();
    panel_ = std::make_unique<PanelFrame>(plugin_);
    app_->addChild(graph_.get());
    app_->addChild(curve_.get());
    app_->addChild(panel_.get());
    app_->addChild(handle_.get());

    auto layout = [this] {
        ++gResizes;
        gAppW = app_->width();
        gAppH = app_->height();
        graph_->setBounds(0, 0, kWidth, kHeight);
        curve_->setBounds(0, 0, kWidth, kHeight);
        const float pw = 104.0f + 3 * 104.0f + tok::space4, ph = 56.0f;
        panel_->setBounds((kWidth - pw) * 0.5f, kHeight - tok::space8 - ph, pw, ph);
        layoutHandle();
        gGraphW = graph_->width();
        gGraphH = graph_->height();
    };
    app_->onResize() = layout;
    layout();

    handle_->onBegin = [this] {
        plugin_.guiBeginEdit(kFreq);
        plugin_.guiBeginEdit(kGain);
    };
    handle_->onEnd = [this] {
        plugin_.guiEndEdit(kFreq);
        plugin_.guiEndEdit(kGain);
    };
    handle_->onDrag = [this](float x, float y) {
        const double hz = std::clamp(geo::hzOfX(x), dsp::kMinBandFrequencyHz, dsp::kMaxBandFrequencyHz);
        const double db = std::clamp(geo::dbOfY(y), -30.0, 30.0);
        plugin_.guiSetValue(kFreq, std::log2(hz));
        plugin_.guiSetValue(kGain, db);
        refreshFromPlugin();
    };
    graph_->onWheelQ = [this](double delta) {
        plugin_.guiBeginEdit(kQ);
        plugin_.guiSetValue(kQ, plugin_.uiValue(kQ) + 0.02 * delta);
        plugin_.guiEndEdit(kQ);
        refreshFromPlugin();
    };
}

SpikeEditor::~SpikeEditor() {
    if (app_) app_->close();
    if (const char* path = std::getenv("AMQ_SPIKE_STATS")) {
        if (FILE* f = std::fopen(path, "w")) {
            std::fputs(statsJson().c_str(), f);
            std::fclose(f);
        }
    }
}

void SpikeEditor::attach(void* parentHwnd) { app_->show(parentHwnd); }

void SpikeEditor::nativeSize(uint32_t* w, uint32_t* h) const {
    *w = static_cast<uint32_t>(app_->nativeWidth() > 0 ? app_->nativeWidth() : kWidth);
    *h = static_cast<uint32_t>(app_->nativeHeight() > 0 ? app_->nativeHeight() : kHeight);
}

void SpikeEditor::layoutHandle() {
    const float x = geo::xOfHz(std::exp2(plugin_.uiValue(kFreq)));
    const float y = geo::yOfDb(plugin_.uiValue(kGain));
    handle_->setBounds(x - HandleFrame::kBox * 0.5f, y - HandleFrame::kBox * 0.5f, HandleFrame::kBox,
                       HandleFrame::kBox);
}

void SpikeEditor::refreshFromPlugin() {
    layoutHandle();
    handle_->redraw();
    curve_->redraw();
    panel_->redraw();
}

}  // namespace amq::spike

// Exported for the spike's own test host (evidence collection only).
extern "C" __declspec(dllexport) const char* amq_spike_editor_stats() {
    static std::string s;
    s = amq::spike::SpikeEditor::statsJson();
    return s.c_str();
}
extern "C" __declspec(dllexport) void amq_spike_editor_reset_stats() { amq::spike::SpikeEditor::resetStats(); }
