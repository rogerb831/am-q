#include "amq/ui/editor.h"

#include "amq/params/mappings.hpp"
#include "amq/params/shape_info.hpp"
#include "amq/ui/graph_model.hpp"
#include "amq/ui/snapshot_poller.hpp"

#include <clap/ext/gui.h>
#include <visage/app.h>

#include "embedded/fonts.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

namespace amq::ui {

namespace {

std::atomic<int> gLiveEditors{0};

// Colours from the cleared design tokens (CLEAN_ROOM.md, design-system provenance). ARGB.
namespace tok {
constexpr uint32_t surface100 = 0xff17161a;
constexpr uint32_t surface200 = 0xff201f24;
constexpr uint32_t surface300 = 0xff2b2a30;
constexpr uint32_t lineGrid = 0xff26252b;
constexpr uint32_t lineHairline = 0xff3d3c43;
constexpr uint32_t lineZero = 0xff8f8d96;
constexpr uint32_t ink = 0xffeceaf0;
constexpr uint32_t inkMuted = 0xffa5a3ad;
constexpr uint32_t inkAxis = 0xff8c8a93;
constexpr uint32_t accent = 0xffffb81c;
constexpr uint32_t node = 0xffc4ee3c;
constexpr uint32_t nodeDisabled = 0xff5d5c63;
}  // namespace tok

visage::Font font(float size) { return visage::Font(size, visage::fonts::Lato_Regular_ttf); }

std::string bandLabel(int b) { return "Band " + std::to_string(b + 1); }

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

// Static ground: background, grid, 0 dB line and axis labels. Cached; redrawn only when the
// display range changes.
class GroundFrame : public visage::Frame {
public:
    explicit GroundFrame(const GraphGeometry& g) : g_(g), font_(font(12.0f)) { setCached(true); setIgnoresMouseEvents(true, false); }
    void draw(visage::Canvas& c) override {
        c.setColor(tok::surface100);
        c.fill(0, 0, width(), height());
        c.setColor(tok::lineGrid);
        const double lines[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
        const char* names[] = {"20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k"};
        const float top = GraphGeometry::kTop, bottom = height() - GraphGeometry::kBottom;
        for (double f : lines) c.fill(std::round(g_.xOfHz(f)), top, 1.0f, bottom - top);
        const double r = g_.rangeDb();
        const double step = r <= 3.0 ? 1.0 : r <= 6.0 ? 2.0 : r <= 12.0 ? 3.0 : 6.0;
        for (double db = -r; db <= r + 1e-9; db += step) c.fill(GraphGeometry::kLeft, std::round(g_.yOfDb(db)), width() - GraphGeometry::kLeft - GraphGeometry::kRight, 1.0f);
        c.setColor(tok::lineZero);
        c.fill(GraphGeometry::kLeft, std::round(g_.yOfDb(0.0)), width() - GraphGeometry::kLeft - GraphGeometry::kRight, 1.0f);
        c.setColor(tok::inkAxis);
        for (int i = 0; i < 10; ++i)
            c.text(visage::String(names[i]), font_, visage::Font::kCenter, g_.xOfHz(lines[i]) - 20.0f, bottom + 8.0f,
                   40.0f, 16.0f);
        c.setColor(tok::accent);
        for (double db = -r; db <= r + 1e-9; db += step * 2) {
            char b[16];
            std::snprintf(b, sizeof b, db > 0 ? "+%g" : "%g", db);
            c.text(visage::String(b), font_, visage::Font::kRight, width() - GraphGeometry::kRight + 4.0f,
                   g_.yOfDb(db) - 8.0f, 40.0f, 16.0f);
        }
    }

private:
    const GraphGeometry& g_;
    visage::Font font_;
};

// The combined response curve.
class CurveFrame : public visage::Frame {
public:
    CurveFrame(const GraphGeometry& g, ResponseCurve& curve, const params::ParameterTable& t)
        : g_(g), curve_(curve), table_(t) {
        setIgnoresMouseEvents(true, false);
    }
    void draw(visage::Canvas& c) override {
        curve_.update(table_);
        const auto& hz = curve_.frequencies();
        const auto& db = curve_.combinedDb();
        if (hz.empty()) return;
        visage::Path path;
        for (size_t i = 0; i < hz.size(); ++i) {
            const float x = g_.xOfHz(hz[i]);
            const float y = std::clamp(g_.yOfDb(db[i]), 0.0f, height());
            if (i == 0) path.moveTo(x, y);
            else path.lineTo(x, y);
        }
        c.setColor(tok::accent);
        c.stroke(path, 0.0f, 0.0f, width(), height(), 2.0f);
    }

private:
    const GraphGeometry& g_;
    ResponseCurve& curve_;
    const params::ParameterTable& table_;
};

// Band nodes; forwards pointer input to GraphInteraction.
class NodesFrame : public visage::Frame {
public:
    explicit NodesFrame(GraphInteraction& m) : m_(m), font_(font(11.0f)) {}
    void draw(visage::Canvas& c) override {
        for (int b = 0; b < params::kBandCount; ++b) {
            const auto n = m_.node(b);
            if (!n.used) continue;
            const bool sel = b == m_.selected();
            const float d = sel ? 22.0f : 18.0f;
            c.setColor(tok::surface100);
            c.circle(n.x - d * 0.5f - 2.0f, n.y - d * 0.5f - 2.0f, d + 4.0f);
            if (sel) {
                c.setColor(tok::ink);
                c.circle(n.x - d * 0.5f - 2.0f, n.y - d * 0.5f - 2.0f, d + 4.0f);
            }
            c.setColor(n.enabled ? tok::node : tok::nodeDisabled);
            c.circle(n.x - d * 0.5f, n.y - d * 0.5f, d);
            c.setColor(tok::surface100);
            c.text(visage::String(std::to_string(b + 1)), font_, visage::Font::kCenter, n.x - d * 0.5f, n.y - d * 0.5f, d, d);
        }
    }
    static Modifiers mods(const visage::MouseEvent& e) {
        return {e.isShiftDown(), e.isAltDown(), e.isMainModifier()};  // area 13 R1
    }
    static Button button(const visage::MouseEvent& e) {
        return e.isRightButton() ? Button::Right : e.isMiddleButton() ? Button::Middle : Button::Left;
    }
    void mouseDown(const visage::MouseEvent& e) override {
        const auto p = e.position;  // frame-relative (relativePosition() is not)
        m_.pointerDown(p.x, p.y, button(e), mods(e), e.repeatClickCount());
        redraw();
    }
    void mouseDrag(const visage::MouseEvent& e) override {
        const auto p = e.position;  // frame-relative (relativePosition() is not)
        m_.pointerDrag(p.x, p.y, mods(e));
    }
    void mouseUp(const visage::MouseEvent& e) override {
        const auto p = e.position;  // frame-relative (relativePosition() is not)
        m_.pointerUp(p.x, p.y, mods(e));
    }
    bool mouseWheel(const visage::MouseEvent& e) override {
        const auto p = e.position;  // frame-relative (relativePosition() is not)
        m_.wheel(p.x, p.y, e.precise_wheel_delta_y, mods(e));
        return true;
    }

private:
    GraphInteraction& m_;
    visage::Font font_;
};

// Readout of the selected band: name and Frequency / Gain / Q text from the formatters.
class ReadoutFrame : public visage::Frame {
public:
    ReadoutFrame(const GraphInteraction& m, const params::ParameterTable& t)
        : m_(m), t_(t), label_(font(13.0f)), cap_(font(11.0f)) {
        setIgnoresMouseEvents(true, false);
    }
    void draw(visage::Canvas& c) override {
        const int b = m_.selected();
        if (b < 0) return;
        c.setColor(tok::lineHairline);
        c.roundedRectangle(0.0f, 0.0f, width(), height(), 12.0f);
        c.setColor(tok::surface200);
        c.roundedRectangle(1.0f, 1.0f, width() - 2.0f, height() - 2.0f, 11.0f);
        c.setColor(tok::ink);
        c.text(visage::String(bandLabel(b)), label_, visage::Font::kLeft, 16.0f, 0.0f, 80.0f, height());
        const params::BandField fields[] = {params::BandField::Freq, params::BandField::Gain, params::BandField::Q};
        const char* caps[] = {"FREQ", "GAIN", "Q"};
        float x = 96.0f;
        for (int i = 0; i < 3; ++i) {
            const auto id = params::bandParam(b, fields[i]);
            c.setColor(tok::inkMuted);
            c.text(visage::String(caps[i]), cap_, visage::Font::kLeft, x, 6.0f, 96.0f, 14.0f);
            c.setColor(tok::surface300);
            c.roundedRectangle(x, 22.0f, 90.0f, 22.0f, 4.0f);
            c.setColor(tok::accent);
            c.text(visage::String(params::displayText(t_, id)), label_, visage::Font::kCenter, x, 22.0f,
                   90.0f, 22.0f);
            x += 100.0f;
        }
    }

private:
    const GraphInteraction& m_;
    const params::ParameterTable& t_;
    visage::Font label_, cap_;
};

// ---------------------------------------------------------------------------
// Accessible-control model (area 15 R12)
// ---------------------------------------------------------------------------

class BandNodeAccessible : public AccessibleControl {
public:
    BandNodeAccessible(EditController& c, GraphInteraction& m, int band) : c_(c), m_(m), band_(band) {}
    std::string accessibleName() const override { return bandLabel(band_); }
    AccessibleRole accessibleRole() const override { return AccessibleRole::GraphNode; }
    std::string accessibleValueText() const override {
        const auto f = params::bandParam(band_, params::BandField::Freq);
        const auto g = params::bandParam(band_, params::BandField::Gain);
        return params::formatPlain(*params::specAt(f), c_.table().value(f)) + ", " +
               params::formatPlain(*params::specAt(g), c_.table().value(g));
    }
    double accessibleValueNormalised() const override {
        const auto f = params::bandParam(band_, params::BandField::Freq);
        return params::toNormalised(*params::specAt(f), c_.table().value(f));
    }
    std::vector<AccessibleAction> accessibleActions() const override {
        return {AccessibleAction::Press, AccessibleAction::Increment, AccessibleAction::Decrement, AccessibleAction::SetValue};
    }
    // Press selects; Increment/Decrement nudge gain by 1 dB (PROVISIONAL step); SetValue sets
    // the normalised frequency. All through gestures.
    bool performAccessibleAction(AccessibleAction a, double value) override {
        if (!accessibleVisible()) return false;
        const auto g = params::bandParam(band_, params::BandField::Gain);
        const auto f = params::bandParam(band_, params::BandField::Freq);
        switch (a) {
            case AccessibleAction::Press: m_.pointerDown(m_.node(band_).x, m_.node(band_).y, Button::Left, {}, 1); m_.pointerUp(0, 0, {}); return true;
            case AccessibleAction::Increment:
            case AccessibleAction::Decrement: {
                EditGesture e(c_, {g});
                e.set(g, c_.table().value(g) + (a == AccessibleAction::Increment ? 1.0 : -1.0));
                return true;
            }
            case AccessibleAction::SetValue: {
                EditGesture e(c_, {f});
                e.set(f, params::fromNormalised(*params::specAt(f), value));
                return true;
            }
            default: return false;
        }
    }
    bool accessibleVisible() const override { return m_.node(band_).used; }

private:
    EditController& c_;
    GraphInteraction& m_;
    int band_;
};

class GraphAccessible : public AccessibleControl {
public:
    explicit GraphAccessible(std::array<std::unique_ptr<BandNodeAccessible>, params::kBandCount>& nodes) : nodes_(nodes) {}
    std::string accessibleName() const override { return "Response graph"; }
    AccessibleRole accessibleRole() const override { return AccessibleRole::Group; }
    std::vector<AccessibleAction> accessibleActions() const override { return {}; }
    bool performAccessibleAction(AccessibleAction, double) override { return false; }
    std::vector<AccessibleControl*> accessibleChildren() const override {
        std::vector<AccessibleControl*> v;
        for (auto& n : nodes_)
            if (n->accessibleVisible()) v.push_back(n.get());
        return v;
    }

private:
    std::array<std::unique_ptr<BandNodeAccessible>, params::kBandCount>& nodes_;
};

class RootAccessible : public AccessibleControl {
public:
    explicit RootAccessible(GraphAccessible& g) : g_(g) {}
    std::string accessibleName() const override { return "Am-Q"; }
    AccessibleRole accessibleRole() const override { return AccessibleRole::Group; }
    std::vector<AccessibleAction> accessibleActions() const override { return {}; }
    bool performAccessibleAction(AccessibleAction, double) override { return false; }
    std::vector<AccessibleControl*> accessibleChildren() const override { return {&g_}; }

private:
    GraphAccessible& g_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------

struct Editor::Impl : params::ParameterListener {
    EditController& c;
    visage::ApplicationWindow app;
    GraphGeometry geometry;
    ResponseCurve curve;
    GraphInteraction interaction;
    GroundFrame ground;
    CurveFrame curveFrame;
    NodesFrame nodes;
    ReadoutFrame readout;
    // Snapshot polling on Visage's event loop, which runs timers on the display-paced vblank
    // tick (area 12 R5/R10). Nothing draws the meters yet (band panel and metering: Phase 2),
    // so a poll changes no pixels and presents nothing.
    SnapshotPoller poller;
    visage::EventTimer pollTimer;
    std::array<std::unique_ptr<BandNodeAccessible>, params::kBandCount> nodeAccess;
    GraphAccessible graphAccess;
    RootAccessible rootAccess;

