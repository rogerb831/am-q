#include "amq/ui/editor.h"

#include "amq/dsp/log_grid.hpp"
#include "amq/params/mappings.hpp"
#include "amq/params/shape_info.hpp"
#include "amq/ui/band_menu.hpp"
#include "amq/ui/band_ops.hpp"
#include "amq/ui/band_panel.hpp"
#include "amq/ui/graph_model.hpp"
#include "amq/ui/key_dispatcher.hpp"
#include "amq/ui/snapshot_poller.hpp"

#include <clap/ext/gui.h>
#include <visage/app.h>
#include <visage/ui.h>
#include <visage/windowing.h>

#include "embedded/fonts.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace amq::ui {

namespace {

std::atomic<int> gLiveEditors{0};

// Colours from the cleared design tokens (CLEAN_ROOM.md, design-system provenance). ARGB.
// PROVISIONAL additions for Phase 2 elements until the Product designer's region map.
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
constexpr uint32_t inkDisabled = 0xff5d5c63;
constexpr uint32_t accent = 0xffffb81c;
constexpr uint32_t node = 0xffc4ee3c;
constexpr uint32_t nodeDisabled = 0xff5d5c63;
constexpr uint32_t gainReduction = 0xffff6b5b;
constexpr uint32_t spectrumPre = 0x806f8fd0;
constexpr uint32_t spectrumPost = 0xa0c4ee3c;
constexpr uint32_t spectrumExt = 0x80d07fd0;
constexpr uint32_t spectrumPeak = 0x50eceaf0;
constexpr uint32_t lasso = 0x40eceaf0;
constexpr uint32_t meter = 0xffc4ee3c;
constexpr uint32_t meterHot = 0xffff6b5b;
}  // namespace tok

constexpr float kPanelHeight = 176.0f;  // PROVISIONAL band panel height (region map pending)

visage::Font font(float size) { return visage::Font(size, visage::fonts::Lato_Regular_ttf); }

std::string bandLabel(int b) { return "Band " + std::to_string(b + 1); }

Modifiers modsOf(const visage::MouseEvent& e) { return {e.isShiftDown(), e.isAltDown(), e.isMainModifier()}; }  // area 13 R1

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