    explicit Impl(EditController& ctl)
        : c(ctl),
          interaction(ctl, geometry),
          ground(geometry),
          curveFrame(geometry, curve, ctl.table()),
          nodes(interaction),
          readout(interaction, ctl.table()),
          graphAccess(nodeAccess),
          rootAccess(graphAccess) {
        for (int b = 0; b < params::kBandCount; ++b) nodeAccess[static_cast<size_t>(b)] = std::make_unique<BandNodeAccessible>(ctl, interaction, b);
        interaction.onSelectionChanged = [this] {
            nodes.redraw();
            readout.redraw();
        };
        c.addListener(this);
        pollTimer.onTimerCallback() += [this] { poller.poll(c); };
    }
    ~Impl() override {
        pollTimer.stopTimer();
        c.removeListener(this);
    }

    Editor* owner = nullptr;
    double lastScale = 0.0;           // host-unit scale at the last layout
    float wantW = 0.0f, wantH = 0.0f;  // logical size to keep across DPI changes
    bool dpiResizePending = false;    // asked the host to resize after a DPI change

    double hostScale() const {
#if defined(__APPLE__)
        return 1.0;  // host units are points
#else
        const double s = app.window() ? app.window()->dpiScale() : app.dpiScale();
        return s > 0.0 ? s : 1.0;
#endif
    }

    void hostSize(uint32_t* w, uint32_t* h) const {
#if defined(__APPLE__)
        const int nw = static_cast<int>(std::lround(app.width())), nh = static_cast<int>(std::lround(app.height()));
#else
        const int nw = app.nativeWidth(), nh = app.nativeHeight();
#endif
        *w = nw > 0 ? static_cast<uint32_t>(nw) : static_cast<uint32_t>(std::lround(kDefaultWidth * hostScale()));
        *h = nh > 0 ? static_cast<uint32_t>(nh) : static_cast<uint32_t>(std::lround(kDefaultHeight * hostScale()));
    }

    // A DPI change keeps the native size, so the logical size shrinks or grows. Keep the
    // logical size instead: ask the host for the matching host-unit size.
    void checkDpiChange() {
        const double s = hostScale();
        const bool changed = lastScale > 0.0 && std::abs(s - lastScale) > 1e-3;
        lastScale = s;
        if (!changed || wantW <= 0.0f || !owner || !owner->onRequestResize) return;
        uint32_t w = static_cast<uint32_t>(std::lround(wantW * s)), h = static_cast<uint32_t>(std::lround(wantH * s));
        constrainHostSize(&w, &h, s);
        uint32_t cw = 0, ch = 0;
        hostSize(&cw, &ch);
        if (w == cw && h == ch) return;
        dpiResizePending = true;
        if (!owner->onRequestResize(w, h)) dpiResizePending = false;  // host refused: accept the new size
    }