// Static ground: background, grid, 0 dB line and axis labels. Cached; redrawn when the display
// range or the zoomed span changes.
class GroundFrame : public visage::Frame {
public:
    explicit GroundFrame(const GraphGeometry& g) : g_(g), font_(font(12.0f)) { setCached(true); setIgnoresMouseEvents(true, false); }
    void draw(visage::Canvas& c) override {
        c.setColor(tok::surface100);
        c.fill(0, 0, width(), height());
        const double lines[] = {20, 30, 50, 100, 200, 300, 500, 1000, 2000, 3000, 5000, 10000, 20000};
        const char* names[] = {"20", "30", "50", "100", "200", "300", "500", "1k", "2k", "3k", "5k", "10k", "20k"};
        const float top = GraphGeometry::kTop, bottom = height() - GraphGeometry::kBottom;
        const double span = std::log2(g_.viewHi() / g_.viewLo());
        for (size_t i = 0; i < std::size(lines); ++i) {
            const double f = lines[i];
            if (f < g_.viewLo() || f > g_.viewHi()) continue;
            const bool major = f == 20 || f == 50 || f == 100 || f == 200 || f == 500 || f == 1000 || f == 2000 ||
                               f == 5000 || f == 10000 || f == 20000;
            if (!major && span > 6.0) continue;  // the minor lines only when zoomed in
            c.setColor(tok::lineGrid);
            c.fill(std::round(g_.xOfHz(f)), top, 1.0f, bottom - top);
            c.setColor(tok::inkAxis);
            c.text(visage::String(names[i]), font_, visage::Font::kCenter, g_.xOfHz(f) - 20.0f, bottom + 8.0f, 40.0f, 16.0f);
        }
        const double r = g_.rangeDb();
        const double step = r <= 3.0 ? 1.0 : r <= 6.0 ? 2.0 : r <= 12.0 ? 3.0 : 6.0;
        c.setColor(tok::lineGrid);
        for (double db = -r; db <= r + 1e-9; db += step)
            c.fill(GraphGeometry::kLeft, std::round(g_.yOfDb(db)), width() - GraphGeometry::kLeft - GraphGeometry::kRight, 1.0f);
        c.setColor(tok::lineZero);
        c.fill(GraphGeometry::kLeft, std::round(g_.yOfDb(0.0)), width() - GraphGeometry::kLeft - GraphGeometry::kRight, 1.0f);
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

// Spectrum layer (area 19 "Render pre, post and side-chain spectra"; area 12 R10): the display
// spectra on the shared log grid, with the analyzer range and tilt, for the sources switched on.
class SpectrumFrame : public visage::Frame {
public:
    SpectrumFrame(const GraphGeometry& g, const params::ParameterTable& t, const SnapshotPoller& p)
        : g_(g), t_(t), p_(p) {
        setIgnoresMouseEvents(true, false);
    }
    void draw(visage::Canvas& c) override {
        const auto* s = p_.spectrum();
        if (!s) return;
        static const double ranges[] = {60.0, 90.0, 120.0};
        const double range = ranges[std::clamp(static_cast<int>(val(params::GlobalParam::AnalyzerRange)), 0, 2)];
        const auto& grid = dsp::logGrid();
        const float top = GraphGeometry::kTop, bottom = height() - GraphGeometry::kBottom;
        auto drawOne = [&](const std::array<float, analysis::kMaxSpectrumBins>& db, uint32_t colour, float thickness = 1.0f) {
            visage::Path path;
            bool started = false;
            const uint32_t n = std::min<uint32_t>(s->binCount, static_cast<uint32_t>(grid.size()));
            for (uint32_t i = 0; i < n; ++i) {
                const double hz = grid[i];
                if (hz < g_.viewLo() || hz > g_.viewHi()) continue;
                const double v = db[i];  // tilt and ballistics come from the display follower
                const float y = std::clamp(top + static_cast<float>(-v / range) * (bottom - top), top, bottom);
                const float x = g_.xOfHz(hz);
                if (!started) path.moveTo(x, y), started = true;
                else path.lineTo(x, y);
            }
            if (!started) return;
            c.setColor(colour);
            c.stroke(path, 0.0f, 0.0f, width(), height(), thickness);
        };
        if (val(params::GlobalParam::AnalyzerShowPre) >= 0.5) drawOne(s->pre, tok::spectrumPre);
        if (val(params::GlobalParam::AnalyzerShowPost) >= 0.5) {
            drawOne(s->postPeak, tok::spectrumPeak, 0.75f);  // peak hold
            drawOne(s->post, tok::spectrumPost);
        }
        if (val(params::GlobalParam::AnalyzerShowExternal) >= 0.5) drawOne(s->external, tok::spectrumExt);
    }

private:
    double val(params::GlobalParam g) const { return t_.value(params::globalParam(g)); }
    const GraphGeometry& g_;
    const params::ParameterTable& t_;
    const SnapshotPoller& p_;
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

// Band nodes, range handles, gain-reduction rings, the lasso and the solo-level indicator; takes
// pointer input (GraphInteraction), keys (KeyDispatcher, focus first) and right-click menus.
class NodesFrame : public visage::Frame {
public:
    NodesFrame(GraphInteraction& m, BandOps& ops, KeyDispatcher& keys, const SnapshotPoller& p)
        : m_(m), ops_(ops), keys_(keys), p_(p), font_(font(11.0f)) {
        setAcceptsKeystrokes(true);
    }
    std::function<void()> onViewChanged;  // the zoomed span changed
    std::function<void()> onEdited;       // keys or menus changed something

    void draw(visage::Canvas& c) override {
        const auto& gr = p_.gainChange();
        for (int b = 0; b < params::kBandCount; ++b) {
            const auto n = m_.node(b);
            if (!n.used) continue;
            if (n.hasRangeHandle && n.rangeDb != 0.0) {  // range handle: a line to gain + range
                c.setColor(n.dynamic ? tok::accent : tok::inkDisabled);
                c.segment(n.x, n.y, n.x, n.rangeY, 1.5f, true);
                c.circle(n.x - GraphInteraction::kHandleRadiusPx * 0.5f, n.rangeY - GraphInteraction::kHandleRadiusPx * 0.5f,
                         GraphInteraction::kHandleRadiusPx);
            }
            const float d = n.focused ? 22.0f : 18.0f;
            c.setColor(tok::surface100);
            c.circle(n.x - d * 0.5f - 2.0f, n.y - d * 0.5f - 2.0f, d + 4.0f);
            if (n.selected) {
                c.setColor(tok::ink);
                c.ring(n.x - d * 0.5f - 3.0f, n.y - d * 0.5f - 3.0f, d + 6.0f, n.focused ? 2.0f : 1.0f);
            }
            c.setColor(n.enabled ? tok::node : tok::nodeDisabled);
            c.circle(n.x - d * 0.5f, n.y - d * 0.5f, d);
            // Gain reduction on the node (area 14 R12): an arc whose length follows |change| / 30 dB.
            const float g = gr.currentDb[static_cast<size_t>(b)];
            if (std::abs(g) > 0.05f) {
                c.setColor(tok::gainReduction);
                const float frac = std::min(std::abs(g) / 30.0f, 1.0f);
                c.arc(n.x - d * 0.5f - 4.0f, n.y - d * 0.5f - 4.0f, d + 8.0f, 2.5f, -1.5707963f, frac * 6.2831853f, true);
            }
            c.setColor(tok::surface100);
            c.text(visage::String(std::to_string(b + 1)), font_, visage::Font::kCenter, n.x - d * 0.5f, n.y - d * 0.5f, d, d);
        }
        if (m_.lassoActive()) {
            const auto r = m_.lassoRect();
            c.setColor(tok::lasso);
            c.fill(r[0], r[1], r[2] - r[0], r[3] - r[1]);
        }
        if (m_.soloDragActive() && m_.focused() >= 0) {  // solo-level indicator (area 13 R11)
            const auto n = m_.node(m_.focused());
            char buf[48];
            std::snprintf(buf, sizeof buf, "Solo Gain %+.1f dB", m_.soloDragDb());
            c.setColor(tok::ink);
            c.text(visage::String(buf), font_, visage::Font::kLeft, n.x + 16.0f, n.y - 28.0f, 140.0f, 16.0f);
        }
    }
    static Button button(const visage::MouseEvent& e) {
        return e.isRightButton() ? Button::Right : e.isMiddleButton() ? Button::Middle : Button::Left;
    }
    void mouseDown(const visage::MouseEvent& e) override {
        requestKeyboardFocus();
        const auto p = e.position;  // frame-relative (relativePosition() is not)
        if (button(e) == Button::Right) {
            showMenu(p.x, p.y);
            return;
        }
        m_.pointerDown(p.x, p.y, button(e), modsOf(e), e.repeatClickCount());
        redraw();
    }
    void mouseDrag(const visage::MouseEvent& e) override {
        m_.pointerDrag(e.position.x, e.position.y, modsOf(e));
        if (m_.lassoActive() || m_.soloDragActive()) redraw();
    }
    void mouseUp(const visage::MouseEvent& e) override {
        m_.pointerUp(e.position.x, e.position.y, modsOf(e));
        redraw();
    }
    bool mouseWheel(const visage::MouseEvent& e) override {
        const double lo = m_.selected() >= 0 ? 0.0 : 1.0;
        (void)lo;
        const auto before = std::make_pair(geometryLo(), geometryHi());
        m_.wheel(e.position.x, e.position.y, e.precise_wheel_delta_y, modsOf(e));
        if (std::make_pair(geometryLo(), geometryHi()) != before && onViewChanged) onViewChanged();
        return true;
    }
    // Focus first (a text field would take keys before this frame), then the graph commands;
    // false hands the key to the host (area 15 R1-R3).
    bool keyPress(const visage::KeyEvent& e) override {
        KeyEvent k;
        k.mods = {e.isShiftDown(), e.isAltDown(), e.isMainModifier()};
        switch (e.keyCode()) {
            case visage::KeyCode::Delete: k.key = Key::Delete; break;
            case visage::KeyCode::Backspace: k.key = Key::Backspace; break;
            case visage::KeyCode::Escape: k.key = Key::Escape; break;
            case visage::KeyCode::Left: k.key = Key::Left; break;
            case visage::KeyCode::Right: k.key = Key::Right; break;
            case visage::KeyCode::Up: k.key = Key::Up; break;
            case visage::KeyCode::Down: k.key = Key::Down; break;
            case visage::KeyCode::Tab: k.key = Key::Tab; break;
            default: {
                const int code = static_cast<int>(e.keyCode());
                if (code >= 'a' && code <= 'z') {
                    k.key = Key::Character;
                    k.ch = static_cast<char32_t>(code);
                } else {
                    return false;
                }
            }
        }
        const bool handled = keys_.keyDown(k);
        if (handled) {
            redraw();
            if (onEdited) onEdited();
        }
        return handled;
    }
    GraphGeometry* geometry = nullptr;

private:
    double geometryLo() const { return geometry ? geometry->viewLo() : 0.0; }
    double geometryHi() const { return geometry ? geometry->viewHi() : 0.0; }

    // Right-click (area 14 R1): the band menu for the node or the selection, else the graph menu.
    void showMenu(float x, float y) {
        menuTargets_ = m_.contextTargets(x, y);
        redraw();
        const bool clip = keys_.clipboard.find("\"am-q.bands\"") != std::string::npos ||
                          visage::readClipboardText().find("\"am-q.bands\"") != std::string::npos;
        const auto items = menuTargets_.empty() ? graphMenu(ops_, clip) : bandMenu(ops_, menuTargets_, clip);
        visage::PopupMenu menu;
        std::map<std::string, size_t> subs;
        std::vector<visage::PopupMenu> subMenus;
        std::vector<std::string> order;  // top-level entries and submenus in item order
        for (const auto& it : items) {
            if (!it.submenu.empty()) {
                if (!subs.count(it.submenu)) {
                    subs[it.submenu] = subMenus.size();
                    subMenus.emplace_back(visage::String(it.submenu));
                    order.push_back("\x01" + it.submenu);
                }
                subMenus[subs[it.submenu]].addOption(it.action, visage::String(it.label)).enable(it.enabled).select(it.checked);
                continue;
            }
            order.push_back(std::to_string(it.action));
        }
        size_t k = 0;
        for (const auto& it : items) {
            if (!it.submenu.empty()) continue;
            (void)k;
            if (it.separatorBefore && menu.size() > 0) menu.addBreak();
            // Submenus go before the clipboard group, in their first item's position.
            if (it.action == menu::Cut)
                for (auto& s : subMenus) menu.addSubMenu(s);
            menu.addOption(it.action, visage::String(it.label)).enable(it.enabled).select(it.checked);
        }
        menu.onSelection() += [this](int id) { activateMenu(id); };
        menu.show(this, visage::Point(x, y));
    }
    void activateMenu(int id) {
        if (id == menu::SelectAll) m_.selectAll();
        else if (id == menu::ClearSelection) m_.clearSelection();
        else {
            const std::string sys = visage::readClipboardText();
            if (sys.find("\"am-q.bands\"") != std::string::npos) keys_.clipboard = sys;
            std::string msg;
            const std::string before = keys_.clipboard;
            activate(ops_, id, menuTargets_, keys_.clipboard, &msg);
            keys_.message = msg;
            if (keys_.clipboard != before) visage::setClipboardText(keys_.clipboard);
            if (id == menu::Delete || id == menu::DeleteAll || id == menu::DeleteDisabled || id == menu::Cut)
                m_.clearSelection();
        }
        redraw();
        if (onEdited) onEdited();
    }

    GraphInteraction& m_;
    BandOps& ops_;
    KeyDispatcher& keys_;
    const SnapshotPoller& p_;
    visage::Font font_;
    std::vector<int> menuTargets_;
};

// Band panel (area 12 band panel tasks; area 13 "bind every band-panel control"; area 15 R7-R9):
// the focused band's controls from the panel model, as value boxes (drag vertically; Shift fine;
// double-click resets), toggles, choice menus and buttons; the side-chain gain-reduction readout;
// output meters and analyzer toggles bound to their parameters (area 12 metering task); and a
// status line with tooltips and messages.
class PanelFrame : public visage::Frame {
public:
    PanelFrame(UndoController& c, GraphInteraction& m, BandOps& ops, KeyDispatcher& keys, const SnapshotPoller& p)
        : c_(c), m_(m), ops_(ops), keys_(keys), p_(p), label_(font(11.0f)), value_(font(12.0f)), title_(font(13.0f)) {}

    void draw(visage::Canvas& c) override {
        c.setColor(tok::surface200);
        c.fill(0, 0, width(), height());
        c.setColor(tok::lineHairline);
        c.fill(0, 0, width(), 1.0f);
        cells_.clear();
        const float meterW = 150.0f;
        drawMeters(c, width() - meterW, meterW);
        const int b = m_.focused();
        if (b < 0) {
            c.setColor(tok::inkMuted);
            c.text(visage::String("Double-click the graph to add a band. Select a band to edit it here."), label_,
                   visage::Font::kLeft, 16.0f, 12.0f, width() - meterW - 32.0f, 20.0f);
        } else {
            controls_ = bandPanel(c_.table(), b);
            c.setColor(tok::ink);
            c.text(visage::String(bandLabel(b)), title_, visage::Font::kLeft, 16.0f, 8.0f, 90.0f, 20.0f);
            const char* rows[2][3] = {{"Band", "", ""}, {"Dynamics", "Side Chain", "Spectral"}};
            float y = 32.0f;
            for (const auto& row : rows) {
                std::vector<size_t> idx;
                for (size_t i = 0; i < controls_.size(); ++i)
                    for (const char* s : row)
                        if (*s && controls_[i].section == s && controls_[i].visible) idx.push_back(i);
                const float cellW = std::min(92.0f, (width() - meterW - 24.0f) / std::max<size_t>(idx.size(), 1));
                float x = 12.0f;
                for (size_t i : idx) {
                    drawCell(c, controls_[i], x, y, cellW - 6.0f, i);
                    x += cellW;
                }
                y += 56.0f;
            }
            // Side-chain gain reduction readout (area 06 / 12 dynamics section).
            const float gr = p_.gainChange().currentDb[static_cast<size_t>(b)];
            char buf[32];
            std::snprintf(buf, sizeof buf, "GR %+.1f dB", gr);
            c.setColor(std::abs(gr) > 0.05f ? tok::gainReduction : tok::inkDisabled);
            c.text(visage::String(buf), label_, visage::Font::kLeft, 120.0f, 8.0f, 90.0f, 20.0f);
        }
        // Status: the hovered control's tooltip, else the last message (paste into a full set).
        c.setColor(tok::inkMuted);
        const std::string status = !hoverTip_.empty() ? hoverTip_ : keys_.message;
        c.text(visage::String(status), label_, visage::Font::kLeft, 16.0f, height() - 22.0f, width() - meterW - 32.0f, 18.0f);
    }

    void mouseDown(const visage::MouseEvent& e) override {
        const int k = cellAt(e.position.x, e.position.y);
        if (k < 0) return;
        const auto& cell = cells_[static_cast<size_t>(k)];
        if (!cell.enabled) return;
        const int b = m_.focused();
        if (cell.kind == ControlKind::Button) {
            if (cell.button == PanelButton::Delete) {
                ops_.remove({b});  // the same code path as the menu and the keys (area 14 R8)
                m_.clearSelection();
            } else if (cell.button == PanelButton::ClearDynamics) {
                ops_.clearDynamics({b});
            }
        } else if (cell.kind == ControlKind::Toggle) {
            EditGesture g(c_, {cell.id});
            g.set(cell.id, c_.table().value(cell.id) >= 0.5 ? 0.0 : 1.0);
        } else if (cell.kind == ControlKind::Choice) {
            showChoice(cell, b);
        } else if (e.repeatClickCount() >= 2) {  // double-click: back to the default
            EditGesture g(c_, {cell.id});
            g.set(cell.id, params::specAt(cell.id)->def);
        } else {
            dragId_ = cell.id;
            dragY_ = e.position.y;
            dragStart_ = params::toNormalised(*params::specAt(cell.id), c_.table().value(cell.id));
            gesture_.open(c_, {cell.id});
        }
        redraw();
    }
    void mouseDrag(const visage::MouseEvent& e) override {
        if (!gesture_.isOpen()) return;
        const double perPx = (e.isShiftDown() ? 0.1 : 1.0) / 200.0;  // 200 px for the full range, PROVISIONAL
        const double n = std::clamp(dragStart_ + (dragY_ - e.position.y) * perPx, 0.0, 1.0);
        gesture_.set(dragId_, params::fromNormalised(*params::specAt(dragId_), n));
        c_.dispatchNotifications();
        redraw();
    }
    void mouseUp(const visage::MouseEvent&) override {
        gesture_.close();
        redraw();
    }
    void mouseMove(const visage::MouseEvent& e) override {
        const int k = cellAt(e.position.x, e.position.y);
        const std::string tip = k >= 0 ? cells_[static_cast<size_t>(k)].tooltip : std::string();
        if (tip != hoverTip_) {
            hoverTip_ = tip;
            redraw();
        }
    }
    void mouseExit(const visage::MouseEvent&) override {
        if (!hoverTip_.empty()) {
            hoverTip_.clear();
            redraw();
        }
    }

private:
    struct Cell {
        float x = 0, y = 0, w = 0, h = 0;
        ControlKind kind = ControlKind::Knob;
        params::ParamIndex id = 0;
        PanelButton button = PanelButton::None;
        bool enabled = true;
        std::string tooltip;
    };

    void drawCell(visage::Canvas& c, const PanelControl& p, float x, float y, float w, size_t) {
        c.setColor(p.enabled ? tok::inkMuted : tok::inkDisabled);
        c.text(visage::String(p.label), label_, visage::Font::kLeft, x, y, w, 14.0f);
        const float by = y + 16.0f, bh = 24.0f;
        if (p.kind == ControlKind::Button) {
            c.setColor(tok::surface300);
            c.roundedRectangle(x, by, w, bh, 4.0f);
            c.setColor(p.enabled ? tok::ink : tok::inkDisabled);
            c.text(visage::String(p.label), value_, visage::Font::kCenter, x, by, w, bh);
        } else if (p.kind == ControlKind::Toggle) {
            const bool on = c_.table().value(p.id) >= 0.5;
            c.setColor(on && p.enabled ? tok::accent : tok::surface300);
            c.roundedRectangle(x, by, w, bh, 4.0f);
            c.setColor(on && p.enabled ? tok::surface100 : (p.enabled ? tok::ink : tok::inkDisabled));
            c.text(visage::String(on ? "On" : "Off"), value_, visage::Font::kCenter, x, by, w, bh);
        } else {
            c.setColor(tok::surface300);
            c.roundedRectangle(x, by, w, bh, 4.0f);
            c.setColor(p.enabled ? (p.kind == ControlKind::Choice ? tok::ink : tok::accent) : tok::inkDisabled);
            c.text(visage::String(p.text), value_, visage::Font::kCenter, x, by, w, bh);
        }
        Cell cell;
        cell.x = x, cell.y = by, cell.w = w, cell.h = bh;
        cell.kind = p.kind, cell.id = p.id, cell.button = p.button, cell.enabled = p.enabled;
        cell.tooltip = p.tooltip;
        cells_.push_back(cell);
    }

    void drawMeters(visage::Canvas& c, float x0, float w) {
        // Output meters (area 11 R3, area 12 metering task): peak bars with RMS, per channel.
        const auto& lv = p_.levels();
        const float top = 12.0f, bottom = height() - 48.0f;
        auto yOf = [&](float db) { return top + (bottom - top) * std::clamp(-db / 60.0f, 0.0f, 1.0f); };
        for (int ch = 0; ch < 2; ++ch) {
            const float mx = x0 + 12.0f + ch * 16.0f;
            c.setColor(tok::surface300);
            c.fill(mx, top, 10.0f, bottom - top);
            const auto& l = lv.output[static_cast<size_t>(ch)];
            c.setColor(l.peakDb > -0.1f ? tok::meterHot : tok::meter);
            c.fill(mx, yOf(l.peakDb), 10.0f, bottom - yOf(l.peakDb));
            c.setColor(tok::ink);
            c.fill(mx, yOf(l.rmsDb), 10.0f, 2.0f);
        }
        c.setColor(tok::inkMuted);
        c.text(visage::String("OUT"), label_, visage::Font::kCenter, x0 + 8.0f, bottom + 2.0f, 40.0f, 14.0f);
        // Analyzer source toggles bound to their parameters (area 12, area 19).
        struct Btn {
            const char* name;
            params::GlobalParam g;
        };
        const Btn btns[] = {{"Pre", params::GlobalParam::AnalyzerShowPre},
                            {"Post", params::GlobalParam::AnalyzerShowPost},
                            {"SC", params::GlobalParam::AnalyzerShowExternal}};
        float y = 12.0f;
        for (const auto& bt : btns) {
            const auto id = params::globalParam(bt.g);
            const bool on = c_.table().value(id) >= 0.5;
            const float bx = x0 + 60.0f, bw = w - 72.0f;
            c.setColor(on ? tok::accent : tok::surface300);
            c.roundedRectangle(bx, y, bw, 22.0f, 4.0f);
            c.setColor(on ? tok::surface100 : tok::ink);
            c.text(visage::String(bt.name), value_, visage::Font::kCenter, bx, y, bw, 22.0f);
            Cell cell;
            cell.x = bx, cell.y = y, cell.w = bw, cell.h = 22.0f;
            cell.kind = ControlKind::Toggle, cell.id = id;
            cells_.push_back(cell);
            y += 28.0f;
        }
    }

    void showChoice(const Cell& cell, int band) {
        visage::PopupMenu menu;
        for (const auto& it : choiceItems(c_.table(), cell.id))
            menu.addOption(it.value, visage::String(it.label)).enable(it.enabled).select(it.checked);
        const auto id = cell.id;
        menu.onSelection() += [this, id, band](int v) {
            if (id == params::bandParam(band, params::BandField::Shape)) {
                ops_.setShape({band}, static_cast<dsp::Shape>(v));  // one undo step (area 15 R8)
            } else {
                EditGesture g(c_, {id});
                g.set(id, v);
            }
            redraw();
        };
        menu.show(this, visage::Point(cell.x, cell.y + cell.h));
    }

    int cellAt(float x, float y) const {
        for (size_t i = 0; i < cells_.size(); ++i) {
            const auto& c = cells_[i];
            if (x >= c.x && x <= c.x + c.w && y >= c.y && y <= c.y + c.h) return static_cast<int>(i);
        }
        return -1;
    }

    UndoController& c_;
    GraphInteraction& m_;
    BandOps& ops_;
    KeyDispatcher& keys_;
    const SnapshotPoller& p_;
    visage::Font label_, value_, title_;
    std::vector<PanelControl> controls_;
    std::vector<Cell> cells_;
    std::string hoverTip_;
    EditGesture gesture_;
    params::ParamIndex dragId_ = 0;
    float dragY_ = 0.0f;
    double dragStart_ = 0.0;
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
            case AccessibleAction::Press: m_.setSelection({band_}, band_); return true;
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
    UndoController& c;
    visage::ApplicationWindow app;
    GraphGeometry geometry;
    ResponseCurve curve;
    GraphInteraction interaction;
    BandOps ops;
    KeyDispatcher keys;
    // Snapshot polling on Visage's event loop, which runs timers on the display-paced vblank
    // tick (area 12 R5/R10); only frames whose data changed redraw.
    SnapshotPoller poller;
    visage::EventTimer pollTimer;
    GroundFrame ground;
    SpectrumFrame spectrum;
    CurveFrame curveFrame;
    NodesFrame nodes;
    PanelFrame panel;
    std::array<std::unique_ptr<BandNodeAccessible>, params::kBandCount> nodeAccess;
    GraphAccessible graphAccess;
    RootAccessible rootAccess;

    explicit Impl(UndoController& ctl)
        : c(ctl),
          interaction(ctl, geometry),
          ops(ctl),
          keys(interaction, ops),
          ground(geometry),
          spectrum(geometry, ctl.table(), poller),
          curveFrame(geometry, curve, ctl.table()),
          nodes(interaction, ops, keys, poller),
          panel(ctl, interaction, ops, keys, poller),
          graphAccess(nodeAccess),
          rootAccess(graphAccess) {
        for (int b = 0; b < params::kBandCount; ++b) nodeAccess[static_cast<size_t>(b)] = std::make_unique<BandNodeAccessible>(ctl, interaction, b);
        nodes.geometry = &geometry;
        keys.writeSystemClipboard = [](const std::string& s) { visage::setClipboardText(s); };
        keys.readSystemClipboard = [] { return visage::readClipboardText(); };
        interaction.onSelectionChanged = [this] {
            nodes.redraw();
            panel.redraw();
        };
        nodes.onViewChanged = [this] { relayoutCurve(); };
        nodes.onEdited = [this] { panel.redraw(); };
        c.addListener(this);
        pollTimer.onTimerCallback() += [this] {
            const unsigned changed = poller.poll(c);
            if (changed & SnapshotPoller::kSpectrum) spectrum.redraw();
            if (changed & (SnapshotPoller::kLevels | SnapshotPoller::kGainChange)) panel.redraw();
            if (changed & SnapshotPoller::kGainChange) nodes.redraw();
        };
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

    // The curve's frequency grid follows the zoomed span.
    void relayoutCurve() {
        std::vector<double> hz;
        const int n = std::max(64, static_cast<int>(geometry.width() / 2.0f));
        for (int i = 0; i <= n; ++i) hz.push_back(geometry.viewLo() * std::pow(geometry.viewHi() / geometry.viewLo(), i / double(n)));
        curve.setFrequencies(std::move(hz));
        ground.redraw();
        spectrum.redraw();
        curveFrame.redraw();
        nodes.redraw();
    }

    void layout() {
        checkDpiChange();
        const float w = app.width() > 0 ? app.width() : static_cast<float>(kDefaultWidth);
        const float h = app.height() > 0 ? app.height() : static_cast<float>(kDefaultHeight);
        if (!dpiResizePending) {
            wantW = w;
            wantH = h;
        }
        const float gh = std::max(120.0f, h - kPanelHeight);
        geometry.setSize(w, gh);
        geometry.setRangeDb(rangeDb());
        ground.setBounds(0, 0, w, gh);
        spectrum.setBounds(0, 0, w, gh);
        curveFrame.setBounds(0, 0, w, gh);
        nodes.setBounds(0, 0, w, gh);
        panel.setBounds(0, gh, w, h - gh);
        relayoutCurve();
        panel.redraw();
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
            panel.redraw();
            return;
        }
        // Display settings redraw only their own layer (area 13 "redraw only on change").
        if (i == params::globalParam(params::GlobalParam::DisplayRange)) {
            geometry.setRangeDb(rangeDb());
            ground.redraw();
            curveFrame.redraw();
            nodes.redraw();
        } else if (i == params::globalParam(params::GlobalParam::GainScale)) {
            curveFrame.redraw();
        } else if (i >= params::globalParam(params::GlobalParam::AnalyzerShowPre) &&
                   i <= params::globalParam(params::GlobalParam::AnalyzerGrab)) {
            spectrum.redraw();
            panel.redraw();
        } else if (i == params::globalParam(params::GlobalParam::SoloGain)) {
            nodes.redraw();
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

Editor::Editor(UndoController& controller, uint32_t logicalWidth, uint32_t logicalHeight)
    : impl_(std::make_unique<Impl>(controller)) {
    ++gLiveEditors;
    impl_->owner = this;
    auto& app = impl_->app;
    app.setMinimumDimensions(static_cast<float>(kMinWidth), static_cast<float>(kMinHeight));
    app.setWindowDimensions(static_cast<float>(std::clamp(logicalWidth, kMinWidth, kMaxWidth)),
                            static_cast<float>(std::clamp(logicalHeight, kMinHeight, kMaxHeight)));
    app.addChild(&impl_->ground);
    app.addChild(&impl_->spectrum);
    app.addChild(&impl_->curveFrame);
    app.addChild(&impl_->nodes);
    app.addChild(&impl_->panel);
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