    void layout() {
        checkDpiChange();
        const float w = app.width() > 0 ? app.width() : static_cast<float>(kDefaultWidth);
        const float h = app.height() > 0 ? app.height() : static_cast<float>(kDefaultHeight);
        if (!dpiResizePending) {
            wantW = w;
            wantH = h;
        }
        geometry.setSize(w, h);
        geometry.setRangeDb(rangeDb());
        std::vector<double> hz;
        const int n = std::max(64, static_cast<int>(w / 2.0f));
        for (int i = 0; i <= n; ++i) hz.push_back(GraphGeometry::kMinHz * std::pow(GraphGeometry::kMaxHz / GraphGeometry::kMinHz, i / double(n)));
        curve.setFrequencies(std::move(hz));
        ground.setBounds(0, 0, w, h);
        curveFrame.setBounds(0, 0, w, h);
        nodes.setBounds(0, 0, w, h);
        readout.setBounds(GraphGeometry::kLeft + 16.0f, GraphGeometry::kTop + 8.0f, 400.0f, 52.0f);
        ground.redraw();
        curveFrame.redraw();
        nodes.redraw();
        readout.redraw();
        if (owner && owner->onSizeChanged) {
            uint32_t hw = 0, hh = 0;
            hostSize(&hw, &hh);
            owner->onSizeChanged(hw, hh, hostScale());
        }
    }

    double rangeDb() const {
        static const double ranges[] = {3.0, 6.0, 12.0, 30.0};  // display.range choices
        const int i = static_cast<int>(c.table().value(params::globalParam(params::GlobalParam::DisplayRange)));
        return ranges[std::clamp(i, 0, 3)];
    }

    // Host, preset or GUI changes: redraw only what depends on them (area 12 R7, area 13 R22).
    void parameterChanged(params::ParamIndex i, double, params::ParamSource) override {
        if (i < static_cast<params::ParamIndex>(params::kBandCount * params::kBandFieldCount)) {
            curve.markDirty(static_cast<int>(i) / params::kBandFieldCount);
            curveFrame.redraw();
            nodes.redraw();
            readout.redraw();
            return;
        }
        if (i == params::globalParam(params::GlobalParam::DisplayRange)) {
            geometry.setRangeDb(rangeDb());
            ground.redraw();
            curveFrame.redraw();
            nodes.redraw();
        } else if (i == params::globalParam(params::GlobalParam::GainScale)) {
            curveFrame.redraw();
        }
    }
};

const char* Editor::windowApi() noexcept {
#if defined(_WIN32)
    return CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
    return CLAP_WINDOW_API_COCOA;
#else
    return CLAP_WINDOW_API_X11;
#endif
}

void Editor::constrainHostSize(uint32_t* w, uint32_t* h, double scale) noexcept {
    if (!(scale > 0.0)) scale = 1.0;
    auto clampOne = [scale](uint32_t v, uint32_t lo, uint32_t hi) {
        const double logical = std::clamp(v / scale, static_cast<double>(lo), static_cast<double>(hi));
        return static_cast<uint32_t>(std::lround(logical * scale));
    };
    *w = clampOne(*w, kMinWidth, kMaxWidth);
    *h = clampOne(*h, kMinHeight, kMaxHeight);
}

Editor::Editor(EditController& controller, uint32_t logicalWidth, uint32_t logicalHeight)
    : impl_(std::make_unique<Impl>(controller)) {
    ++gLiveEditors;
    impl_->owner = this;
    auto& app = impl_->app;
    app.setMinimumDimensions(static_cast<float>(kMinWidth), static_cast<float>(kMinHeight));
    app.setWindowDimensions(static_cast<float>(std::clamp(logicalWidth, kMinWidth, kMaxWidth)),
                            static_cast<float>(std::clamp(logicalHeight, kMinHeight, kMaxHeight)));
    app.addChild(&impl_->ground);
    app.addChild(&impl_->curveFrame);
    app.addChild(&impl_->nodes);
    app.addChild(&impl_->readout);
    // Add to (not replace) Visage's own resize callback, which keeps its top-level frame in step.
    app.onResize() += [this] { impl_->layout(); };
    impl_->layout();
}

Editor::~Editor() {
    if (impl_) impl_->app.close();
    impl_.reset();
    --gLiveEditors;
}

void Editor::attach(void* nativeParent) {
    impl_->app.show(nativeParent);
    impl_->layout();  // the window's DPI is known only now
    impl_->pollTimer.startTimer(static_cast<int>(SnapshotPoller::kPollMs));
}

bool Editor::hostSize(uint32_t* w, uint32_t* h) const {
    impl_->hostSize(w, h);
    return true;
}

void Editor::setHostSize(uint32_t w, uint32_t h) {
    impl_->dpiResizePending = false;
#if defined(__APPLE__)
    impl_->app.setWindowDimensionsFromHost(static_cast<int>(w), static_cast<int>(h));
#else
    impl_->app.setNativeWindowDimensionsFromHost(static_cast<int>(w), static_cast<int>(h));
#endif
    impl_->layout();
}

double Editor::hostScale() const noexcept { return impl_->hostScale(); }
float Editor::logicalWidth() const noexcept { return impl_->app.width(); }
float Editor::logicalHeight() const noexcept { return impl_->app.height(); }

AccessibleControl& Editor::accessibleRoot() noexcept { return impl_->rootAccess; }

int Editor::liveCount() noexcept { return gLiveEditors.load(); }

}  // namespace amq::ui
