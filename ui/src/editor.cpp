#include "amq/ui/editor.h"

#include "amq/dsp/latency.hpp"
#include "amq/dsp/log_grid.hpp"
#include "amq/params/mappings.hpp"
#include "amq/params/shape_info.hpp"
#include "amq/ui/band_menu.hpp"
#include "amq/ui/band_ops.hpp"
#include "amq/ui/band_panel.hpp"
#include "amq/ui/display_format.hpp"
#include "amq/ui/editor_layout.hpp"
#include "amq/ui/eq_match_apply.hpp"
#include "amq/ui/file_dialog.hpp"
#include "amq/ui/graph_model.hpp"
#include "amq/ui/key_dispatcher.hpp"
#include "amq/ui/snapshot_poller.hpp"

#include "editor_theme.hpp"

#include <clap/ext/gui.h>
#include <visage/app.h>
#include <visage/ui.h>
#include <visage/windowing.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

// The editor, built to docs/design/editor-spec.md (region map §1, components and states in
// editor-spec-panels.md, overlays and the graph in editor-spec-graph.md). Frames:
//   GroundFrame    grid, 0 dB line, axis labels (cached)
//   SpectrumFrame  analyzer layers (pre fill, collisions, peak hold, post, side chain)
//   CurvesFrame    band fills and strokes, live dynamic curves, the summed curve; EQ Match layers
//   MeterFrame     output meters in the right gutter
//   GraphTop       handles, readouts, lasso, the band panel, band list and legends; graph input
//   TopBar, StatusBar
//   Overlay        menus and popovers (one family at a time, spec §4.0) and the inline editor

namespace amq::ui {

namespace {

using namespace theme;
using params::BandField;
using params::GlobalParam;

std::atomic<int> gLiveEditors{0};

Modifiers modsOf(const visage::MouseEvent& e) { return {e.isShiftDown(), e.isAltDown(), e.isMainModifier()}; }

std::string bandLabel(int b) { return "Band " + std::to_string(b + 1); }

// ---------------------------------------------------------------------------
// Shared editor state
// ---------------------------------------------------------------------------

struct MatchUi {
    explicit MatchUi(UndoController& c) : applier(c) {}
    EqMatchApplier applier;
    MatchStatus st;
    double amount = 1.0;
    int count = 0;
    bool applied = false;
    std::string msg;
};

// One clickable or draggable area, registered while drawing.
struct Hit {
    Rect r;
    int kind = 0;
    int arg = 0;
    params::ParamIndex id = 0;
};

enum HitKind : int {
    kNone = 0,
    // Top bar
    kUndo, kRedo, kPreset, kEqMatch, kSettings, kHelp, kOverflow,
    // Status bar
    kBands, kProcessing, kAnalyzer, kFreeze, kToggle, kDragValue, kOutputMenu,
    // Band panel
    kShape, kSlope, kPlacement, kChoiceMenu, kDelete, kScExt, kScFilter, kStripExpand, kKnob, kKnobValue,
    // Band list
    kBandRow,
    // Overlay
    kMenuItem, kSubParent, kSlopeOpt, kMode, kResolution, kSegParam, kTiltPresets, kSelectMenu, kInlineField,
    // EQ Match
    kMatchRec, kMatchReset, kMatchSource, kMatchWindow, kMatchSmoothing, kMatchAlign, kMatchSlider, kMatchPrimary,
    kMatchCancel,
};

int hitAt(const std::vector<Hit>& hits, float x, float y) {
    for (size_t i = hits.size(); i-- > 0;)
        if (hits[i].r.contains(x, y)) return static_cast<int>(i);
    return -1;
}

struct Ctx {
    explicit Ctx(UndoController& ctl) : c(ctl), interaction(ctl, geometry), ops(ctl), keys(interaction, ops) {}
    UndoController& c;
    GraphGeometry geometry;
    ResponseCurve curve;
    GraphInteraction interaction;
    BandOps ops;
    KeyDispatcher keys;
    SnapshotPoller poller;
    EditorLayout L;

    bool matchMode = false, hintMode = false, bandListOpen = false, stripExpanded = false;
    bool settingsOpen = false;  // the analyzer legend shows while the Analyzer panel is open
    std::unique_ptr<MatchUi> match;

    std::function<void()> redrawAll, relayout, redrawGraph, redrawBars;

    // Tooltips (spec §4.0 layer 9): the hovered control's text, shown after 600 ms.
    std::string tipText;
    Rect tipAnchor;  // window coordinates
    double tipSince = 0.0;
    bool tipShown = false;
    std::function<void()> tipChanged;
    void hoverTip(const std::string& text, Rect anchor) {
        if (text == tipText && (text.empty() || (anchor.x == tipAnchor.x && anchor.y == tipAnchor.y))) return;
        tipText = text, tipAnchor = anchor, tipSince = theme::nowMs(), tipShown = false;
        if (tipChanged) tipChanged();
    }
    void clearTip() { hoverTip("", {}); }
    // Motion (spec §7): frames call kickAnimation() when a target changes.
    std::function<void()> kickAnimation;

    [[nodiscard]] const params::ParameterTable& table() const { return c.table(); }
    [[nodiscard]] double val(params::ParamIndex i) const { return c.table().value(i); }
    [[nodiscard]] double gval(GlobalParam g) const { return val(params::globalParam(g)); }
    [[nodiscard]] double bval(int b, BandField f) const { return val(params::bandParam(b, f)); }
    [[nodiscard]] bool on(params::ParamIndex i) const { return val(i) >= 0.5; }

    void set(params::ParamIndex id, double v) {
        EditGesture g(c, {id});
        g.set(id, v);
        c.dispatchNotifications();
    }
    void toggle(params::ParamIndex id) { set(id, on(id) ? 0.0 : 1.0); }

    // Value drags (knobs, readouts, sliders): vertical, 200 px for the full range, Shift fine.
    EditGesture drag;
    params::ParamIndex dragId = 0;
    float dragY = 0.0f;
    double dragStart = 0.0;
    void beginDrag(params::ParamIndex id, float y) {
        dragId = id, dragY = y;
        dragStart = params::toNormalised(*params::specAt(id), val(id));
        drag.open(c, {id});
    }
    void dragTo(float y, bool fine) {
        if (!drag.isOpen()) return;
        const double n = std::clamp(dragStart + (dragY - y) * (fine ? 0.1 : 1.0) / 200.0, 0.0, 1.0);
        drag.set(dragId, params::fromNormalised(*params::specAt(dragId), n));
        c.dispatchNotifications();
    }
    void endDrag() { drag.close(); }
    void resetToDefault(params::ParamIndex id) { set(id, params::specAt(id)->def); }

    [[nodiscard]] double sampleRate() const { return c.sampleRate() > 0.0 ? c.sampleRate() : 48000.0; }
    [[nodiscard]] bool spectralActive() const {
        for (int b = 0; b < params::kBandCount; ++b)
            if (bval(b, BandField::Used) >= 0.5 && bval(b, BandField::Enabled) >= 0.5 && bval(b, BandField::DynEnabled) >= 0.5 &&
                bval(b, BandField::Spectral) >= 0.5 &&
                dsp::shapeHasGain(static_cast<dsp::Shape>(static_cast<int>(bval(b, BandField::Shape)))))
                return true;
        return false;
    }
    [[nodiscard]] uint32_t latency(int mode, int resolution) const {
        return dsp::latencySamples(static_cast<dsp::ProcessingMode>(mode), resolution, sampleRate(), spectralActive());
    }
    [[nodiscard]] int usedCount() const { return static_cast<int>(ops.usedBands().size()); }
    [[nodiscard]] PanelContent panelContent() const {
        PanelContent pc;
        const int b = interaction.focused();
        if (b < 0 || bval(b, BandField::Used) < 0.5 || matchMode) return pc;
        const auto info = params::shapeInfo(static_cast<dsp::Shape>(static_cast<int>(bval(b, BandField::Shape))));
        pc.band = true, pc.gain = info.hasGain, pc.q = info.hasQ, pc.slope = info.hasSlope;
        pc.dynamic = bval(b, BandField::DynEnabled) >= 0.5;
        pc.spectral = pc.dynamic && bval(b, BandField::Spectral) >= 0.5;
        pc.rangeKnob = info.hasGain;
        return pc;
    }
    [[nodiscard]] std::string analyzerSummary() const {
        std::string s;
        auto add = [&](GlobalParam g, const char* n) {
            if (gval(g) >= 0.5) s += (s.empty() ? "" : " + ") + std::string(n);
        };
        add(GlobalParam::AnalyzerShowPre, "Pre");
        add(GlobalParam::AnalyzerShowPost, "Post");
        add(GlobalParam::AnalyzerShowExternal, "SC");
        return s.empty() ? "Off" : s;
    }
    [[nodiscard]] double displayRange() const {
        static const double r[] = {3.0, 6.0, 12.0, 30.0};
        return r[std::clamp(static_cast<int>(gval(GlobalParam::DisplayRange)), 0, 3)];
    }
    [[nodiscard]] double analyzerRange() const {
        static const double r[] = {60.0, 90.0, 120.0};
        return r[std::clamp(static_cast<int>(gval(GlobalParam::AnalyzerRange)), 0, 2)];
    }
};

const char* kModeNames[] = {"Zero Latency", "Natural Phase", "Linear Phase"};

// Tooltip text for a control (spec panels §2; PROVISIONAL wording).
std::string tipFor(const Ctx& x, const Hit& h) {
    auto g = [](GlobalParam p) { return params::globalParam(p); };
    switch (h.kind) {
        case kUndo: return "Undo (Ctrl+Z)";
        case kRedo: return "Redo (Ctrl+Shift+Z)";
        case kSettings: return "Interface and analyzer settings";
        case kHelp: return x.hintMode ? "Hide the gesture hints" : "Show the gesture hints";
        case kEqMatch: return x.matchMode ? "Leave EQ Match" : "EQ Match";
        case kFreeze: return "Unfreeze the analyzer";
        case kBands: return "Band list";
        case kProcessing: return "Processing mode and latency";
        case kAnalyzer: return "Analyzer settings";
        case kOutputMenu: return "Output settings";
        case kDragValue: return "Drag to change, Shift for fine, double-click to reset";
        case kToggle:
            if (h.id == g(GlobalParam::AnalyzerFreeze)) return "Freeze analyzer";
            if (h.id == g(GlobalParam::Invert)) return "Invert polarity";
            if (h.id == g(GlobalParam::Bypass)) return "Bypass plugin";
            if (h.id == g(GlobalParam::AutoGain)) return "Keeps loudness level while you EQ";
            switch (h.arg) {
                case 1: return "Enable/disable band (Alt-click the handle)";
                case 2: return "Automatic threshold and timing";
                case 3: return "Listen to side chain";
                case 7: return "Solo band";
                default: return "";
            }
        case kDelete: return "Delete band (Del)";
        case kShape: return "Filter shape";
        case kSlope: return "Slope";
        case kPlacement: return "Stereo placement";
        case kScFilter: return "Side-chain filter";
        case kKnob: {
            // Attack and Release show their effective time (area 06, from the panel model).
            const int b = static_cast<int>(h.id) / params::kBandFieldCount;
            for (const auto& pc : bandPanel(x.table(), b))
                if (pc.id == h.id && !pc.tooltip.empty()) return pc.tooltip;
            return "Drag to change, Shift for fine, double-click to reset";
        }
        case kKnobValue: return "Click to type a value";
        case kMatchRec: return "Record or stop this capture";
        case kMatchReset: return "Reset this capture";
        case kMatchPrimary:
            if (x.match && !x.match->applier.fit().empty() && !x.match->applied) return "Writes bands as one undo step";
            if (x.match && x.match->st.fitting) return "Stop the fit; the bands stay unchanged";
            return "Fit bands to the difference";
        default: return "";
    }
}

// ---------------------------------------------------------------------------
// Overlay: menus, popovers and the inline editor (editor-spec-graph §4)
// ---------------------------------------------------------------------------

struct Entry {
    int id = 0;
    std::string label, hint, glyph;
    bool enabled = true, checked = false, mixed = false, checkbox = false, radio = false, separatorBefore = false;
    std::vector<Entry> sub;
    bool slopeGrid = false;  // a submenu drawn as the slope grid
};

enum class Pop : uint8_t { None, Menu, Slope, Processing, Output, Analyzer, Settings, ScFilter, Inline };

class Overlay : public visage::Frame {
public:
    explicit Overlay(Ctx& x) : x_(x) {
        setAcceptsKeystrokes(true);
        setVisible(false);
    }

    [[nodiscard]] Pop kind() const { return closing_ ? Pop::None : pop_; }
    // Closing fades out over 80 ms (spec §7); the overlay ignores input meanwhile.
    void close() {
        if (pop_ == Pop::None || closing_) return;
        x_.endDrag();
        x_.settingsOpen = false;
        closing_ = true;
        setIgnoresMouseEvents(true, false);
        if (x_.kickAnimation) x_.kickAnimation();
        if (x_.redrawAll) x_.redrawAll();
    }
    // Advances the open and close fades; returns true while still moving.
    bool animate(double dtMs) {
        if (pop_ == Pop::None) return false;
        if (closing_) {
            alpha_ = static_cast<float>(alpha_ - dtMs / 80.0);
            if (alpha_ <= 0.0f) {
                finishClose();
                return false;
            }
        } else if (alpha_ < 1.0f) {
            alpha_ = static_cast<float>(std::min(1.0, alpha_ + dtMs / 120.0));
        } else {
            return false;
        }
        setAlphaTransparency(std::clamp(alpha_, 0.0f, 1.0f));
        redraw();
        return true;
    }
    // A menu anchored at a point (right-click) or under/over a rectangle (selects).
    void openMenu(std::string caption, float width, std::vector<Entry> items, std::function<void(int)> pick, Rect anchor,
                  bool atPoint) {
        caption_ = std::move(caption), width_ = width, items_ = std::move(items), pick_ = std::move(pick);
        anchor_ = anchor, atPoint_ = atPoint;
        show(Pop::Menu);
    }
    void openPopover(Pop p, Rect anchor, int band = -1) {
        anchor_ = anchor, band_ = band;
        if (p == Pop::Analyzer || p == Pop::Settings) x_.settingsOpen = true;
        show(p);
    }
    struct Field {
        std::string label;
        params::ParamIndex id = 0;
        std::string text;
        bool invalid = false;
    };
    void openInline(Rect anchor, std::vector<Field> fields) {
        fields_ = std::move(fields), field_ = 0, anchor_ = anchor;
        show(Pop::Inline);
    }

    void draw(visage::Canvas& c) override {
        hits_.clear();
        panels_.clear();
        // Opening slides 4 px towards the anchor (spec §7); bottom-anchored overlays rise.
        const float slide = 4.0f * (1.0f - std::clamp(alpha_, 0.0f, 1.0f)) * (anchor_.y > height() * 0.5f ? 1.0f : -1.0f);
        c.setPosition(0.0f, slide);
        switch (pop_) {
            case Pop::Menu: drawMenu(c); break;
            case Pop::Slope: drawSlope(c); break;
            case Pop::Processing: drawProcessing(c); break;
            case Pop::Output: drawOutput(c); break;
            case Pop::Analyzer: drawAnalyzer(c); break;
            case Pop::Settings:
                drawAnalyzer(c);
                drawInterface(c);
                break;
            case Pop::ScFilter: drawScFilter(c); break;
            case Pop::Inline: drawInline(c); break;
            case Pop::None: break;
        }
    }

    // Visage sends only mouseEnter on the move that enters a frame: treat it as a move.
    void mouseEnter(const visage::MouseEvent& e) override { mouseMove(e); }
    void mouseMove(const visage::MouseEvent& e) override {
        const int h = hitAt(hits_, e.position.x, e.position.y);
        if (h != hover_) {
            hover_ = h;
            if (h >= 0 && hits_[static_cast<size_t>(h)].kind == kSubParent) sub_ = hits_[static_cast<size_t>(h)].arg;
            redraw();
        }
    }
    void mouseExit(const visage::MouseEvent&) override {
        hover_ = -1;
        redraw();
    }
    void mouseDown(const visage::MouseEvent& e) override {
        const float px = e.position.x, py = e.position.y;
        const int h = hitAt(hits_, px, py);
        if (h < 0) {
            bool inside = false;
            for (const auto& r : panels_) inside = inside || r.contains(px, py);
            if (!inside) {
                if (pop_ == Pop::Inline) commitInline();
                close();
            }
            return;
        }
        const Hit hit = hits_[static_cast<size_t>(h)];
        switch (hit.kind) {
            case kMenuItem: {
                auto pick = pick_;
                close();
                if (pick) pick(hit.arg);
                return;
            }
            case kSubParent: sub_ = hit.arg; break;
            case kSlopeOpt: {
                const int b = band_;
                close();
                if (b >= 0) x_.ops.setSlope({b}, static_cast<dsp::Slope>(hit.arg));
                return;
            }
            case kMode: x_.set(params::globalParam(GlobalParam::Mode), hit.arg); break;
            case kResolution: x_.set(params::globalParam(GlobalParam::Resolution), hit.arg); break;
            case kSegParam: x_.set(hit.id, hit.arg); break;
            case kToggle:
                if (hit.arg == -1) x_.interaction.gainQCoupling = !x_.interaction.gainQCoupling;  // editor state
                else x_.toggle(hit.id);
                break;
            case kDragValue:
                if (e.repeatClickCount() >= 2) x_.resetToDefault(hit.id);
                else x_.beginDrag(hit.id, py);
                break;
            case kSelectMenu: openSelect(hit); return;
            case kTiltPresets: {
                std::vector<Entry> items;
                const double v = x_.gval(GlobalParam::AnalyzerTilt);
                for (int k = 0; k <= 4; ++k) {
                    Entry en;
                    en.id = k, en.label = fmt::dbPerOct(1.5 * k), en.radio = true, en.checked = std::abs(v - 1.5 * k) < 1e-6;
                    items.push_back(en);
                }
                const auto id = params::globalParam(GlobalParam::AnalyzerTilt);
                Ctx& cx = x_;
                openMenu("", 140, items, [&cx, id](int k) { cx.set(id, 1.5 * k); }, hit.r, false);
                return;
            }
            case kInlineField: field_ = hit.arg; break;
            default: break;
        }
        redraw();
        if (x_.redrawAll) x_.redrawAll();
    }
    void mouseDrag(const visage::MouseEvent& e) override {
        x_.dragTo(e.position.y, e.isShiftDown());
        redraw();
    }
    void mouseUp(const visage::MouseEvent&) override { x_.endDrag(); }

    bool receivesTextInput() override { return pop_ == Pop::Inline; }
    void textInput(const std::string& t) override {
        if (pop_ != Pop::Inline || fields_.empty()) return;
        auto& f = fields_[static_cast<size_t>(field_)];
        if (fresh_) f.text.clear(), fresh_ = false;
        f.text += t;
        f.invalid = false;
        redraw();
    }
    bool keyPress(const visage::KeyEvent& e) override {
        if (pop_ == Pop::None) return false;
        const auto k = e.keyCode();
        if (k == visage::KeyCode::Escape) {
            if (sub_ >= 0 && pop_ == Pop::Menu) sub_ = -1, redraw();
            else close();
            return true;
        }
        if (pop_ != Pop::Inline) return false;  // menus: other keys pass on (area 15)
        auto& f = fields_[static_cast<size_t>(field_)];
        if (k == visage::KeyCode::Backspace) {
            if (fresh_) f.text.clear(), fresh_ = false;
            else if (!f.text.empty()) {
                // Drop one UTF-8 code point.
                size_t n = f.text.size() - 1;
                while (n > 0 && (static_cast<unsigned char>(f.text[n]) & 0xc0) == 0x80) --n;
                f.text.erase(n);
            }
            redraw();
            return true;
        }
        if (k == visage::KeyCode::Return || k == visage::KeyCode::KPEnter) {
            if (commitInline()) close();
            return true;
        }
        if (k == visage::KeyCode::Tab) {
            if (commitField(field_)) field_ = (field_ + 1) % static_cast<int>(fields_.size()), fresh_ = true;
            redraw();
            return true;
        }
        return true;  // a focused text field takes every key (area 15 R4)
    }

private:
    void finishClose() {
        closing_ = false;
        pop_ = Pop::None, sub_ = -1, hover_ = -1;
        alpha_ = 0.0f;
        setIgnoresMouseEvents(false, false);
        setVisible(false);
        if (x_.redrawAll) x_.redrawAll();
    }
    void show(Pop p) {
        closing_ = false;
        setIgnoresMouseEvents(false, false);
        pop_ = p, sub_ = -1, hover_ = -1, fresh_ = true;
        alpha_ = 0.0f;
        setAlphaTransparency(0.0f);
        if (x_.kickAnimation) x_.kickAnimation();
        setVisible(true);
        requestKeyboardFocus();
        redraw();
        if (x_.redrawAll) x_.redrawAll();
    }

    // Places a panel of size w×h at the anchor: above or below a control, or at a point; flips and
    // clamps to keep 16 px from the window edges (spec §4.0).
    Rect place(float w, float h, bool preferAbove, bool alignRight = false) const {
        const float W = width(), H = height();
        Rect r{alignRight ? anchor_.right() - w : anchor_.x, 0.0f, w, h};
        if (atPoint_ && pop_ == Pop::Menu) {
            r.x = anchor_.x, r.y = anchor_.y;
            if (r.right() > W - 16.0f) r.x = anchor_.x - w;
            if (r.bottom() > H - 16.0f) r.y = anchor_.y - h;
        } else {
            const float above = anchor_.y - 8.0f - h, below = anchor_.bottom() + 8.0f;
            r.y = preferAbove ? (above >= 16.0f ? above : below) : (below + h <= H - 16.0f ? below : above);
        }
        r.x = std::clamp(r.x, 16.0f, std::max(16.0f, W - 16.0f - w));
        r.y = std::clamp(r.y, 16.0f, std::max(16.0f, H - 16.0f - h));
        return r;
    }
    void addHit(Rect r, int kind, int arg = 0, params::ParamIndex id = 0) { hits_.push_back({r, kind, arg, id}); }
    [[nodiscard]] bool hovered(int kind, int arg = 0) const {
        if (hover_ < 0) return false;
        const auto& h = hits_[static_cast<size_t>(hover_)];
        return h.kind == kind && h.arg == arg;
    }

    // --- Menus (band menu, select menus, presets) ---
    void drawMenuPanel(visage::Canvas& c, Rect r, const std::string& caption, const std::vector<Entry>& items, int level) {
        panels_.push_back(r);
        theme::panel(c, r, 16.0f);
        float y = r.y + 6.0f;
        const auto& F = fonts();
        if (!caption.empty()) {
            text(c, F.semi12, caption, r.x + 16.0f, y + 6.0f, r.w - 32.0f, 14.0f, tok::inkMuted);
            y += 26.0f;
        }
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& it = items[i];
            if (it.separatorBefore && i > 0) {
                c.setColor(tok::lineHairline);
                c.fill(r.x + 12.0f, y + 4.0f, r.w - 24.0f, 1.0f);
                y += 9.0f;
            }
            const Rect ir{r.x + 6.0f, y, r.w - 12.0f, 30.0f};
            const bool parent = !it.sub.empty();
            const int kind = parent ? kSubParent : kMenuItem;
            const int arg = parent ? static_cast<int>(i) + 1000 * level : it.id;
            const bool hl = it.enabled && (hovered(kind, arg) || (parent && level == 0 && sub_ == static_cast<int>(i)));
            if (hl) {
                c.setColor(tok::surface300);
                c.roundedRectangle(ir.x, ir.y, ir.w, ir.h, 6.0f);
            }
            const uint32_t fg = it.enabled ? tok::ink : tok::inkMuted;
            if (!it.glyph.empty()) icon(c, it.glyph, ir.x + 10.0f, ir.y + 7.0f, it.checked && !it.checkbox ? tok::accent : fg);
            else if (it.checkbox || it.radio)
                if (it.checked || it.mixed) icon(c, it.mixed ? icons::dash : icons::check, ir.x + 10.0f, ir.y + 7.0f, tok::accent);
            const float tx = ir.x + 36.0f;
            text(c, F.label13, it.label, tx, ir.y, ir.w - 46.0f, ir.h, fg);
            if (parent) icon(c, icons::chevRight, ir.right() - 22.0f, ir.y + 9.0f, tok::inkMuted);
            else if (!it.hint.empty())
                text(c, F.caption12, it.hint, ir.x, ir.y, ir.w - 10.0f, ir.h, tok::inkMuted, visage::Font::kRight);
            if (it.enabled) addHit(ir, kind, arg);
            if (parent && level == 0 && sub_ == static_cast<int>(i)) subAnchor_ = ir;
            y += 30.0f;
        }
    }
    static float menuHeight(const std::string& caption, const std::vector<Entry>& items) {
        float h = 12.0f + (caption.empty() ? 0.0f : 26.0f);
        for (size_t i = 0; i < items.size(); ++i) h += 30.0f + (items[i].separatorBefore && i > 0 ? 9.0f : 0.0f);
        return h;
    }
    void drawMenu(visage::Canvas& c) {
        const Rect r = place(width_, menuHeight(caption_, items_), false);
        subAnchor_ = {};
        drawMenuPanel(c, r, caption_, items_, 0);
        if (sub_ >= 0 && sub_ < static_cast<int>(items_.size()) && !items_[static_cast<size_t>(sub_)].sub.empty()) {
            const auto& s = items_[static_cast<size_t>(sub_)];
            const float w = s.slopeGrid ? 236.0f : 176.0f;
            const float h = s.slopeGrid ? slopeHeight() : menuHeight("", s.sub);
            Rect sr{r.right() + 4.0f, subAnchor_.y - 6.0f, w, h};
            if (sr.right() > width() - 16.0f) sr.x = r.x - 4.0f - w;
            sr.y = std::clamp(sr.y, 16.0f, std::max(16.0f, height() - 16.0f - h));
            if (s.slopeGrid) drawSlopeGrid(c, sr, s.sub, "SLOPE");
            else drawMenuPanel(c, sr, "", s.sub, 1);
        }
    }
    void openSelect(const Hit& h) {
        // Choice parameters from the popovers (analyzer range, display range).
        std::vector<Entry> items;
        const auto* spec = params::specAt(h.id);
        const int cur = static_cast<int>(x_.val(h.id));
        for (size_t i = 0; i < spec->choices.size(); ++i) {
            Entry en;
            en.id = static_cast<int>(i), en.radio = true, en.checked = en.id == cur;
            en.label = h.arg == 1 ? "\xC2\xB1" + spec->choices[i] : spec->choices[i];
            items.push_back(en);
        }
        Ctx& cx = x_;
        const auto id = h.id;
        openMenu("", std::max(120.0f, h.r.w), items, [&cx, id](int v) { cx.set(id, v); }, h.r, false);
    }

    // --- Slope picker (spec §4.3) ---
    static float slopeHeight() { return 10.0f + 20.0f + 3 * 30.0f + 6.0f + 28.0f + 8.0f + 32.0f + 10.0f; }
    void drawSlopeGrid(visage::Canvas& c, Rect r, const std::vector<Entry>& items, const std::string& caption) {
        panels_.push_back(r);
        theme::panel(c, r, 16.0f);
        const auto& F = fonts();
        float y = r.y + 10.0f;
        text(c, F.semi12, caption, r.x + 14.0f, y, r.w - 28.0f, 14.0f, tok::inkMuted);
        y += 20.0f;
        const float cw = (r.w - 20.0f - 4.0f) / 3.0f;
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& it = items[i];
            const bool brick = i == 9;
            Rect o = brick ? Rect{r.x + 10.0f, y + 3 * 30.0f + 6.0f, r.w - 20.0f, 28.0f}
                           : Rect{r.x + 10.0f + static_cast<float>(i % 3) * (cw + 2.0f), y + static_cast<float>(i / 3) * 30.0f, cw, 28.0f};
            const int kind = pop_ == Pop::Slope ? kSlopeOpt : kMenuItem;
            const bool hl = it.enabled && hovered(kind, it.id);
            uint32_t bg = brick ? tok::surface300 : 0, fg = it.enabled ? tok::ink : tok::inkMuted;
            if (it.checked) bg = tok::accent, fg = tok::accentInk;
            else if (hl) bg = tok::surface300;
            if (bg) {
                c.setColor(bg);
                c.roundedRectangle(o.x, o.y, o.w, o.h, 6.0f);
            }
            text(c, it.checked ? F.semi13 : F.label13, it.label, o.x, o.y, o.w, o.h, fg, visage::Font::kCenter);
            if (it.enabled) addHit(o, kind, it.id);
        }
        text(c, F.caption12, "dB/oct. Brickwall on Low and", r.x + 14.0f, r.bottom() - 42.0f, r.w - 28.0f, 16.0f, tok::inkMuted);
        text(c, F.caption12, "High Cut only.", r.x + 14.0f, r.bottom() - 26.0f, r.w - 28.0f, 16.0f, tok::inkMuted);
    }
    void drawSlope(visage::Canvas& c) {
        if (band_ < 0) return;
        const auto shape = static_cast<dsp::Shape>(static_cast<int>(x_.bval(band_, BandField::Shape)));
        const int cur = static_cast<int>(x_.bval(band_, BandField::Slope));
        std::vector<Entry> items;
        for (int s = 0; s < dsp::kSlopeCount; ++s) {
            Entry en;
            en.id = s, en.label = fmt::slopeNumber(s), en.checked = s == cur, en.enabled = dsp::slopeAllowed(shape, static_cast<dsp::Slope>(s));
            items.push_back(en);
        }
        drawSlopeGrid(c, place(236.0f, slopeHeight(), true), items, "SLOPE \xC2\xB7 BAND " + std::to_string(band_ + 1));
    }

    // --- Processing menu (panels §2.5) ---
    void drawProcessing(visage::Canvas& c) {
        const auto& F = fonts();
        const float h = 30.0f + 32.0f + 2 * 52.0f + 3 * 4.0f + 8.0f + 42.0f + 40.0f + 40.0f + 9.0f + 32.0f + 14.0f;
        const Rect r = place(340.0f, h, true);
        panels_.push_back(r);
        theme::panel(c, r, 16.0f);
        float y = r.y + 8.0f;
        text(c, F.semi12, "PROCESSING", r.x + 18.0f, y + 8.0f, 200.0f, 14.0f, tok::inkMuted);
        y += 30.0f;
        const int mode = static_cast<int>(x_.gval(GlobalParam::Mode));
        const int res = static_cast<int>(x_.gval(GlobalParam::Resolution));
        static const char* sub[] = {"", "Analog-matched phase near Nyquist", "No phase shift, pre-ringing on steep cuts"};
        for (int m = 0; m < 3; ++m) {
            const float ih = m == 0 ? 32.0f : 52.0f;
            const Rect ir{r.x + 8.0f, y, r.w - 16.0f, ih};
            if (m == mode || hovered(kMode, m)) {
                c.setColor(tok::surface300);
                c.roundedRectangle(ir.x, ir.y, ir.w, ir.h, 8.0f);
            }
            const float cy = ir.y + ih * 0.5f;
            c.setColor(m == mode ? tok::accent : tok::inkMuted);
            c.ring(ir.x + 10.0f, cy - 7.0f, 14.0f, m == mode ? 4.0f : 1.5f);
            if (m == 0) text(c, F.label13, kModeNames[m], ir.x + 36.0f, ir.y, 200.0f, ih, tok::ink);
            else {
                text(c, F.label13, kModeNames[m], ir.x + 36.0f, ir.y + 8.0f, 220.0f, 16.0f, tok::ink);
                text(c, F.caption12, sub[m], ir.x + 36.0f, ir.y + 26.0f, 240.0f, 16.0f, tok::inkMuted);
            }
            text(c, F.label13, fmt::latency(x_.latency(m, res)), ir.x, ir.y, ir.w - 10.0f, ih, tok::inkMuted, visage::Font::kRight);
            addHit(ir, kMode, m);
            y += ih + 4.0f;
        }
        y += 8.0f;
        text(c, F.label13, "Resolution", r.x + 18.0f, y, 200.0f, 16.0f, tok::inkMuted);
        text(c, F.caption12, "Linear Phase, and spectral bands in every mode", r.x + 18.0f, y + 18.0f, 300.0f, 16.0f, tok::inkMuted);
        y += 42.0f;
        const Rect seg{r.x + 18.0f, y, r.w - 36.0f, 32.0f};
        segmented(c, seg, {"Low", "Med", "High", "V. High", "Max"}, res, segHover(seg, 5));
        for (int i = 0; i < 5; ++i) addHit(segPart(seg, 5, i), kResolution, i);
        y += 40.0f;
        std::string lat;
        for (int i = 0; i < 5; ++i) lat += (i ? " \xC2\xB7 " : "") + fmt::count(x_.latency(2, i) * 1LL);
        text(c, F.caption12, "Linear Phase latency:", r.x + 18.0f, y, r.w - 36.0f, 16.0f, tok::inkMuted);
        text(c, F.caption12, lat + " smp", r.x + 18.0f, y + 16.0f, r.w - 36.0f, 16.0f, tok::inkMuted);
        y += 40.0f;
        c.setColor(tok::lineHairline);
        c.fill(r.x + 8.0f, y, r.w - 16.0f, 1.0f);
        y += 9.0f;
        text(c, F.caption12, "While a spectral band is active, latency is at least", r.x + 18.0f, y, r.w - 36.0f, 16.0f, tok::inkMuted);
        text(c, F.caption12, "the FFT latency at this resolution.", r.x + 18.0f, y + 16.0f, r.w - 36.0f, 16.0f, tok::inkMuted);
    }
    static Rect segPart(const Rect& seg, int n, int i) {
        const float w = (seg.w - 4.0f - 2.0f * (n - 1)) / n;
        return {seg.x + 2.0f + i * (w + 2.0f), seg.y, w, seg.h};
    }
    [[nodiscard]] int segHover(const Rect& seg, int n) const {
        (void)n;
        if (hover_ < 0) return -1;
        const auto& h = hits_[static_cast<size_t>(hover_)];
        return seg.contains(h.r.x + 1.0f, h.r.y + 1.0f) ? h.arg : -1;
    }

    // Rows "label ... control" used by the Output, Analyzer and Interface panels.
    float row(visage::Canvas& c, const Rect& r, float y, const std::string& label, const std::string& sub) {
        const auto& F = fonts();
        if (sub.empty()) {
            text(c, F.label13, label, r.x + 16.0f, y, 200.0f, 28.0f, tok::ink);
            return 28.0f;
        }
        text(c, F.label13, label, r.x + 16.0f, y, 200.0f, 16.0f, tok::ink);
        text(c, F.caption12, sub, r.x + 16.0f, y + 18.0f, 220.0f, 16.0f, tok::inkMuted);
        return 34.0f;
    }
    void rowChip(visage::Canvas& c, const Rect& r, float y, float h, params::ParamIndex id) {
        const std::string t = x_.on(id) ? "On" : "Off";
        const float w = chipWidth("Off");
        const Rect cr{r.right() - 16.0f - w, y + (h - 28.0f) * 0.5f, w, 28.0f};
        chip(c, cr, t, {hovered(kToggle, static_cast<int>(id)), false, x_.on(id)});
        addHit(cr, kToggle, static_cast<int>(id), id);
    }
    void rowReadout(visage::Canvas& c, const Rect& r, float y, float h, params::ParamIndex id, const std::string& t, bool accentVariant) {
        const float w = readoutWidth(t);
        const Rect rr{r.right() - 16.0f - w, y + (h - 22.0f) * 0.5f, w, 22.0f};
        readout(c, rr, t, accentVariant, hovered(kDragValue, static_cast<int>(id)));
        addHit(rr, kDragValue, static_cast<int>(id), id);
    }
    void divider(visage::Canvas& c, const Rect& r, float& y) {
        c.setColor(tok::lineHairline);
        c.fill(r.x + 16.0f, y + 4.0f, r.w - 32.0f, 1.0f);
        y += 10.0f;
    }

    // --- Output menu (panels §2.7) ---
    void drawOutput(visage::Canvas& c) {
        const auto& F = fonts();
        const float h = 16.0f + 14.0f + 10.0f + 28.0f + 10.0f + 28.0f + 10.0f + 28.0f + 10.0f + 28.0f + 10.0f + 34.0f + 10.0f + 10.0f +
                        34.0f + 10.0f + 16.0f + 10.0f + 20.0f + 10.0f + 16.0f + 10.0f + 34.0f + 16.0f;
        const Rect r = place(320.0f, h, true, true);
        panels_.push_back(r);
        theme::panel(c, r, 16.0f);
        float y = r.y + 16.0f;
        text(c, F.semi12, "OUTPUT", r.x + 16.0f, y, 200.0f, 14.0f, tok::inkMuted);
        y += 24.0f;
        auto gid = [](GlobalParam g) { return params::globalParam(g); };
        row(c, r, y, "Pan Mode", "");
        const Rect seg{r.right() - 16.0f - 140.0f, y, 140.0f, 28.0f};
        segmented(c, seg, {"L/R", "M/S"}, static_cast<int>(x_.gval(GlobalParam::PanMode)), segHover(seg, 2));
        for (int i = 0; i < 2; ++i) addHit(segPart(seg, 2, i), kSegParam, i, gid(GlobalParam::PanMode));
        y += 38.0f;
        row(c, r, y, "Output Level", "");
        rowReadout(c, r, y, 28.0f, gid(GlobalParam::OutputLevel),
                   fmt::outputDb(x_.gval(GlobalParam::OutputLevel), params::kOutputSilenceDb), true);
        y += 38.0f;
        row(c, r, y, "Output Pan", "");
        rowReadout(c, r, y, 28.0f, gid(GlobalParam::OutputPan), fmt::pan(x_.gval(GlobalParam::OutputPan), x_.gval(GlobalParam::PanMode) >= 0.5), false);
        y += 38.0f;
        row(c, r, y, "Invert polarity", "");
        rowChip(c, r, y, 28.0f, gid(GlobalParam::Invert));
        y += 38.0f;
        y += row(c, r, y, "Bypass", "Smoothed, latency-compensated") - 34.0f;
        rowChip(c, r, y, 34.0f, gid(GlobalParam::Bypass));
        y += 44.0f;
        divider(c, r, y);
        row(c, r, y, "Auto Gain", "Keeps loudness level while you EQ");
        rowChip(c, r, y, 34.0f, gid(GlobalParam::AutoGain));
        y += 44.0f;
        text(c, F.label13, "Gain Scale", r.x + 16.0f, y, 200.0f, 16.0f, tok::ink);
        text(c, F.semi13, fmt::pct(x_.gval(GlobalParam::GainScale)), r.x, y, r.w - 16.0f, 16.0f, tok::accent, visage::Font::kRight);
        y += 26.0f;
        const auto gs = gid(GlobalParam::GainScale);
        slider(c, {r.x + 16.0f, y, r.w - 32.0f, 20.0f}, params::toNormalised(*params::specAt(gs), x_.val(gs)), 0.5, gs);
        y += 30.0f;
        text(c, F.caption12, "Scales every band's gain and dynamic range together.", r.x + 16.0f, y, r.w - 32.0f, 16.0f, tok::inkMuted);
        y += 26.0f;
        row(c, r, y, "Solo Gain", "Audition level while a band is soloed");
        rowReadout(c, r, y, 34.0f, gid(GlobalParam::SoloGain), fmt::db(x_.gval(GlobalParam::SoloGain)), false);
    }
    // A range slider (panels §2.9): drawn track, accent fill from `from`, an ink thumb.
    void slider(visage::Canvas& c, Rect r, double n, double from, params::ParamIndex id) {
        const float cy = r.y + r.h * 0.5f;
        c.setColor(tok::surface000);
        c.roundedRectangle(r.x, cy - 2.0f, r.w, 4.0f, 2.0f);
        const float xn = r.x + static_cast<float>(n) * r.w, xf = r.x + static_cast<float>(from) * r.w;
        c.setColor(tok::accent);
        c.roundedRectangle(std::min(xn, xf), cy - 2.0f, std::abs(xn - xf), 4.0f, 2.0f);
        c.setColor(0x99000000);
        c.circle(xn - 8.0f, cy - 6.0f, 16.0f);
        c.setColor(tok::ink);
        c.circle(xn - 7.0f, cy - 7.0f, 14.0f);
        addHit(r, kDragValue, static_cast<int>(id), id);
    }

    // --- Analyzer and Interface panels (panels §2.6) ---
    void drawAnalyzer(visage::Canvas& c) {
        const auto& F = fonts();
        const float h = 16.0f + 14.0f + 10.0f + 4 * 38.0f + 6.0f + 10.0f + 16.0f + 8.0f + 32.0f + 6.0f + 16.0f + 10.0f + 16.0f + 8.0f + 32.0f +
                        10.0f + 2 * 38.0f + 8.0f;
        Rect r = pop_ == Pop::Settings ? Rect{160.0f, height() - 44.0f - 8.0f - h, 360.0f, h} : place(360.0f, h, true);
        if (pop_ == Pop::Settings) r.y = std::max(16.0f, r.y);
        panels_.push_back(r);
        theme::panel(c, r, 16.0f);
        auto gid = [](GlobalParam g) { return params::globalParam(g); };
        float y = r.y + 16.0f;
        text(c, F.semi12, "ANALYZER", r.x + 16.0f, y, 200.0f, 14.0f, tok::inkMuted);
        y += 24.0f;
        row(c, r, y, "Show", "");
        {
            float xr = r.right() - 16.0f;
            const std::pair<GlobalParam, const char*> show[] = {{GlobalParam::AnalyzerShowExternal, "Side chain"},
                                                                {GlobalParam::AnalyzerShowPost, "Post"},
                                                                {GlobalParam::AnalyzerShowPre, "Pre"}};
            for (const auto& [g, n] : show) {
                const float w = chipWidth(n);
                const Rect cr{xr - w, y, w, 28.0f};
                chip(c, cr, n, {hovered(kToggle, static_cast<int>(gid(g))), false, x_.gval(g) >= 0.5});
                addHit(cr, kToggle, static_cast<int>(gid(g)), gid(g));
                xr -= w + 4.0f;
            }
        }
        y += 38.0f;
        row(c, r, y, "Freeze", "");
        rowChip(c, r, y, 28.0f, gid(GlobalParam::AnalyzerFreeze));
        y += 38.0f;
        row(c, r, y, "Peak hold", "");
        rowChip(c, r, y, 28.0f, gid(GlobalParam::AnalyzerPeakHold));
        y += 38.0f;
        row(c, r, y - 3.0f, "Collisions", "Where input and side chain overlap");
        rowChip(c, r, y - 3.0f, 34.0f, gid(GlobalParam::AnalyzerCollisions));
        y += 38.0f;
        divider(c, r, y);
        text(c, F.label13, "Resolution (FFT size)", r.x + 16.0f, y, 300.0f, 16.0f, tok::inkMuted);
        y += 24.0f;
        const auto ares = gid(GlobalParam::AnalyzerResolution);
        Rect seg{r.x + 16.0f, y, r.w - 32.0f, 32.0f};
        const int resI = static_cast<int>(x_.val(ares));
        segmented(c, seg, {"Low", "Medium", "High", "Max"}, resI, segHover(seg, 4));
        for (int i = 0; i < 4; ++i) addHit(segPart(seg, 4, i), kSegParam, i, ares);
        y += 38.0f;
        const int fft = (2048 << std::clamp(resI, 0, 3)) * dsp::rateFactor(x_.sampleRate());
        char buf[64];
        std::snprintf(buf, sizeof buf, "%d-point FFT at %g kHz", fft, x_.sampleRate() / 1000.0);
        text(c, F.caption12, buf, r.x + 16.0f, y, r.w - 32.0f, 16.0f, tok::inkMuted);
        y += 26.0f;
        text(c, F.label13, "Speed", r.x + 16.0f, y, 200.0f, 16.0f, tok::inkMuted);
        y += 24.0f;
        const auto spd = gid(GlobalParam::AnalyzerSpeed);
        seg = {r.x + 16.0f, y, r.w - 32.0f, 32.0f};
        segmented(c, seg, {"V. Slow", "Slow", "Medium", "Fast", "V. Fast"}, static_cast<int>(x_.val(spd)), segHover(seg, 5));
        for (int i = 0; i < 5; ++i) addHit(segPart(seg, 5, i), kSegParam, i, spd);
        y += 42.0f;
        row(c, r, y, "Tilt", "");
        {
            const auto tid = gid(GlobalParam::AnalyzerTilt);
            const Rect pr{r.right() - 16.0f - 28.0f, y, 28.0f, 28.0f};
            iconButton(c, pr, icons::chevDown, {hovered(kTiltPresets)});
            addHit(pr, kTiltPresets);
            const std::string t = fmt::dbPerOct(x_.val(tid));
            const float w = readoutWidth(t);
            const Rect rr{pr.x - 4.0f - w, y + 3.0f, w, 22.0f};
            readout(c, rr, t, false, hovered(kDragValue, static_cast<int>(tid)), 0, hovered(kDragValue, static_cast<int>(tid)));
            addHit(rr, kDragValue, static_cast<int>(tid), tid);
        }
        y += 38.0f;
        row(c, r, y, "Range", "");
        {
            const auto rid = gid(GlobalParam::AnalyzerRange);
            const std::string t = params::specAt(rid)->choices[static_cast<size_t>(std::clamp(static_cast<int>(x_.val(rid)), 0, 2))];
            const float w = selectWidth("", t, "", 1);
            const Rect sr{r.right() - 16.0f - w, y, w, 28.0f};
            select(c, sr, "", t, "", 1, {hovered(kSelectMenu, 0)});
            addHit(sr, kSelectMenu, 0, rid);
        }
    }
    void drawInterface(visage::Canvas& c) {
        const auto& F = fonts();
        const float h = 16.0f + 14.0f + 10.0f + 16.0f + 8.0f + 32.0f + 8.0f + 16.0f + 10.0f + 10.0f + 2 * 38.0f + 8.0f;
        const Rect r{width() - 24.0f - 300.0f, std::max(16.0f, height() - 44.0f - 8.0f - h), 300.0f, h};
        panels_.push_back(r);
        theme::panel(c, r, 16.0f);
        float y = r.y + 16.0f;
        text(c, F.semi12, "INTERFACE", r.x + 16.0f, y, 200.0f, 14.0f, tok::inkMuted);
        y += 24.0f;
        text(c, F.label13, "Scale", r.x + 16.0f, y, 200.0f, 16.0f, tok::inkMuted);
        y += 24.0f;
        // Interface zoom (P-7) is not built yet: the segments show 100 % and are inactive.
        segmented(c, {r.x + 16.0f, y, r.w - 32.0f, 32.0f}, {"100 %", "150 %", "200 %", "300 %"}, 0, -1, false);
        y += 40.0f;
        text(c, F.caption12, "Drag the window corner for any size in between.", r.x + 16.0f, y, r.w - 32.0f, 16.0f, tok::inkMuted);
        y += 20.0f;
        divider(c, r, y);
        row(c, r, y, "Display range", "");
        {
            const auto rid = params::globalParam(GlobalParam::DisplayRange);
            const std::string t = "\xC2\xB1" + params::specAt(rid)->choices[static_cast<size_t>(std::clamp(static_cast<int>(x_.val(rid)), 0, 3))];
            const float w = selectWidth("", t, "", 1);
            const Rect sr{r.right() - 16.0f - w, y, w, 28.0f};
            select(c, sr, "", t, "", 1, {hovered(kSelectMenu, 1)});
            addHit(sr, kSelectMenu, 1, rid);
        }
        y += 38.0f;
        row(c, r, y, "Gain-Q coupling", "");
        {
            const float w = chipWidth("Off");
            const Rect cr{r.right() - 16.0f - w, y, w, 28.0f};
            chip(c, cr, x_.interaction.gainQCoupling ? "On" : "Off", {hovered(kToggle, -1), false, x_.interaction.gainQCoupling});
            addHit(cr, kToggle, -1, static_cast<params::ParamIndex>(-1));
        }
    }

    // --- Side-chain filter popover (P-11) ---
    void drawScFilter(visage::Canvas& c) {
        if (band_ < 0) return;
        const auto& F = fonts();
        const bool free = x_.bval(band_, BandField::ScMode) >= 0.5;
        const float h = 12.0f + 32.0f + 8.0f + (free ? 2 * 30.0f : 16.0f) + 12.0f;
        const Rect r = place(220.0f, h, true);
        panels_.push_back(r);
        theme::panel(c, r, 16.0f);
        const auto mid = params::bandParam(band_, BandField::ScMode);
        const Rect seg{r.x + 12.0f, r.y + 12.0f, r.w - 24.0f, 32.0f};
        segmented(c, seg, {"Band", "Free"}, free ? 1 : 0, segHover(seg, 2));
        for (int i = 0; i < 2; ++i) addHit(segPart(seg, 2, i), kSegParam, i, mid);
        float y = r.y + 52.0f;
        if (!free) {
            text(c, F.caption12, "Follows the band.", r.x + 12.0f, y, r.w - 24.0f, 16.0f, tok::inkMuted);
            return;
        }
        for (auto f : {BandField::ScLow, BandField::ScHigh}) {
            const auto id = params::bandParam(band_, f);
            text(c, F.label13, f == BandField::ScLow ? "Low" : "High", r.x + 12.0f, y, 80.0f, 26.0f, tok::ink);
            const std::string t = fmt::hz(std::exp2(x_.val(id)));
            const float w = readoutWidth(t);
            const Rect rr{r.right() - 12.0f - w, y + 2.0f, w, 22.0f};
            readout(c, rr, t, false, hovered(kDragValue, static_cast<int>(id)), 0, hovered(kDragValue, static_cast<int>(id)));
            addHit(rr, kDragValue, static_cast<int>(id), id);
            y += 30.0f;
        }
    }

    // --- Inline value editor (P-26) ---
    void drawInline(visage::Canvas& c) {
        const auto& F = fonts();
        float w = 16.0f;
        for (const auto& f : fields_) w += std::max(64.0f, textWidth(F.semi13, f.text) + 16.0f) + 8.0f;
        Rect r{anchor_.x, anchor_.y, w, 30.0f};
        r.x = std::clamp(r.x, 8.0f, std::max(8.0f, width() - 8.0f - r.w));
        r.y = std::clamp(r.y, 8.0f, std::max(8.0f, height() - 8.0f - r.h));
        panels_.push_back(r);
        c.setColor(tok::lineHairline);
        c.roundedRectangle(r.x - 1.0f, r.y - 1.0f, r.w + 2.0f, r.h + 2.0f, 5.0f);
        c.setColor(tok::surface200);
        c.roundedRectangle(r.x, r.y, r.w, r.h, 4.0f);
        float x = r.x + 8.0f;
        for (size_t i = 0; i < fields_.size(); ++i) {
            const auto& f = fields_[i];
            const float fw = std::max(64.0f, textWidth(F.semi13, f.text) + 16.0f);
            const Rect fr{x, r.y + 4.0f, fw, 22.0f};
            if (static_cast<int>(i) == field_ || f.invalid) {
                c.setColor(f.invalid ? tok::signalClip : tok::accent);
                c.roundedRectangle(fr.x - 2.0f, fr.y - 2.0f, fr.w + 4.0f, fr.h + 4.0f, 4.0f);
            }
            c.setColor(tok::surface000);
            c.roundedRectangle(fr.x, fr.y, fr.w, fr.h, 2.0f);
            text(c, F.semi13, f.text, fr.x + 8.0f, fr.y, fr.w - 12.0f, fr.h, tok::ink);
            if (static_cast<int>(i) == field_ && !fresh_) {
                c.setColor(tok::ink);
                c.fill(fr.x + 8.0f + textWidth(F.semi13, f.text) + 1.0f, fr.y + 4.0f, 1.0f, 14.0f);
            }
            addHit(fr, kInlineField, static_cast<int>(i));
            x += fw + 8.0f;
        }
    }
    bool commitField(int i) {
        auto& f = fields_[static_cast<size_t>(i)];
        std::string t = f.text;
        for (size_t p; (p = t.find(fmt::kMinus)) != std::string::npos;) t.replace(p, 3, "-");
        const auto v = params::parsePlain(*params::specAt(f.id), t);
        if (!v) {
            f.invalid = true;
            redraw();
            return false;
        }
        if (std::abs(*v - x_.val(f.id)) > 1e-12) x_.set(f.id, *v);
        return true;
    }
    bool commitInline() {
        bool ok = true;
        for (size_t i = 0; i < fields_.size(); ++i) ok = commitField(static_cast<int>(i)) && ok;
        return ok;
    }

    Ctx& x_;
    Pop pop_ = Pop::None;
    bool closing_ = false;
    float alpha_ = 1.0f;
    std::vector<Hit> hits_;
    std::vector<Rect> panels_;
    int hover_ = -1;
    Rect anchor_;
    bool atPoint_ = false;
    int band_ = -1;
    // Menu
    std::string caption_;
    float width_ = 196.0f;
    std::vector<Entry> items_;
    std::function<void(int)> pick_;
    int sub_ = -1;
    Rect subAnchor_;
    // Inline editor
    std::vector<Field> fields_;
    int field_ = 0;
    bool fresh_ = true;  // the first key replaces the text
};

// ---------------------------------------------------------------------------
// Graph layers
// ---------------------------------------------------------------------------

// Grid, 0 dB line and axis labels (editor-spec-graph §5.2). Cached; redrawn on range or span changes.
class GroundFrame : public visage::Frame {
public:
    explicit GroundFrame(Ctx& x) : x_(x) {
        setCached(true);
        setIgnoresMouseEvents(true, false);
    }
    void draw(visage::Canvas& c) override {
        const auto& g = x_.geometry;
        const auto& F = fonts();
        c.setColor(tok::surface100);
        c.fill(0, 0, width(), height());
        const float top = GraphGeometry::kTop, bottom = height() - GraphGeometry::kBottom;
        const float right = width() - GraphGeometry::kRight;
        // Frequency lines; labels thinned when crowded; minor lines when a decade is >= 600 px.
        const double majors[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
        const float decade = g.xOfHz(1000.0) - g.xOfHz(100.0);
        if (decade >= 600.0f) {
            c.setColor(withAlpha(tok::lineGrid, 0.5f));
            for (double d = 10.0; d <= 10000.0; d *= 10.0)
                for (int m : {3, 4, 6, 7, 8, 9}) {
                    const double f = m * d;
                    if (f > g.viewLo() && f < g.viewHi()) c.fill(std::round(g.xOfHz(f)), top, 1.0f, bottom - top);
                }
        }
        const bool thin1 = g.xOfHz(100.0) - g.xOfHz(50.0) < 28.0f;
        const bool thin2 = g.xOfHz(200.0) - g.xOfHz(100.0) < 40.0f;
        for (double f : majors) {
            if (f < g.viewLo() || f > g.viewHi()) continue;
            const float xf = std::round(g.xOfHz(f));
            c.setColor(tok::lineGrid);
            c.fill(xf, top, 1.0f, bottom - top);
            const int lead = static_cast<int>(f / std::pow(10.0, std::floor(std::log10(f))) + 0.5);
            if ((thin1 && lead == 5) || (thin2 && lead == 2)) continue;
            text(c, F.scale12, fmt::hzAxis(f), xf - 24.0f, bottom + 4.0f, 48.0f, 16.0f, tok::inkAxis, visage::Font::kCenter);
        }
        // Gain lines every R/6 (every other when crowded), labels on every other line.
        const double R = g.rangeDb(), step = R / 6.0;
        const float px = static_cast<float>(g.yOfDb(0.0) - g.yOfDb(step));
        const int every = px < 12.0f ? 2 : 1;
        for (int k = -6; k <= 6; ++k) {
            if (k == 0 || k % every) continue;
            c.setColor(tok::lineGrid);
            c.fill(0.0f, std::round(g.yOfDb(k * step)), right, 1.0f);
        }
        c.setColor(tok::lineZero);
        c.fill(0.0f, std::round(g.yOfDb(0.0)), right, 1.0f);
        const bool sparse = 2.0f * px < 20.0f;
        if (!x_.matchMode || true) {
            for (int k = -6; k <= 6; k += 2) {
                if (sparse && k != 0 && std::abs(k) != 6) continue;
                text(c, F.scale12, fmt::dbAxis(k * step, true), right, g.yOfDb(k * step) - 8.0f, 30.0f, 16.0f, tok::accent,
                     visage::Font::kRight);
            }
        }
        // Analyzer scale (dBFS), every 10 dB (20 when crowded); not in EQ Match mode (board).
        if (!x_.matchMode) {
            const double ar = x_.analyzerRange();
            const float per10 = (bottom - top) * 10.0f / static_cast<float>(ar);
            const int stepDb = per10 < 20.0f ? 20 : 10;
            for (int d = stepDb; d < static_cast<int>(ar); d += stepDb) {
                const float y = top + (bottom - top) * static_cast<float>(d / ar);
                text(c, F.scale12, fmt::dbAxis(-d, false), right + 30.0f, y - 8.0f, 36.0f, 16.0f, tok::inkAxis, visage::Font::kRight);
            }
        }
    }

private:
    Ctx& x_;
};

// Analyzer layers (§5.3 rows 2-6).
class SpectrumFrame : public visage::Frame {
public:
    explicit SpectrumFrame(Ctx& x) : x_(x) { setIgnoresMouseEvents(true, false); }
    void draw(visage::Canvas& c) override {
        const auto* s = x_.poller.spectrum();
        if (!s) return;
        const auto& g = x_.geometry;
        const double range = x_.analyzerRange();
        const auto& grid = dsp::logGrid();
        const float top = GraphGeometry::kTop, bottom = height() - GraphGeometry::kBottom, right = width() - GraphGeometry::kRight;
        c.trimClampBounds(0.0f, top, right, bottom - top);
        const uint32_t n = std::min<uint32_t>(s->binCount, static_cast<uint32_t>(grid.size()));
        auto yOf = [&](float db) { return std::clamp(top + static_cast<float>(-db / range) * (bottom - top), top, bottom); };
        auto line = [&](const std::array<float, analysis::kMaxSpectrumBins>& db, bool close) {
            Poly p;
            bool started = false;
            float lastX = 0.0f;
            for (uint32_t i = 0; i < n; ++i) {
                if (grid[i] < g.viewLo() || grid[i] > g.viewHi()) continue;
                const float xx = g.xOfHz(grid[i]), yy = yOf(db[i]);
                if (!started) {
                    if (close) p.moveTo(xx, bottom), p.lineTo(xx, yy);
                    else p.moveTo(xx, yy);
                    started = true;
                } else p.lineTo(xx, yy);
                lastX = xx;
            }
            if (started && close) p.lineTo(lastX, bottom), p.close();
            return p;
        };
        const bool pre = x_.gval(GlobalParam::AnalyzerShowPre) >= 0.5, post = x_.gval(GlobalParam::AnalyzerShowPost) >= 0.5,
                   ext = x_.gval(GlobalParam::AnalyzerShowExternal) >= 0.5;
        if (x_.matchMode) {  // EQ Match: input and reference spectra (§5.7)
            c.setColor(tok::analyzerPost);
            strokePoly(c, line(s->pre, false), 1.5f);
            c.setColor(tok::analyzerSc);
            strokePoly(c, line(s->external, false), 1.5f);
            return;
        }
        if (pre) {
            c.setColor(tok::analyzerPre);
            fillPoly(c, line(s->pre, true));
        }
        if (x_.gval(GlobalParam::AnalyzerCollisions) >= 0.5 && ext) drawCollisions(c, *s, n, yOf, bottom);
        if (post && x_.gval(GlobalParam::AnalyzerPeakHold) >= 0.5) {
            c.setColor(tok::inkMuted);
            strokePoly(c, line(s->postPeak, false), 1.0f, {1.0f, 3.0f});
        }
        if (post) {
            c.setColor(tok::analyzerPost);
            strokePoly(c, line(s->post, false), 1.0f);
        }
        if (ext) {
            c.setColor(tok::analyzerSc);
            strokePoly(c, line(s->external, false), 1.0f, {2.0f, 2.0f});
        }
    }

private:
    // Hatch under the lower of the pre and side-chain spectra where both are above -60 dBFS
    // (PROVISIONAL activity floor): 2-wide stripes every 6 px at 45°.
    template <typename F>
    void drawCollisions(visage::Canvas& c, const analysis::SpectrumSnapshot& s, uint32_t n, F yOf, float bottom) {
        const auto& g = x_.geometry;
        const auto& grid = dsp::logGrid();
        const float W = width() - GraphGeometry::kRight;
        std::vector<float> topY(static_cast<size_t>(W) + 1, bottom);
        for (uint32_t i = 0; i + 1 < n; ++i) {
            if (s.pre[i] < -60.0f || s.external[i] < -60.0f) continue;
            const float x0 = g.xOfHz(grid[i]), x1 = g.xOfHz(grid[i + 1]);
            const float y = yOf(std::min(s.pre[i], s.external[i]));
            for (int xx = std::max(0, static_cast<int>(x0)); xx <= std::min(static_cast<int>(W), static_cast<int>(x1)); ++xx)
                topY[static_cast<size_t>(xx)] = std::min(topY[static_cast<size_t>(xx)], y);
        }
        c.setColor(withAlpha(tok::signalClip, 0.55f));
        for (int xx = 0; xx <= static_cast<int>(W); xx += 1) {
            const float ty = topY[static_cast<size_t>(xx)];
            if (ty >= bottom) continue;
            // Pixels on the stripe lattice (x + y) mod 6 < 2, drawn as short vertical runs.
            for (float y = bottom - 1.0f; y >= ty; y -= 6.0f) {
                const int phase = (xx + static_cast<int>(y)) % 6;
                const float y0 = y - static_cast<float>(phase);
                c.fill(static_cast<float>(xx), std::max(ty, y0 - 1.0f), 1.0f, 2.0f);
            }
        }
    }
    Ctx& x_;
};

// Band fills and strokes, live dynamic curves, the summed curve (§5.3 rows 8-11), or the EQ Match
// layers (§5.7) in EQ Match mode.
class CurvesFrame : public visage::Frame {
public:
    explicit CurvesFrame(Ctx& x) : x_(x) { setIgnoresMouseEvents(true, false); }
    void draw(visage::Canvas& c) override {
        const auto& g = x_.geometry;
        const float top = GraphGeometry::kTop, bottom = height() - GraphGeometry::kBottom, right = width() - GraphGeometry::kRight;
        c.trimClampBounds(0.0f, top, right, bottom - top);
        if (x_.matchMode) {
            drawMatch(c);
            return;
        }
        x_.curve.update(x_.table());
        const auto& hz = x_.curve.frequencies();
        if (hz.empty()) return;
        const float y0 = g.yOfDb(0.0);
        const bool dragging = x_.interaction.dragging();
        auto yClamp = [&](double db) { return std::clamp(g.yOfDb(db), top - 4.0f, bottom + 4.0f); };
        for (int b = 0; b < params::kBandCount; ++b) {
            if (x_.bval(b, BandField::Used) < 0.5) continue;
            const auto& db = x_.curve.bandDb(b);
            if (db.size() != hz.size()) continue;
            const bool enabled = x_.bval(b, BandField::Enabled) >= 0.5;
            const bool sel = x_.interaction.isSelected(b);
            float fillA = sel ? tok::opacityBandSelected : (dragging ? tok::opacityBandIdle : tok::opacityBandFill);
            float strokeA = 1.0f;
            if (!enabled) fillA *= tok::opacityBypassed, strokeA = tok::opacityBypassed;
            if (soloBand_ >= 0 && soloBand_ != b) fillA = tok::opacityBandIdle, strokeA = 0.5f;
            target_[static_cast<size_t>(b)] = fillA;
            if (fill_[static_cast<size_t>(b)] < 0.0f) fill_[static_cast<size_t>(b)] = fillA;
            fillA = fill_[static_cast<size_t>(b)];
            Poly fill, stroke;
            fill.moveTo(g.xOfHz(hz.front()), y0);
            for (size_t i = 0; i < hz.size(); ++i) {
                const float xx = g.xOfHz(hz[i]), yy = yClamp(db[i]);
                fill.lineTo(xx, yy);
                if (i == 0) stroke.moveTo(xx, yy);
                else stroke.lineTo(xx, yy);
            }
            fill.lineTo(g.xOfHz(hz.back()), y0);
            fill.close();
            const uint32_t col = bandColour(b);
            c.setColor(withAlpha(col, fillA));
            fillPoly(c, fill);
            c.setColor(withAlpha(col, strokeA));
            strokePoly(c, stroke, 1.5f);
        }
        drawLiveCurves(c);
        // Summed curve: enabled bands only (C-20); ResponseCurve's combination follows the DSP.
        const auto& sum = x_.curve.combinedDb();
        Poly p;
        for (size_t i = 0; i < hz.size(); ++i) {
            const float xx = g.xOfHz(hz[i]), yy = yClamp(sum[i]);
            if (i == 0) p.moveTo(xx, yy);
            else p.lineTo(xx, yy);
        }
        if (x_.usedCount() > 0) {
            c.setColor(tok::accent);
            strokePoly(c, p, 2.0f);
        }
    }
    int soloBand_ = -1;
    // Fill opacities move towards their targets over 100 ms (spec §7).
    bool animate(double dtMs) {
        bool moving = false;
        for (size_t b = 0; b < fill_.size(); ++b) {
            if (fill_[b] < 0.0f) continue;
            const float step = static_cast<float>(0.4 * dtMs / 100.0);
            if (std::abs(fill_[b] - target_[b]) <= step) fill_[b] = target_[b];
            else fill_[b] += fill_[b] < target_[b] ? step : -step, moving = true;
        }
        if (moving) redraw();
        return moving;
    }

private:
    std::array<float, params::kBandCount> fill_ = [] {
        std::array<float, params::kBandCount> a{};
        a.fill(-1.0f);
        return a;
    }();
    std::array<float, params::kBandCount> target_{};
    void drawLiveCurves(visage::Canvas& c) {
        const auto& g = x_.geometry;
        const auto& gr = x_.poller.gainChange();
        const auto& hz = x_.curve.frequencies();
        const double scale = x_.gval(GlobalParam::GainScale) / 100.0;
        for (int b = 0; b < params::kBandCount; ++b) {
            const float ch = gr.currentDb[static_cast<size_t>(b)];
            if (std::abs(ch) < 0.05f || x_.bval(b, BandField::Used) < 0.5 || x_.bval(b, BandField::DynEnabled) < 0.5) continue;
            const auto shape = static_cast<dsp::Shape>(static_cast<int>(x_.bval(b, BandField::Shape)));
            if (!dsp::shapeHasGain(shape)) continue;
            const double gain = std::clamp(x_.bval(b, BandField::Gain) * scale + ch, -30.0, 30.0);
            const auto proto = dsp::designAnalogPrototype({shape, std::exp2(x_.bval(b, BandField::Freq)), gain,
                                                           params::qFromPosition(x_.bval(b, BandField::Q)),
                                                           static_cast<dsp::Slope>(static_cast<int>(x_.bval(b, BandField::Slope)))});
            if (!proto.supported) continue;
            Poly p;
            for (size_t i = 0; i < hz.size(); i += 2) {
                const double db = 20.0 * std::log10(std::max(std::abs(proto.evaluateHz(hz[i])), 1e-9));
                const float xx = g.xOfHz(hz[i]), yy = g.yOfDb(db);
                if (i == 0) p.moveTo(xx, yy);
                else p.lineTo(xx, yy);
            }
            c.setColor(withAlpha(bandColour(b), 0.8f));
            strokePoly(c, p, 1.0f, {3.0f, 2.0f});
        }
    }
    void drawMatch(visage::Canvas& c) {
        auto* mu = x_.match.get();
        if (!mu || !mu->st.curveValid) return;
        const auto& g = x_.geometry;
        const auto& grid = analysis::matchGrid();
        std::array<double, analysis::kMatchGridPoints> fitted{};
        const auto& fit = mu->applier.fit();
        for (int i = 0; i < std::min<int>(mu->count, static_cast<int>(fit.size())); ++i) {
            const auto b = EqMatchApplier::scaled(fit[static_cast<size_t>(i)], mu->amount);
            const auto p = dsp::designAnalogPrototype({b.shape, b.hz, b.gainDb, b.q, b.slope});
            if (!p.supported) continue;
            for (size_t k = 0; k < grid.size(); ++k) fitted[k] += 20.0 * std::log10(std::max(std::abs(p.evaluateHz(grid[k])), 1e-9));
        }
        auto path = [&](auto value, size_t from, size_t to) {
            Poly p;
            bool started = false;
            for (size_t k = from; k < to; ++k) {
                if (grid[k] < g.viewLo() || grid[k] > g.viewHi()) continue;
                const float xx = g.xOfHz(grid[k]), yy = g.yOfDb(value(k));
                if (!started) p.moveTo(xx, yy), started = true;
                else p.lineTo(xx, yy);
            }
            return p;
        };
        // Difference: dash 4 3 in ink; low-confidence runs at 0.35 (P-18).
        size_t k = 0;
        while (k + 1 < grid.size()) {
            const bool low = mu->st.weight[k] < 0.5;
            size_t e = k + 1;
            while (e < grid.size() && (mu->st.weight[e] < 0.5) == low) ++e;
            c.setColor(withAlpha(tok::ink, low ? 0.35f : 1.0f));
            strokePoly(c, path([&](size_t i) { return mu->st.target[i]; }, k, std::min(e + 1, grid.size())), 1.0f, {4.0f, 3.0f});
            k = e;
        }
        if (!fit.empty()) {
            c.setColor(tok::inkMuted);
            strokePoly(c, path([&](size_t i) { return mu->st.target[i] - fitted[i]; }, 0, grid.size()), 1.0f, {1.0f, 2.0f});
            c.setColor(tok::accent);
            strokePoly(c, path([&](size_t i) { return fitted[i]; }, 0, grid.size()), 2.0f);
        }
    }
    Ctx& x_;
};

// Output meters in the right gutter (§5.5).
class MeterFrame : public visage::Frame {
public:
    explicit MeterFrame(Ctx& x) : x_(x) { setIgnoresMouseEvents(true, false); }
    void draw(visage::Canvas& c) override {
        if (x_.matchMode) return;
        const auto& lv = x_.poller.levels();
        const float top = GraphGeometry::kTop, bottom = height() - GraphGeometry::kBottom;
        auto yOf = [&](float db) { return top + (bottom - top) * std::clamp(-db / 60.0f, 0.0f, 1.0f); };
        for (int ch = 0; ch < 2; ++ch) {
            const float mx = 78.0f + 8.0f * static_cast<float>(ch);
            c.setColor(tok::surface000);
            c.roundedRectangle(mx, top, 6.0f, bottom - top, 2.0f);
            const auto& l = lv.output[static_cast<size_t>(ch)];
            const float yp = yOf(l.peakDb);
            // Segments: ok below -18, hot -18..-3, clip above -3 dBFS.
            const float y18 = yOf(-18.0f), y3 = yOf(-3.0f);
            if (yp < bottom) {
                c.setColor(tok::meterOk);
                c.fill(mx, std::max(yp, y18), 6.0f, bottom - std::max(yp, y18));
                if (yp < y18) {
                    c.setColor(tok::meterHot);
                    c.fill(mx, std::max(yp, y3), 6.0f, y18 - std::max(yp, y3));
                }
                if (yp < y3) {
                    c.setColor(tok::signalClip);
                    c.fill(mx, yp, 6.0f, y3 - yp);
                }
            }
            if (l.holdDb > -60.0f) {
                c.setColor(tok::meterHot);
                c.fill(mx, yOf(l.holdDb) - 1.0f, 6.0f, 2.0f);
            }
            if (l.rmsDb > -60.0f) {
                c.setColor(tok::inkMuted);
                c.fill(mx, yOf(l.rmsDb), 6.0f, 1.0f);
            }
            c.setColor(l.peakDb > 0.0f || clip_[static_cast<size_t>(ch)] ? tok::signalClip : tok::surface000);
            clip_[static_cast<size_t>(ch)] = clip_[static_cast<size_t>(ch)] || l.peakDb > 0.0f;
            c.roundedRectangle(mx, top - 8.0f, 6.0f, 4.0f, 1.0f);
        }
    }

private:
    Ctx& x_;
    std::array<bool, 2> clip_{};
};

// ---------------------------------------------------------------------------
// GraphTop: handles, readouts, the band panel, band list and legends; graph input
// ---------------------------------------------------------------------------

class GraphTop : public visage::Frame {
public:
    GraphTop(Ctx& x, Overlay& o) : x_(x), o_(o) { setAcceptsKeystrokes(true); }
    std::function<void()> onViewChanged;

    void draw(visage::Canvas& c) override {
        hits_.clear();
        drawHandles(c);
        if (!x_.matchMode) {
            drawReadouts(c);
            if (x_.interaction.lassoActive()) {
                const auto r = x_.interaction.lassoRect();
                c.setColor(withAlpha(tok::ink, 0.04f));
                c.fill(r[0], r[1], r[2] - r[0], r[3] - r[1]);
                c.setColor(tok::ink);
                Poly p;
                p.addRectangle(r[0], r[1], r[2] - r[0], r[3] - r[1]);
                strokePoly(c, p, 1.0f, {4.0f, 3.0f});
            }
            drawPanel(c);
            if (x_.bandListOpen) drawBandList(c);
            if (x_.settingsOpen) drawAnalyzerLegend(c);
        } else {
            drawMatchLegend(c);
            drawMatchPanel(c);
        }
    }

    // --- input ---
    void mouseDown(const visage::MouseEvent& e) override {
        requestKeyboardFocus();
        x_.clearTip();
        const float px = e.position.x, py = e.position.y;
        const int h = hitAt(hits_, px, py);
        if (h >= 0) {
            onHit(hits_[static_cast<size_t>(h)], e);
            return;
        }
        if (x_.matchMode) return;
        listFocus_ = false;
        if (e.isRightButton()) {
            showBandMenu(px, py);
            return;
        }
        if (!plotRect().contains(px, py)) return;
        const int node = x_.interaction.hitTest(px, py);
        if (e.repeatClickCount() == 2 && node >= 0 && !e.isAltDown()) {  // inline editor (P-26)
            openNodeEditor(node);
            return;
        }
        if (node < 0 && x_.interaction.hitRangeHandle(px, py) < 0 && x_.bandListOpen && e.repeatClickCount() < 2)
            x_.bandListOpen = false;  // a click on empty plot closes the band list (§4.5)
        x_.interaction.pointerDown(px, py, NodesButton(e), modsOf(e), e.repeatClickCount());
        x_.redrawGraph();
    }
    void mouseDrag(const visage::MouseEvent& e) override {
        if (x_.drag.isOpen()) {
            x_.dragTo(e.position.y, e.isShiftDown());
            redraw();
            return;
        }
        if (matchSlider_) {
            matchSliderTo(e.position.x);
            return;
        }
        x_.interaction.pointerDrag(e.position.x, e.position.y, modsOf(e));
        pointer_ = {e.position.x, e.position.y};
        x_.redrawGraph();
    }
    void mouseUp(const visage::MouseEvent& e) override {
        if (x_.drag.isOpen()) {
            x_.endDrag();
            redraw();
            return;
        }
        if (matchSlider_) {
            matchSlider_ = 0;
            if (x_.match && x_.match->applied) x_.match->applier.finish();
            return;
        }
        x_.interaction.pointerUp(e.position.x, e.position.y, modsOf(e));
        x_.redrawGraph();
    }
    // Visage sends only mouseEnter on the move that enters a frame: treat it as a move.
    void mouseEnter(const visage::MouseEvent& e) override { mouseMove(e); }
    void mouseMove(const visage::MouseEvent& e) override {
        pointer_ = {e.position.x, e.position.y};
        inside_ = true;
        const int h = hitAt(hits_, e.position.x, e.position.y);
        hover_ = h >= 0 ? hits_[static_cast<size_t>(h)] : Hit{};
        Rect a = hover_.r;
        a.y += x_.L.graph.y;
        x_.hoverTip(h >= 0 ? tipFor(x_, hover_) : std::string(), a);
        redraw();
    }
    void mouseExit(const visage::MouseEvent&) override {
        inside_ = false;
        hover_ = {};
        x_.clearTip();
        redraw();
    }
    bool mouseWheel(const visage::MouseEvent& e) override {
        if (x_.matchMode && !plotRect().contains(e.position.x, e.position.y)) return true;
        const auto lo = x_.geometry.viewLo(), hi = x_.geometry.viewHi();
        if (x_.matchMode) x_.geometry.zoomAt(e.position.x, std::pow(GraphInteraction::kZoomPerWheel, e.precise_wheel_delta_y));
        else x_.interaction.wheel(e.position.x, e.position.y, e.precise_wheel_delta_y, modsOf(e));
        if ((x_.geometry.viewLo() != lo || x_.geometry.viewHi() != hi) && onViewChanged) onViewChanged();
        x_.redrawGraph();
        return true;
    }
    // Focus first, then graph commands; false hands the key to the host (area 15 R1-R3).
    bool keyPress(const visage::KeyEvent& e) override {
        if (x_.matchMode) {
            if (e.keyCode() == visage::KeyCode::Escape) {
                exitMatch();
                return true;
            }
            return false;
        }
        if (e.keyCode() == visage::KeyCode::Escape && x_.hintMode) {
            x_.hintMode = false;
            x_.redrawBars();
            return true;
        }
        if (listFocus_ && x_.bandListOpen) {  // band list keys (spec §6.3)
            const auto used = sortedBands();
            int i = static_cast<int>(std::find(used.begin(), used.end(), x_.interaction.focused()) - used.begin());
            if (e.keyCode() == visage::KeyCode::Up || e.keyCode() == visage::KeyCode::Down) {
                if (used.empty()) return true;
                i = std::clamp(i + (e.keyCode() == visage::KeyCode::Down ? 1 : -1), 0, static_cast<int>(used.size()) - 1);
                const int b = used[static_cast<size_t>(i)];
                auto sel = e.isShiftDown() ? x_.interaction.selection() : std::vector<int>{};
                if (std::find(sel.begin(), sel.end(), b) == sel.end()) sel.push_back(b);
                x_.interaction.setSelection(sel, b);
                x_.redrawGraph();
                return true;
            }
            if (e.keyCode() == visage::KeyCode::Return && x_.interaction.focused() >= 0) {
                x_.toggle(params::bandParam(x_.interaction.focused(), BandField::Enabled));
                return true;
            }
        }
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
        const bool handled = x_.keys.keyDown(k);
        if (handled) x_.redrawAll();
        return handled;
    }

    [[nodiscard]] Rect plotRect() const { return x_.L.plot; }
    // Handle size eases towards 22 px (selected) or 18 px over 100 ms (spec §7).
    bool animate(double dtMs) {
        bool moving = false;
        for (int b = 0; b < params::kBandCount; ++b) {
            auto& sz = size_[static_cast<size_t>(b)];
            const float target = x_.interaction.node(b).selected ? 22.0f : 18.0f;
            const float step = static_cast<float>(4.0 * dtMs / 100.0);
            if (std::abs(sz - target) <= step) sz = target;
            else sz += sz < target ? step : -step, moving = true;
        }
        redraw();
        return moving;
    }
    void enterMatch() {
        if (!x_.c.matchHost()) return;
        if (!x_.match) x_.match = std::make_unique<MatchUi>(x_.c);  // lazily, once (area 12 R2)
        x_.matchMode = true;
        x_.bandListOpen = false;
        o_.close();
        x_.relayout();
    }
    void exitMatch() {
        if (x_.match) x_.match->applier.finish();
        x_.matchMode = false;
        x_.relayout();
    }
    void pollMatch() {
        MatchHost* host = x_.c.matchHost();
        if (!x_.matchMode || !x_.match || !host) return;
        auto& mu = *x_.match;
        host->status(mu.st);
        std::vector<MatchBand> bands;
        if (host->takeFit(bands)) {
            mu.applier.setFit(bands);
            mu.count = static_cast<int>(bands.size());
            mu.amount = 1.0;
            mu.applied = false;
            mu.msg = bands.empty() ? "Nothing to match: the curves already agree." : "";
        }
        if (!mu.st.fileError.empty()) mu.msg = mu.st.fileError;
        x_.redrawAll();
    }

private:
    static Button NodesButton(const visage::MouseEvent& e) {
        return e.isRightButton() ? Button::Right : e.isMiddleButton() ? Button::Middle : Button::Left;
    }
    void add(Rect r, int kind, int arg = 0, params::ParamIndex id = 0) { hits_.push_back({r, kind, arg, id}); }
    [[nodiscard]] bool hov(int kind, int arg = 0) const { return hover_.kind == kind && hover_.arg == arg; }
    [[nodiscard]] bool hovId(int kind, params::ParamIndex id) const { return hover_.kind == kind && hover_.id == id; }

    std::vector<int> sortedBands() const {
        auto used = x_.ops.usedBands();
        std::sort(used.begin(), used.end(), [&](int a, int b) { return x_.bval(a, BandField::Freq) < x_.bval(b, BandField::Freq); });
        return used;
    }

    // --- handles (§5.4) ---
    void drawHandles(visage::Canvas& c) {
        if (x_.matchMode) {
            drawFittedNodes(c);
            return;
        }
        int solo = -1;
        for (int b = 0; b < params::kBandCount; ++b)
            if (x_.bval(b, BandField::Used) >= 0.5 && x_.bval(b, BandField::Solo) >= 0.5) solo = b;
        // Range stems and handles first (below the nodes).
        for (int b = 0; b < params::kBandCount; ++b) {
            const auto n = x_.interaction.node(b);
            if (!n.used || !n.hasRangeHandle || !n.dynamic || n.rangeDb == 0.0) continue;
            const uint32_t col = bandColour(b);
            Poly stem;
            stem.moveTo(n.x, n.y);
            stem.lineTo(n.x, n.rangeY);
            c.setColor(col);
            strokePoly(c, stem, 1.0f, {3.0f, 3.0f});
            const bool hv = x_.interaction.hitRangeHandle(pointer_.x, pointer_.y) == b && inside_;
            c.setColor(0x99000000);
            c.circle(n.x - 8.0f, n.rangeY - 6.0f, 16.0f);
            c.setColor(hv ? tok::surface300 : tok::surface100);
            c.circle(n.x - 7.0f, n.rangeY - 7.0f, 14.0f);
            c.setColor(col);
            c.ring(n.x - 7.0f, n.rangeY - 7.0f, 14.0f, 2.0f);
        }
        const int hoverNode = inside_ ? x_.interaction.hitTest(pointer_.x, pointer_.y) : -1;
        std::vector<int> order;
        for (int b = 0; b < params::kBandCount; ++b)
            if (x_.interaction.node(b).used) order.push_back(b);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return x_.interaction.node(a).selected < x_.interaction.node(b).selected;
        });
        for (int b : order) {
            const auto n = x_.interaction.node(b);
            const bool sel = n.selected;
            const float d = size_[static_cast<size_t>(b)];
            const uint32_t col = bandColour(b);
            const float alpha = n.enabled ? 1.0f : tok::opacityBypassed;
            // shadow-handle: ground ring + drop shadow
            c.setColor(withAlpha(0x99000000, alpha));
            c.circle(n.x - d * 0.5f - 1.0f, n.y - d * 0.5f + 1.0f, d + 2.0f);
            if (sel) {
                c.setColor(withAlpha(tok::surface100, alpha));
                c.circle(n.x - d * 0.5f - 5.0f, n.y - d * 0.5f - 5.0f, d + 10.0f);
                c.setColor(withAlpha(x_.interaction.dragging() && hoverNode == b ? tok::accentHover : tok::ink, alpha));
                c.circle(n.x - d * 0.5f - 3.0f, n.y - d * 0.5f - 3.0f, d + 6.0f);
            } else {
                c.setColor(withAlpha(tok::surface100, alpha));
                c.circle(n.x - d * 0.5f - 2.0f, n.y - d * 0.5f - 2.0f, d + 4.0f);
                if (hoverNode == b) {
                    c.setColor(tok::inkMuted);
                    c.ring(n.x - d * 0.5f - 3.0f, n.y - d * 0.5f - 3.0f, d + 6.0f, 1.0f);
                }
            }
            if (n.enabled) {
                c.setColor(col);
                c.circle(n.x - d * 0.5f, n.y - d * 0.5f, d);
            } else {
                c.setColor(withAlpha(tok::surface100, alpha));
                c.circle(n.x - d * 0.5f, n.y - d * 0.5f, d);
                c.setColor(withAlpha(col, alpha));
                c.ring(n.x - d * 0.5f, n.y - d * 0.5f, d, 2.0f);
            }
            if (n.focused && x_.interaction.selection().size() > 1) {  // focus ring for the focused band
                c.setColor(tok::accent);
                c.ring(n.x - d * 0.5f - 9.0f, n.y - d * 0.5f - 9.0f, d + 18.0f, 2.0f);
            }
            if (b == solo) {  // solo badge (P-19)
                const float bx = n.x + d * 0.5f * 0.707f, by = n.y - d * 0.5f * 0.707f;
                c.setColor(tok::surface100);
                c.circle(bx - 5.0f, by - 5.0f, 10.0f);
                c.setColor(tok::signalSolo);
                c.circle(bx - 3.0f, by - 3.0f, 6.0f);
            }
        }
    }

    // --- readouts (§5.6, §4.12) ---
    void chipAt(visage::Canvas& c, float ax, float ay, float nodeX, const std::string& t, uint32_t dot) {
        const float w = readoutWidth(t, dot != 0), h = 22.0f;
        float rx = ax, ry = ay;
        const Rect plot = plotRect();
        if (rx + w > plot.right() - 8.0f) rx = nodeX - 18.0f - w;
        if (ry < plot.y) ry = ay + 70.0f;
        const Rect pr = x_.L.panel.rect;
        if (x_.L.panel.mode != PanelMode::Hidden && Rect{pr.x - 4.0f, pr.y - 4.0f, pr.w + 8.0f, pr.h + 8.0f}.contains(rx + w * 0.5f, ry + h))
            ry = pr.y - h - 8.0f;
        readout(c, {rx, ry, w, h}, t, false, false, dot, true);
    }
    void drawReadouts(visage::Canvas& c) {
        const int f = x_.interaction.focused();
        if (x_.interaction.soloDragActive() && f >= 0) {
            const auto n = x_.interaction.node(f);
            chipAt(c, n.x + 18.0f, n.y - 44.0f, n.x, "Solo Gain " + fmt::db(x_.interaction.soloDragDb()), tok::signalSolo);
            return;
        }
        if (f >= 0 && x_.interaction.node(f).used) {
            const auto n = x_.interaction.node(f);
            const auto info = params::shapeInfo(static_cast<dsp::Shape>(static_cast<int>(x_.bval(f, BandField::Shape))));
            std::string t = fmt::hz(n.hz);
            const std::string dot = " \xC2\xB7 ";
            if (info.hasGain) t += dot + fmt::db(x_.bval(f, BandField::Gain));
            if (info.hasQ) t += dot + "Q " + fmt::q(params::qFromPosition(x_.bval(f, BandField::Q)));
            chipAt(c, n.x + 18.0f, n.y - 44.0f, n.x, t, bandColour(f));
            return;
        }
        // Hover readout and add cursor over empty plot (§4.12).
        if (!inside_ || x_.interaction.dragging() || x_.interaction.lassoActive()) return;
        const Rect plot = plotRect();
        if (!plot.contains(pointer_.x, pointer_.y)) return;
        if (x_.interaction.hitTest(pointer_.x, pointer_.y) >= 0 || x_.interaction.hitRangeHandle(pointer_.x, pointer_.y) >= 0) return;
        if (x_.L.panel.mode != PanelMode::Hidden && x_.L.panel.rect.contains(pointer_.x, pointer_.y)) return;
        if (x_.bandListOpen && bandListRect_.contains(pointer_.x, pointer_.y)) return;
        if (x_.usedCount() >= params::kBandCount) {
            chipAt(c, pointer_.x + 16.0f, pointer_.y - 32.0f, pointer_.x, "24 bands in use", 0);
            return;
        }
        c.setColor(tok::ink);
        Poly ring;
        ring.addCircle(pointer_.x, pointer_.y, 9.0f);
        strokePoly(c, ring, 1.5f, {3.0f, 3.0f});
        c.segment(pointer_.x - 4.0f, pointer_.y, pointer_.x + 4.0f, pointer_.y, 1.5f, false);
        c.segment(pointer_.x, pointer_.y - 4.0f, pointer_.x, pointer_.y + 4.0f, 1.5f, false);
        const std::string dot = " \xC2\xB7 ";
        chipAt(c, pointer_.x + 16.0f, pointer_.y - 32.0f, pointer_.x,
               "Double-click to add" + dot + fmt::hz(x_.geometry.hzOfX(pointer_.x)) + dot + fmt::db(x_.geometry.dbOfY(pointer_.y)), 0);
    }

    // --- band panel (panels §2.3) ---
    // Draws a knob centred in its slot (EditorLayout::knobSlot) and returns the slot width.
    float knobHit(visage::Canvas& c, float x, float yBottom, float size, int b, BandField f, const std::string& label, bool bipolar,
                  bool greyed, const std::string& valueText, float grFraction = 0.0f) {
        const float slot = EditorLayout::knobSlot(size, static_cast<int>(label.size()));
        x += (slot - size) * 0.5f;
        const auto id = params::bandParam(b, f);
        const float y = yBottom - size - EditorLayout::kKnobText;
        const double n = params::toNormalised(*params::specAt(id), x_.val(id));
        KnobStyle st;
        st.bipolar = bipolar, st.greyed = greyed, st.hover = hovId(kKnob, id), st.dragging = x_.drag.isOpen() && x_.dragId == id;
        st.grFraction = grFraction, st.grColour = bandColour(b);
        knob(c, x, y, size, n, label, valueText, st);
        add({x, y, size, size}, kKnob, 0, id);
        add({x + size * 0.5f - 40.0f, y + size + 28.0f, 80.0f, 20.0f}, kKnobValue, 0, id);
        return slot;
    }
    void drawPanel(visage::Canvas& c) {
        const auto& P = x_.L.panel;
        const int b = x_.interaction.focused();
        if (P.mode == PanelMode::Hidden || b < 0) return;
        const auto& F = fonts();
        const uint32_t col = bandColour(b);
        const auto content = x_.panelContent();
        const bool enabled = x_.bval(b, BandField::Enabled) >= 0.5;
        Rect r = P.rect;
        if (P.mode == PanelMode::Strip && !x_.stripExpanded) {
            theme::panel(c, r, 16.0f);
            float xx = r.x + 16.0f;
            c.setColor(col);
            c.circle(xx, r.y + 17.0f, 10.0f);
            xx += 18.0f;
            const std::string title = "Band #" + std::to_string(b + 1);
            text(c, F.semi13, title, xx, r.y, 80.0f, r.h, tok::ink);
            xx += textWidth(F.semi13, title) + 12.0f;
            const auto info = params::shapeInfo(static_cast<dsp::Shape>(static_cast<int>(x_.bval(b, BandField::Shape))));
            text(c, F.label13, std::string(info.name), xx, r.y, 100.0f, r.h, tok::inkMuted);
            const Rect er{r.right() - 36.0f, r.y + 8.0f, 28.0f, 28.0f};
            iconButton(c, er, icons::chevUp, {hov(kStripExpand)});
            add(er, kStripExpand);
            return;
        }
        if (P.mode == PanelMode::Strip) {  // expanded strip: the compact panel over the plot
            r.y = std::max(4.0f, r.bottom() - 260.0f);
        }
        theme::panel(c, r, 16.0f);
        const float cy0 = r.y + P.padY;
        const float rowBottom = cy0 + P.row1H;
        float x = r.x + P.padX;
        // Column A
        c.setColor(withAlpha(col, enabled ? 1.0f : tok::opacityBypassed));
        c.circle(x, cy0 + 9.0f, 10.0f);
        std::string title = "Band #" + std::to_string(b + 1);
        text(c, F.semi13, title, x + 18.0f, cy0, 120.0f, 28.0f, enabled ? tok::ink : tok::inkDisabled);
        if (x_.interaction.selection().size() > 1)
            text(c, F.caption12, "(multiple)", x + 22.0f + textWidth(F.semi13, title), cy0, 80.0f, 28.0f, tok::inkMuted);
        const Rect enR{x + EditorLayout::kColumn - 28.0f, cy0, 28.0f, 28.0f};
        iconButton(c, enR, icons::power, {hov(kToggle, 1), false, false, true}, !enabled);
        if (!enabled) icon(c, icons::power, enR.x + 6.0f, enR.y + 6.0f, tok::inkDisabled);
        add(enR, kToggle, 1, params::bandParam(b, BandField::Enabled));
        float y = cy0 + 36.0f;
        const auto shape = static_cast<int>(x_.bval(b, BandField::Shape));
        const Rect shR{x, y, EditorLayout::kColumn, P.control};
        select(c, shR, icons::shapeGlyph(shape), std::string(params::shapeInfo(static_cast<dsp::Shape>(shape)).name), "", 1,
               {hov(kShape), false, false, true, o_.kind() == Pop::Menu && menuOwner_ == kShape});
        add(shR, kShape);
        y += P.control + 8.0f;
        if (content.slope) {
            const Rect slR{x, y, EditorLayout::kColumn, P.control};
            select(c, slR, "", fmt::slope(static_cast<int>(x_.bval(b, BandField::Slope))), "", 1,
                   {hov(kSlope), false, false, true, o_.kind() == Pop::Slope});
            add(slR, kSlope);
        }
        x += EditorLayout::kColumn + EditorLayout::kColGap;
        // Column B: Freq, Gain, Q
        const double hz = std::exp2(x_.bval(b, BandField::Freq));
        knobHit(c, x, rowBottom, P.knobSm, b, BandField::Freq, "Freq", false, false, fmt::hz(hz));
        x += P.knobSm + EditorLayout::kKnobGap;
        if (content.gain) {
            knobHit(c, x, rowBottom, P.knobLg, b, BandField::Gain, "Gain", true, false, fmt::db(x_.bval(b, BandField::Gain)));
            x += P.knobLg + EditorLayout::kKnobGap;
        }
        if (content.q) {
            knobHit(c, x, rowBottom, P.knobSm, b, BandField::Q, "Q", false, false, fmt::q(params::qFromPosition(x_.bval(b, BandField::Q))));
            x += P.knobSm + EditorLayout::kKnobGap;
        }
        x += EditorLayout::kColGap - EditorLayout::kKnobGap;
        // Dynamics group and SIDE CHAIN, inline or on the second row.
        float dynX = x, dynTop = cy0, dynBottom = rowBottom;
        if (P.wrapped) {
            const float sepY = cy0 + P.row1H + 16.0f;
            c.setColor(tok::lineHairline);
            c.fill(r.x + P.padX, sepY, r.w - 2.0f * P.padX, 1.0f);
            dynX = r.x + P.padX, dynTop = sepY + 17.0f, dynBottom = dynTop + P.row2H;
        }
        float colDX = x;
        if (content.dynamic) {
            float dx = dynX;
            if (!P.wrapped) {
                c.setColor(tok::lineHairline);
                c.fill(dx, dynTop, 1.0f, dynBottom - dynTop);
            }
            dx += 24.0f;
            const bool autoOn = x_.bval(b, BandField::DynAuto) >= 0.5;
            const Rect aR{dx, dynTop, chipWidth("Auto"), 28.0f};
            chip(c, aR, "Auto", {hov(kToggle, 2), false, autoOn});
            add(aR, kToggle, 2, params::bandParam(b, BandField::DynAuto));
            const float range = static_cast<float>(x_.bval(b, BandField::DynRange));
            const float gr = x_.poller.gainChange().currentDb[static_cast<size_t>(b)];
            if (content.rangeKnob) {
                dx += knobHit(c, dx, dynBottom, P.knobDyn, b, BandField::DynRange, "Range", true, false, fmt::db(range),
                              std::clamp(gr / 30.0f, -1.0f, 1.0f)) +
                      EditorLayout::kDynGap;
            }
            dx += knobHit(c, dx, dynBottom, P.knobDyn, b, BandField::Threshold, "Threshold", false, autoOn,
                          autoOn ? "Auto" : fmt::dbfs(x_.bval(b, BandField::Threshold))) +
                  EditorLayout::kDynGap;
            dx += knobHit(c, dx, dynBottom, P.knobDyn, b, BandField::Attack, "Attack", false, autoOn,
                          fmt::pct(x_.bval(b, BandField::Attack))) +
                  EditorLayout::kDynGap;
            dx += knobHit(c, dx, dynBottom, P.knobDyn, b, BandField::Release, "Release", false, autoOn,
                          fmt::pct(x_.bval(b, BandField::Release))) +
                  EditorLayout::kDynGap;
            if (content.spectral)
                dx += knobHit(c, dx, dynBottom, P.knobDyn, b, BandField::SpectralDensity, "Density", false, false,
                              fmt::pct(x_.bval(b, BandField::SpectralDensity))) +
                      EditorLayout::kDynGap;
            dx += EditorLayout::kColGap - EditorLayout::kDynGap;
            // SIDE CHAIN column
            float sy = dynTop;
            text(c, F.semi12, "SIDE CHAIN", dx, sy, EditorLayout::kColumn, 14.0f, tok::inkMuted);
            sy += 22.0f;
            const bool ext = x_.bval(b, BandField::ScExternal) >= 0.5;
            const float cw = (EditorLayout::kColumn - 28.0f - 8.0f) * 0.5f;
            chip(c, {dx, sy, cw, 28.0f}, "Int", {hov(kScExt, 0), false, !ext});
            add({dx, sy, cw, 28.0f}, kScExt, 0);
            chip(c, {dx + cw + 4.0f, sy, cw, 28.0f}, "Ext", {hov(kScExt, 1), false, ext});
            add({dx + cw + 4.0f, sy, cw, 28.0f}, kScExt, 1);
            const Rect lR{dx + EditorLayout::kColumn - 28.0f, sy, 28.0f, 28.0f};
            const bool listen = x_.bval(b, BandField::ScListen) >= 0.5;
            iconButton(c, lR, icons::headphones, {hov(kToggle, 3), false, listen});
            if (listen) {
                c.setColor(tok::signalSolo);
                c.circle(lR.right() - 10.0f, lR.y + 2.0f, 8.0f);
            }
            add(lR, kToggle, 3, params::bandParam(b, BandField::ScListen));
            sy += 36.0f;
            const bool free = x_.bval(b, BandField::ScMode) >= 0.5;
            double lo = std::exp2(x_.bval(b, BandField::ScLow)), hi = std::exp2(x_.bval(b, BandField::ScHigh));
            if (!free) lo = hz / 2.0, hi = hz * 2.0;  // Band mode: the band's own derived range (display, PROVISIONAL)
            const Rect fR{dx, sy, EditorLayout::kColumn, 28.0f};
            select(c, fR, "", free ? "Free" : "Band", fmt::hzRange(lo, hi), 1, {hov(kScFilter), false, false, true, o_.kind() == Pop::ScFilter});
            add(fR, kScFilter);
            sy += 36.0f;
            if (content.spectral) {
                const auto tid = params::bandParam(b, BandField::SpectralTilt);
                const Rect tR{dx, sy, EditorLayout::kColumn, 28.0f};
                chip(c, tR, "Spectral Tilt", {hov(kToggle, 4), false, x_.on(tid)});
                add(tR, kToggle, 4, tid);
            }
            if (!P.wrapped) colDX = dx + EditorLayout::kColumn + EditorLayout::kColGap;
        }
        // Column D: placement, Dynamic / Spectral, Solo, Delete
        float dy = cy0;
        const Rect plR{colDX, dy, EditorLayout::kColumn, P.control};
        const auto& places = params::specAt(params::bandParam(0, BandField::Placement))->choices;
        select(c, plR, "M6 4 A4 4 0 1 0 6 12 A4 4 0 1 0 6 4 M10 4 A4 4 0 1 0 10 12 A4 4 0 1 0 10 4",
               places[static_cast<size_t>(std::clamp(static_cast<int>(x_.bval(b, BandField::Placement)), 0, 4))], "", 1,
               {hov(kPlacement), false, false, true, o_.kind() == Pop::Menu && menuOwner_ == kPlacement});
        add(plR, kPlacement);
        dy += P.control + 8.0f;
        const float hw = (EditorLayout::kColumn - 4.0f) * 0.5f;
        const bool gainShape = content.gain;
        chip(c, {colDX, dy, hw, P.control}, "Dynamic", {hov(kToggle, 5), false, content.dynamic, gainShape});
        if (gainShape) add({colDX, dy, hw, P.control}, kToggle, 5, params::bandParam(b, BandField::DynEnabled));
        chip(c, {colDX + hw + 4.0f, dy, hw, P.control}, "Spectral", {hov(kToggle, 6), false, content.spectral, gainShape});
        if (gainShape) add({colDX + hw + 4.0f, dy, hw, P.control}, kToggle, 6, params::bandParam(b, BandField::Spectral));
        dy += P.control + 8.0f;
        const Rect delR{colDX + EditorLayout::kColumn - 28.0f, dy, 28.0f, 28.0f};
        const Rect soR{delR.x - 32.0f, dy, 28.0f, 28.0f};
        const bool solo = x_.bval(b, BandField::Solo) >= 0.5;
        iconButton(c, soR, icons::headphones, {hov(kToggle, 7), false, solo});
        if (solo) {
            c.setColor(tok::signalSolo);
            c.circle(soR.right() - 10.0f, soR.y + 2.0f, 8.0f);
        }
        add(soR, kToggle, 7, params::bandParam(b, BandField::Solo));
        iconButton(c, delR, icons::close, {hov(kDelete)});
        add(delR, kDelete);
        panelRect_ = r;
        add(r, kNone);  // the panel swallows clicks
    }

    // --- band list (panels §2.4, §4.5) ---
    void drawBandList(visage::Canvas& c) {
        const auto& F = fonts();
        const auto used = sortedBands();
        const float h = std::min(8.0f + 26.0f + 38.0f * static_cast<float>(used.size()) + 42.0f + 8.0f, height() - 32.0f);
        const Rect r{16.0f, 16.0f, 260.0f, h};
        bandListRect_ = r;
        theme::panel(c, r, 16.0f);
        text(c, F.semi12, "BANDS", r.x + 18.0f, r.y + 12.0f, 100.0f, 14.0f, tok::inkMuted);
        text(c, F.caption12, std::to_string(used.size()) + " of 24", r.x, r.y + 11.0f, r.w - 18.0f, 16.0f, tok::inkMuted,
             visage::Font::kRight);
        float y = r.y + 34.0f;
        for (int b : used) {
            if (y + 36.0f > r.bottom() - 42.0f) break;
            const Rect row{r.x + 8.0f, y, r.w - 16.0f, 36.0f};
            const bool sel = x_.interaction.isSelected(b);
            const bool en = x_.bval(b, BandField::Enabled) >= 0.5;
            if (sel) {
                c.setColor(tok::lineHairline);
                c.roundedRectangle(row.x, row.y, row.w, row.h, 8.0f);
                c.setColor(tok::surface300);
                c.roundedRectangle(row.x + 1.0f, row.y + 1.0f, row.w - 2.0f, row.h - 2.0f, 7.0f);
            } else if (hov(kBandRow, b)) {
                c.setColor(withAlpha(tok::surface300, 0.5f));
                c.roundedRectangle(row.x, row.y, row.w, row.h, 8.0f);
            }
            if (listFocus_ && x_.interaction.focused() == b) {
                c.setColor(tok::accent);
                c.roundedRectangleBorder(row.x - 2.0f, row.y - 2.0f, row.w + 4.0f, row.h + 4.0f, 9.0f, 2.0f);
            }
            text(c, F.micro10, "#" + std::to_string(b + 1), row.x + 10.0f, row.y, 22.0f, row.h, tok::inkMuted);
            const uint32_t col = bandColour(b);
            if (en) {
                c.setColor(col);
                c.circle(row.x + 42.0f, row.y + 13.0f, 10.0f);
            } else {
                c.setColor(col);
                c.ring(row.x + 42.0f, row.y + 13.0f, 10.0f, 2.0f);
            }
            std::string name(params::shapeInfo(static_cast<dsp::Shape>(static_cast<int>(x_.bval(b, BandField::Shape)))).name);
            if (!en) name += " \xC2\xB7 off";
            else if (x_.bval(b, BandField::Spectral) >= 0.5 && x_.bval(b, BandField::DynEnabled) >= 0.5) name += " \xC2\xB7 Spec";
            else if (x_.bval(b, BandField::DynEnabled) >= 0.5) name += " \xC2\xB7 Dyn";
            if (x_.bval(b, BandField::Solo) >= 0.5) name += " \xC2\xB7 Solo";
            text(c, F.label13, name, row.x + 62.0f, row.y, 120.0f, row.h, en ? tok::ink : tok::inkMuted);
            text(c, F.semi13, fmt::hz(std::exp2(x_.bval(b, BandField::Freq))), row.x, row.y, row.w - 10.0f, row.h,
                 en ? tok::ink : tok::inkMuted, visage::Font::kRight);
            add(row, kBandRow, b);
            y += 38.0f;
        }
        text(c, F.caption12, "\xE2\x86\x91 \xE2\x86\x93 move \xC2\xB7 Shift adds \xC2\xB7 Enter enables", r.x + 18.0f, r.bottom() - 42.0f,
             r.w - 28.0f, 16.0f, tok::inkMuted);
        text(c, F.caption12, "Del removes", r.x + 18.0f, r.bottom() - 26.0f, r.w - 28.0f, 16.0f, tok::inkMuted);
        add(r, kNone);
    }

    void drawLegendPanel(visage::Canvas& c, const std::vector<std::pair<std::string, int>>& entries) {
        const auto& F = fonts();
        float w = 28.0f;
        for (const auto& e : entries) w += 28.0f + textWidth(F.label13, e.first) + 16.0f;
        w = std::min(w - 16.0f, 560.0f);
        const Rect r{24.0f, 24.0f, w, 36.0f};
        theme::panel(c, r, 16.0f);
        float x = r.x + 14.0f;
        const float cy = r.y + 18.0f;
        for (const auto& [name, kind] : entries) {
            Poly p;
            p.moveTo(x, cy);
            p.lineTo(x + 20.0f, cy);
            switch (kind) {
                case 0:  // pre fill
                    c.setColor(tok::analyzerPost);
                    c.fill(x - 1.0f, cy - 6.0f, 18.0f, 12.0f);
                    c.setColor(tok::analyzerPre);
                    c.fill(x, cy - 5.0f, 16.0f, 10.0f);
                    break;
                case 1: c.setColor(tok::analyzerPost), strokePoly(c, p, 1.5f); break;
                case 2: c.setColor(tok::analyzerSc), strokePoly(c, p, 1.5f, {2.0f, 2.0f}); break;
                case 3: c.setColor(tok::inkMuted), strokePoly(c, p, 1.5f, {1.0f, 3.0f}); break;
                case 4: c.setColor(withAlpha(tok::signalClip, 0.35f)), c.fill(x, cy - 5.0f, 16.0f, 10.0f); break;
                case 5: c.setColor(tok::analyzerSc), strokePoly(c, p, 1.5f); break;
                case 6: c.setColor(tok::ink), strokePoly(c, p, 1.0f, {4.0f, 3.0f}); break;
                case 7: c.setColor(tok::accent), strokePoly(c, p, 2.0f); break;
                case 8: c.setColor(tok::inkMuted), strokePoly(c, p, 1.0f, {1.0f, 2.0f}); break;
                default: break;
            }
            x += 28.0f;
            text(c, F.label13, name, x, r.y, textWidth(F.label13, name) + 2.0f, r.h, tok::ink);
            x += textWidth(F.label13, name) + 16.0f;
        }
    }
    void drawAnalyzerLegend(visage::Canvas& c) {
        drawLegendPanel(c, {{"Pre", 0}, {"Post", 1}, {"Side chain", 2}, {"Peak hold", 3}, {"Collision", 4}});
    }
    void drawMatchLegend(visage::Canvas& c) {
        drawLegendPanel(c, {{"Input", 1}, {"Reference", 5}, {"Difference", 6}, {"Residual", 8}, {"Match", 7}});
    }

    // --- EQ Match (panels §2.8) ---
    void drawFittedNodes(visage::Canvas& c) {
        if (!x_.match) return;
        const auto& mu = *x_.match;
        const auto& fit = mu.applier.fit();
        for (size_t i = 0; i < fit.size(); ++i) {
            const auto b = EqMatchApplier::scaled(fit[i], mu.amount);
            const float nx = x_.geometry.xOfHz(b.hz), ny = x_.geometry.yOfDb(dsp::shapeHasGain(b.shape) ? b.gainDb : 0.0);
            const bool applied = static_cast<int>(i) < mu.count;
            const uint32_t col = bandColour(static_cast<int>(i));
            const float a = applied ? 1.0f : tok::opacityPreview;
            c.setColor(withAlpha(tok::surface100, a));
            c.circle(nx - 11.0f, ny - 11.0f, 22.0f);
            if (applied) {
                c.setColor(col);
                c.circle(nx - 9.0f, ny - 9.0f, 18.0f);
            } else {
                c.setColor(withAlpha(col, a));
                c.ring(nx - 9.0f, ny - 9.0f, 18.0f, 2.0f);
            }
        }
    }
    void drawMatchPanel(visage::Canvas& c) {
        auto* mup = x_.match.get();
        if (!mup) return;
        auto& mu = *mup;
        const auto& st = mu.st;
        const auto& F = fonts();
        // One row of columns; below the panel's width it wraps after CAPTURE (spec §1.2 reflow).
        const float wOne = 20.0f + 170.0f + 49.0f + 220.0f + 49.0f + 170.0f + 49.0f + 240.0f + 24.0f + 120.0f + 20.0f;
        const float hRow = 16.0f + 14.0f + 8.0f + 32.0f + 8.0f + 32.0f + 8.0f + 32.0f + 8.0f + 40.0f + 16.0f;
        const bool twoRows = width() - 32.0f < wOne;
        const float w = twoRows ? 20.0f + 170.0f + 49.0f + 240.0f + 24.0f + 120.0f + 20.0f : wOne;
        const float h = twoRows ? 2.0f * hRow + 1.0f : hRow;
        const Rect r{std::max(0.0f, (width() - w) * 0.5f), std::max(8.0f, height() - (twoRows ? 16.0f : 32.0f) - h), w, h};
        theme::panel(c, r, 16.0f);
        add(r, kNone);
        float x = r.x + 20.0f;
        float top = r.y + 16.0f;
        auto rule = [&](float at) {
            c.setColor(tok::lineHairline);
            c.fill(at, top, 1.0f, hRow - 32.0f);
        };
        // 1 · REFERENCE
        text(c, F.semi12, "1 \xC2\xB7 REFERENCE", x, top, 170.0f, 14.0f, tok::inkMuted);
        const Rect src{x, top + 22.0f, 170.0f, 32.0f};
        select(c, src, "", st.referenceIsFile ? "Audio file\xE2\x80\xA6" : "Side-chain", "", 1, {hov(kMatchSource), false, false, !st.fileBusy});
        add(src, kMatchSource);
        if (st.referenceIsFile && st.fileBusy) {
            c.setColor(tok::accent);
            c.fill(src.x, src.bottom() + 2.0f, src.w * st.fileProgress, 4.0f);
        }
        text(c, F.caption12, "Or load an audio file.", x, top + 62.0f, 170.0f, 16.0f, tok::inkMuted);
        text(c, F.caption12, "Another Am-Q instance: v2.", x, top + 78.0f, 170.0f, 16.0f, tok::inkMuted);
        x += 170.0f + 24.0f;
        rule(x);
        x += 25.0f;
        // 2 · CAPTURE
        text(c, F.semi12, "2 \xC2\xB7 CAPTURE", x, top, 220.0f, 14.0f, tok::inkMuted);
        float y = top + 22.0f;
        for (int cap = 0; cap < 2; ++cap) {
            const bool ready = cap == 0 ? st.inputReady : st.referenceReady;
            const bool rec = cap == 0 ? st.inputRecording : st.referenceRecording;
            const double secs = cap == 0 ? st.inputSeconds : st.referenceSeconds;
            c.setColor(cap == 0 ? tok::analyzerPost : tok::analyzerSc);
            c.circle(x, y + 12.0f, 8.0f);
            text(c, F.label13, cap == 0 ? "Input" : "Reference", x + 16.0f, y, 90.0f, 32.0f, tok::ink);
            char buf[48];
            if (cap == 1 && st.referenceIsFile) std::snprintf(buf, sizeof buf, "%s", ready ? "\xE2\x9C\x93 file" : (st.fileBusy ? "analysing" : "no file"));
            else if (ready) std::snprintf(buf, sizeof buf, "\xE2\x9C\x93 %.1f s", secs);
            else std::snprintf(buf, sizeof buf, "Need 2 s");
            text(c, F.label13, buf, x + 90.0f, y, 66.0f, 32.0f, ready ? tok::ink : tok::inkMuted, visage::Font::kRight);
            if (!(cap == 1 && st.referenceIsFile)) {
                const Rect recR{x + 220.0f - 60.0f, y + 2.0f, 28.0f, 28.0f};
                iconButton(c, recR, "", {hov(kMatchRec, cap), false, rec});
                c.setColor(rec ? tok::signalClip : tok::ink);
                c.circle(recR.x + 10.0f, recR.y + 10.0f, 8.0f);
                add(recR, kMatchRec, cap);
                const Rect rsR{x + 220.0f - 28.0f, y + 2.0f, 28.0f, 28.0f};
                iconButton(c, rsR, icons::reset, {hov(kMatchReset, cap)});
                add(rsR, kMatchReset, cap);
            }
            y += 32.0f;
        }
        char wb[48];
        std::snprintf(wb, sizeof wb, st.lastSeconds ? "Last %.0f s" : "Whole capture", st.windowSeconds);
        const Rect winR{x, y + 8.0f, 220.0f, 28.0f};
        select(c, winR, "", wb, "Window", 1, {hov(kMatchWindow), false, false, true, o_.kind() == Pop::Menu && menuOwner_ == kMatchWindow}, true);
        add(winR, kMatchWindow);
        const double dropped = st.inputDroppedSeconds + st.referenceDroppedSeconds;
        if (dropped > 0.0) {
            icon(c, icons::warning, x, y + 44.0f, tok::meterHot, 1.2f);
            char db2[64];
            std::snprintf(db2, sizeof db2, "Audio dropped: %.2f s", dropped);
            text(c, F.caption12, db2, x + 20.0f, y + 44.0f, 200.0f, 16.0f, tok::inkMuted);
        }
        if (twoRows) {
            c.setColor(tok::lineHairline);
            c.fill(r.x + 20.0f, r.y + hRow, r.w - 40.0f, 1.0f);
            x = r.x + 20.0f;
            top = r.y + hRow + 1.0f + 16.0f;
        } else {
            x += 220.0f + 24.0f;
            rule(x);
            x += 25.0f;
        }
        // 3 · ALIGN
        text(c, F.semi12, "3 \xC2\xB7 ALIGN", x, top, 170.0f, 14.0f, tok::inkMuted);
        text(c, F.label13, "Level alignment", x, top + 22.0f, 120.0f, 28.0f, tok::ink);
        const Rect alR{x + 170.0f - chipWidth("Off"), top + 22.0f, chipWidth("Off"), 28.0f};
        chip(c, alR, st.levelAlign ? "On" : "Off", {hov(kMatchAlign), false, st.levelAlign});
        add(alR, kMatchAlign);
        text(c, F.caption12, "Removes the loudness gap:", x, top + 56.0f, 170.0f, 16.0f, tok::inkMuted);
        text(c, F.caption12, st.curveValid ? fmt::db(st.levelOffsetDb) : "\xE2\x80\x94", x, top + 72.0f, 170.0f, 16.0f, tok::inkMuted);
        static const char* smooth[] = {"1/3 oct", "1/6 oct", "1/12 oct", "1/24 oct"};
        const Rect smR{x, top + 96.0f, 170.0f, 28.0f};
        select(c, smR, "", smooth[std::clamp(st.smoothing, 0, 3)], "Smoothing", 1,
               {hov(kMatchSmoothing), false, false, true, o_.kind() == Pop::Menu && menuOwner_ == kMatchSmoothing}, true);
        add(smR, kMatchSmoothing);
        x += 170.0f + 24.0f;
        rule(x);
        x += 25.0f;
        // 4 · FIT
        const bool haveFit = !mu.applier.fit().empty();
        text(c, F.semi12, "4 \xC2\xB7 FIT", x, top, 240.0f, 14.0f, tok::inkMuted);
        text(c, F.label13, "Amount", x, top + 22.0f, 120.0f, 16.0f, haveFit ? tok::ink : tok::inkDisabled);
        text(c, F.semi13, fmt::pct(100.0 * mu.amount), x, top + 22.0f, 240.0f, 16.0f, haveFit ? tok::accent : tok::inkDisabled,
             visage::Font::kRight);
        matchSliderDraw(c, {x, top + 42.0f, 240.0f, 20.0f}, mu.amount, haveFit, 1);
        const int nFit = static_cast<int>(mu.applier.fit().size());
        text(c, F.label13, "Bands to apply", x, top + 72.0f, 140.0f, 16.0f, haveFit ? tok::ink : tok::inkDisabled);
        text(c, F.semi13, std::to_string(mu.count) + " of " + std::to_string(nFit) + " fitted", x, top + 72.0f, 240.0f, 16.0f,
             haveFit ? tok::accent : tok::inkDisabled, visage::Font::kRight);
        matchSliderDraw(c, {x, top + 92.0f, 240.0f, 20.0f}, nFit ? static_cast<double>(mu.count) / nFit : 0.0, haveFit, 2);
        if (!mu.msg.empty()) text(c, F.caption12, mu.msg, x, top + 118.0f, 360.0f, 16.0f, tok::inkMuted);
        x += 240.0f + 24.0f;
        // Actions: primary (Match -> Stop -> Apply -> Done) and Cancel (P-17).
        std::string primary = "Match";
        bool primaryEnabled = st.curveValid;
        if (st.fitting) primary = "Stop", primaryEnabled = true;
        else if (haveFit && !mu.applied) primary = "Apply", primaryEnabled = true;
        else if (haveFit && mu.applied) primary = "Done", primaryEnabled = true;
        const Rect pR{x, r.bottom() - 16.0f - 36.0f - 8.0f - 36.0f, 120.0f, 36.0f};
        c.setColor(!primaryEnabled ? tok::surface300 : hov(kMatchPrimary) ? tok::accentHover : tok::accent);
        c.roundedRectangle(pR.x, pR.y, pR.w, pR.h, 8.0f);
        text(c, F.semi13, primary, pR.x, pR.y, pR.w, pR.h, primaryEnabled ? tok::accentInk : tok::inkDisabled, visage::Font::kCenter);
        if (primaryEnabled) add(pR, kMatchPrimary);
        const Rect cR{x, pR.bottom() + 8.0f, 120.0f, 36.0f};
        c.setColor(hov(kMatchCancel) ? tok::surface400 : tok::surface300);
        c.roundedRectangle(cR.x, cR.y, cR.w, cR.h, 8.0f);
        text(c, F.semi13, "Cancel", cR.x, cR.y, cR.w, cR.h, tok::ink, visage::Font::kCenter);
        add(cR, kMatchCancel);
        if (!st.curveValid) {
            text(c, F.caption12, "Start a capture on both sources, or load a reference file.", 0.0f, plotRect().y + plotRect().h * 0.4f,
                 plotRect().w, 16.0f, tok::inkMuted, visage::Font::kCenter);
        }
    }
    void matchSliderDraw(visage::Canvas& c, Rect r, double n, bool enabled, int which) {
        const float cy = r.y + r.h * 0.5f;
        c.setColor(tok::surface000);
        c.roundedRectangle(r.x, cy - 2.0f, r.w, 4.0f, 2.0f);
        const float xn = r.x + static_cast<float>(std::clamp(n, 0.0, 1.0)) * r.w;
        c.setColor(enabled ? tok::accent : tok::inkDisabled);
        c.roundedRectangle(r.x, cy - 2.0f, xn - r.x, 4.0f, 2.0f);
        c.setColor(enabled ? tok::ink : tok::inkDisabled);
        c.circle(xn - 7.0f, cy - 7.0f, 14.0f);
        if (enabled) add(r, kMatchSlider, which);
        if (which == 1) amountRect_ = r;
        else bandsRect_ = r;
    }
    void matchSliderTo(float px) {
        auto& mu = *x_.match;
        const Rect r = matchSlider_ == 1 ? amountRect_ : bandsRect_;
        const double n = std::clamp((px - r.x) / std::max(1.0f, r.w), 0.0f, 1.0f);
        if (matchSlider_ == 1) mu.amount = n;
        else mu.count = static_cast<int>(std::lround(n * static_cast<double>(mu.applier.fit().size())));
        if (mu.applied) mu.applier.apply(mu.amount, mu.count, &mu.msg);  // live, merged into the apply (R13)
        x_.redrawAll();
    }

    // --- actions ---
    void onHit(const Hit& h, const visage::MouseEvent& e) {
        const int b = x_.interaction.focused();
        MatchHost* host = x_.c.matchHost();
        switch (h.kind) {
            case kToggle:
                if (h.arg == 6 && !x_.on(h.id)) x_.ops.makeSpectral({b});  // Spectral also makes the band dynamic
                else if (h.arg == 5 && x_.on(h.id)) x_.ops.clearDynamics({b});
                else x_.toggle(h.id);
                break;
            case kShape: {
                std::vector<Entry> items;
                const int cur = static_cast<int>(x_.bval(b, BandField::Shape));
                for (int s = 0; s < 10; ++s) {
                    Entry en;
                    en.id = s, en.label = std::string(params::shapeInfo(static_cast<dsp::Shape>(s)).name), en.radio = true;
                    en.checked = s == cur, en.glyph = icons::shapeGlyph(s);
                    items.push_back(en);
                }
                menuOwner_ = kShape;
                Ctx& cx = x_;
                o_.openMenu("", 176.0f, items, [&cx, b](int s) { cx.ops.setShape({b}, static_cast<dsp::Shape>(s)); }, h.r, false);
                break;
            }
            case kSlope: o_.openPopover(Pop::Slope, h.r, b); break;
            case kPlacement: {
                std::vector<Entry> items;
                const auto& places = params::specAt(params::bandParam(0, BandField::Placement))->choices;
                const int cur = static_cast<int>(x_.bval(b, BandField::Placement));
                for (int p = 0; p < static_cast<int>(places.size()); ++p) {
                    Entry en;
                    en.id = p, en.label = places[static_cast<size_t>(p)], en.radio = true, en.checked = p == cur;
                    items.push_back(en);
                }
                menuOwner_ = kPlacement;
                Ctx& cx = x_;
                o_.openMenu("", 148.0f, items, [&cx, b](int p) { cx.ops.setPlacement({b}, p); }, h.r, false);
                break;
            }
            case kScExt: x_.set(params::bandParam(b, BandField::ScExternal), h.arg); break;
            case kScFilter: o_.openPopover(Pop::ScFilter, h.r, b); break;
            case kDelete:
                x_.ops.remove({b});
                x_.interaction.clearSelection();
                break;
            case kStripExpand: x_.stripExpanded = true; break;
            case kKnob:
                if (e.repeatClickCount() >= 2) x_.resetToDefault(h.id);
                else x_.beginDrag(h.id, e.position.y);
                break;
            case kKnobValue: {
                Overlay::Field f;
                f.id = h.id, f.text = params::displayText(x_.table(), h.id);
                o_.openInline({h.r.x, h.r.y - 4.0f + x_.L.graph.y, h.r.w, h.r.h}, {f});
                break;
            }
            case kBandRow:
                listFocus_ = true;
                if (e.isShiftDown()) {
                    auto sel = x_.interaction.selection();
                    if (std::find(sel.begin(), sel.end(), h.arg) == sel.end()) sel.push_back(h.arg);
                    x_.interaction.setSelection(sel, h.arg);
                } else {
                    x_.interaction.setSelection({h.arg}, h.arg);
                }
                break;
            case kMatchRec:
                if (host) host->setRecording(h.arg, h.arg == 0 ? !x_.match->st.inputRecording : !x_.match->st.referenceRecording);
                break;
            case kMatchReset:
                if (host) host->reset(h.arg);
                break;
            case kMatchSource: {
                std::vector<Entry> items(2);
                items[0].id = 0, items[0].label = "Side-chain", items[0].radio = true, items[0].checked = !x_.match->st.referenceIsFile;
                items[1].id = 1, items[1].label = "Audio file\xE2\x80\xA6", items[1].radio = true, items[1].checked = x_.match->st.referenceIsFile;
                Ctx& cx = x_;
                o_.openMenu("", 170.0f, items, [&cx, host](int v) {
                    if (!host) return;
                    if (v == 0) host->setReferenceFromFile(false);
                    else {
                        host->setReferenceFromFile(true);
                        const std::string path = pickAudioFile();
                        std::string err;
                        if (!path.empty() && !host->loadReferenceFile(path, &err) && cx.match) cx.match->msg = err;
                    }
                }, h.r, false);
                break;
            }
            case kMatchWindow: {
                std::vector<Entry> items;
                const double opts[] = {0, 5, 10, 30, 60};
                for (int i = 0; i < 5; ++i) {
                    Entry en;
                    en.id = i, en.radio = true;
                    char bb[32];
                    std::snprintf(bb, sizeof bb, i == 0 ? "Whole capture" : "Last %.0f s", opts[i]);
                    en.label = bb;
                    en.checked = i == 0 ? !x_.match->st.lastSeconds : x_.match->st.lastSeconds && std::abs(x_.match->st.windowSeconds - opts[i]) < 0.5;
                    items.push_back(en);
                }
                menuOwner_ = kMatchWindow;
                o_.openMenu("", 220.0f, items, [host](int i) {
                    static const double o2[] = {10, 5, 10, 30, 60};
                    if (host) host->setWindow(i != 0, o2[i]);
                }, h.r, false);
                break;
            }
            case kMatchSmoothing: {
                std::vector<Entry> items;
                static const char* sm[] = {"1/3 oct", "1/6 oct", "1/12 oct", "1/24 oct"};
                for (int i = 0; i < 4; ++i) {
                    Entry en;
                    en.id = i, en.label = sm[i], en.radio = true, en.checked = i == x_.match->st.smoothing;
                    items.push_back(en);
                }
                menuOwner_ = kMatchSmoothing;
                o_.openMenu("", 170.0f, items, [host](int i) { if (host) host->setSmoothing(i); }, h.r, false);
                break;
            }
            case kMatchAlign:
                if (host) host->setLevelAlign(!x_.match->st.levelAlign);
                break;
            case kMatchSlider:
                matchSlider_ = h.arg;
                matchSliderTo(e.position.x);
                break;
            case kMatchPrimary: {
                auto& mu = *x_.match;
                if (mu.st.fitting) {
                    if (host) host->cancelFit();
                } else if (!mu.applier.fit().empty() && !mu.applied) {
                    mu.applier.apply(mu.amount, mu.count, &mu.msg);
                    mu.applied = true;
                } else if (!mu.applier.fit().empty() && mu.applied) {
                    exitMatch();
                    return;
                } else if (host && !host->startFit()) {
                    mu.msg = "Match needs both captures ready.";
                }
                break;
            }
            case kMatchCancel: exitMatch(); return;
            default: break;
        }
        if (host && x_.match) host->status(x_.match->st);
        x_.redrawAll();
    }

    void showBandMenu(float px, float py) {
        auto targets = x_.interaction.contextTargets(px, py);
        x_.redrawGraph();
        const bool clip = x_.keys.clipboard.find("\"am-q.bands\"") != std::string::npos ||
                          visage::readClipboardText().find("\"am-q.bands\"") != std::string::npos;
        std::vector<Entry> items;
        std::string caption;
        if (targets.empty()) {
            for (const auto& it : graphMenu(x_.ops, clip)) {
                Entry en;
                en.id = it.action, en.label = it.label, en.enabled = it.enabled, en.separatorBefore = it.separatorBefore;
                items.push_back(en);
            }
        } else {
            caption = bandMenuCaption(x_.ops, targets);
            static const std::map<int, std::string> glyphs = {
                {menu::InvertGain, icons::invertGain}, {menu::Cut, icons::scissors}, {menu::Copy, icons::copy},
                {menu::Paste, icons::paste}, {menu::Split, icons::split}, {menu::Delete, icons::close}};
            for (const auto& it : bandMenu(x_.ops, targets, clip)) {
                if (!it.submenu.empty()) {
                    auto parent = std::find_if(items.begin(), items.end(), [&](const Entry& en) { return en.label == it.submenu && !en.sub.empty(); });
                    if (parent == items.end()) {
                        Entry p;
                        p.label = it.submenu, p.separatorBefore = it.submenu == "Shape";
                        p.glyph = it.submenu == "Shape" ? icons::shapeGlyph(0) : it.submenu == "Slope" ? icons::slope
                                                                                                       : "M6 4 A4 4 0 1 0 6 12 A4 4 0 1 0 6 4 M10 4 A4 4 0 1 0 10 12 A4 4 0 1 0 10 4";
                        p.slopeGrid = it.submenu == "Slope";
                        items.push_back(p);
                        parent = items.end() - 1;
                    }
                    Entry s;
                    s.id = it.action, s.label = parent->slopeGrid ? fmt::slopeNumber(it.action - menu::SlopeBase) : it.label;
                    s.enabled = it.enabled, s.checked = it.checked, s.radio = true;
                    if (it.submenu == "Shape") s.glyph = icons::shapeGlyph(it.action - menu::ShapeBase);
                    parent->sub.push_back(s);
                    continue;
                }
                Entry en;
                en.id = it.action, en.label = it.label, en.hint = it.hint, en.enabled = it.enabled, en.checked = it.checked;
                en.mixed = it.mixed, en.checkbox = it.checkbox, en.separatorBefore = it.separatorBefore;
                if (it.checkbox && it.action != menu::Enable && it.action != menu::Disable)
                    en.glyph = it.label == "Make Dynamic" ? icons::makeDynamic : icons::makeSpectral;
                if (auto g = glyphs.find(it.action); g != glyphs.end()) en.glyph = g->second;
                if (en.checkbox && (en.checked || en.mixed) && !en.glyph.empty() && it.action != menu::Enable && it.action != menu::Disable) {
                    // Checked checkboxes with their own glyph show the check instead (board: Enabled ✓).
                    en.glyph.clear();
                }
                items.push_back(en);
            }
        }
        Ctx& cx = x_;
        menuOwner_ = kNone;
        o_.openMenu(caption, 196.0f, items, [&cx, targets](int id) {
            if (id == menu::SelectAll) cx.interaction.selectAll();
            else if (id == menu::ClearSelection) cx.interaction.clearSelection();
            else {
                const std::string sys = visage::readClipboardText();
                if (sys.find("\"am-q.bands\"") != std::string::npos) cx.keys.clipboard = sys;
                std::string msg;
                const std::string before = cx.keys.clipboard;
                activate(cx.ops, id, targets, cx.keys.clipboard, &msg);
                cx.keys.message = msg;
                if (cx.keys.clipboard != before) visage::setClipboardText(cx.keys.clipboard);
                if (id == menu::Delete || id == menu::DeleteAll || id == menu::DeleteDisabled || id == menu::Cut) cx.interaction.clearSelection();
            }
            cx.redrawAll();
        }, {px, py, 0.0f, 0.0f}, true);
    }

    void openNodeEditor(int b) {
        const auto n = x_.interaction.node(b);
        const auto info = params::shapeInfo(static_cast<dsp::Shape>(static_cast<int>(x_.bval(b, BandField::Shape))));
        std::vector<Overlay::Field> fields;
        auto field = [&](BandField f) {
            Overlay::Field fl;
            fl.id = params::bandParam(b, f), fl.text = params::displayText(x_.table(), fl.id);
            fields.push_back(fl);
        };
        field(BandField::Freq);
        if (info.hasGain) field(BandField::Gain);
        if (info.hasQ) field(BandField::Q);
        x_.interaction.setSelection({b}, b);
        o_.openInline({n.x + 18.0f, n.y - 44.0f + x_.L.graph.y, 0.0f, 0.0f}, fields);
    }

    Ctx& x_;
    Overlay& o_;
    std::vector<Hit> hits_;
    Hit hover_;
    visage::Point pointer_{0.0f, 0.0f};
    bool inside_ = false;
    bool listFocus_ = false;
    int menuOwner_ = kNone;
    int matchSlider_ = 0;
    std::array<float, params::kBandCount> size_ = [] {
        std::array<float, params::kBandCount> a{};
        a.fill(18.0f);
        return a;
    }();
    Rect panelRect_, bandListRect_, amountRect_, bandsRect_;
};

// ---------------------------------------------------------------------------
// Bars (panels §2.1, §2.2)
// ---------------------------------------------------------------------------

class TopBar : public visage::Frame {
public:
    TopBar(Ctx& x, Overlay& o, GraphTop& g) : x_(x), o_(o), g_(g) {}
    void draw(visage::Canvas& c) override {
        hits_.clear();
        const auto& F = fonts();
        const auto& L = x_.L;
        c.setColor(tok::surface200);
        c.fill(0, 0, width(), height());
        const float cy = height() * 0.5f;
        float x = 24.0f;
        // Wordmark: "Am-" + the Q glyph (spec §0.3), in accent.
        text(c, F.wordmark30, "Am-", x, cy - 16.0f, 80.0f, 32.0f, tok::accent);
        x += textWidth(F.wordmark30, "Am-");
        {
            const float s = 30.0f / 64.0f, gx = x, gy = cy - 15.0f + 0.2f * 30.0f - 3.0f;
            c.setColor(tok::accent);
            c.ring(gx + (28.0f - 17.5f - 5.5f) * s, gy + (28.0f - 17.5f - 5.5f) * s, (35.0f + 11.0f) * s, 11.0f * s);
            c.segment(gx + 37.0f * s, gy + 37.0f * s, gx + 42.5f * s, gy + 44.5f * s, 11.0f * s, true);
            c.circle(gx + 49.0f * s, gy + 49.0f * s, 14.0f * s);
            x += 30.0f;
        }
        if (!x_.matchMode) {
            x += 24.0f;
            separator(c, x, cy);
            x += 25.0f;
            button(c, {x, cy - 14.0f, 28.0f, 28.0f}, icons::undo, kUndo, x_.c.canUndo());
            x += 32.0f;
            button(c, {x, cy - 14.0f, 28.0f, 28.0f}, icons::redo, kRedo, x_.c.canRedo());
            x += 28.0f + 24.0f;
            if (!L.abInOverflow) {  // A/B: pending a product decision (spec C-25), drawn but inactive
                chip(c, {x, cy - 14.0f, 30.0f, 28.0f}, "A", {false, false, true});
                chip(c, {x + 34.0f, cy - 14.0f, 30.0f, 28.0f}, "B", {});
                iconButton(c, {x + 68.0f, cy - 14.0f, 28.0f, 28.0f}, icons::copyAB, {});
            }
        }
        // Centre: preset (the preset system is area 21's; name only for now).
        const std::string name = "Untitled";
        const bool edited = x_.c.canUndo();
        const float pw = std::max(L.presetMinW, textWidth(F.preset15, name) + (edited ? 8.0f + textWidth(F.caption12, "(edited)") : 0.0f) + 32.0f);
        const float px = (width() - pw) * 0.5f;
        if (!x_.matchMode) {
            if (!L.hidePrevNext) {
                iconButton(c, {px - 32.0f, cy - 14.0f, 28.0f, 28.0f}, icons::prev, {false, false, false, false});
                iconButton(c, {px + pw + 4.0f, cy - 14.0f, 28.0f, 28.0f}, icons::next, {false, false, false, false});
            }
            c.setColor(tok::surface300);
            c.roundedRectangle(px, cy - 16.0f, pw, 32.0f, 8.0f);
        }
        {
            const float nw = textWidth(F.preset15, name), ew = edited ? 8.0f + textWidth(F.caption12, "(edited)") : 0.0f;
            const float tx = px + (pw - nw - ew) * 0.5f;
            text(c, F.preset15, name, tx, cy - 10.0f, nw + 2.0f, 20.0f, tok::ink);
            if (edited) text(c, F.caption12, "(edited)", tx + nw + 8.0f, cy - 8.0f, ew, 16.0f, tok::inkMuted);
        }
        // Right cluster.
        float rx = width() - 24.0f;
        if (!x_.matchMode) {
            if (x_.c.matchHost() == nullptr) {}
            const Rect help{rx - 28.0f, cy - 14.0f, 28.0f, 28.0f};
            iconButton(c, help, "", {hov(kHelp), false, x_.hintMode});
            glyphCircle(c, help.x + 14.0f, help.y + 14.0f, 6.0f, x_.hintMode ? tok::accent : tok::ink);
            icon(c, icons::help, help.x + 6.0f, help.y + 6.0f, x_.hintMode ? tok::accent : tok::ink);
            add(help, kHelp);
            rx -= 28.0f + 8.0f;
            const Rect gear{rx - 28.0f, cy - 14.0f, 28.0f, 28.0f};
            iconButton(c, gear, icons::gear, {hov(kSettings), false, false, true, o_.kind() == Pop::Settings});
            glyphCircle(c, gear.x + 14.0f, gear.y + 14.0f, 2.2f, tok::ink);
            add(gear, kSettings);
            rx -= 28.0f + 8.0f;
        }
        if (x_.c.matchHost()) {
            const std::string label = "EQ Match";
            const float w = L.matchGlyphOnly && !x_.matchMode ? 32.0f : 12.0f + 16.0f + 8.0f + textWidth(x_.matchMode ? F.semi13 : F.label13, label) + 12.0f;
            const Rect m{rx - w, cy - 16.0f, w, 32.0f};
            if (x_.matchMode) {
                c.setColor(hov(kEqMatch) ? tok::accentHover : tok::accent);
                c.roundedRectangle(m.x, m.y, m.w, m.h, 8.0f);
                text(c, F.semi13, label, m.x, m.y, m.w, m.h, tok::accentInk, visage::Font::kCenter);
            } else {
                c.setColor(hov(kEqMatch) ? tok::surface400 : tok::surface300);
                c.roundedRectangle(m.x, m.y, m.w, m.h, 8.0f);
                icon(c, icons::matchA, m.x + (L.matchGlyphOnly ? 8.0f : 12.0f), m.y + 8.0f, tok::ink);
                c.setColor(tok::inkMuted);
                c.stroke(iconPath(icons::matchB), m.x + (L.matchGlyphOnly ? 8.0f : 12.0f), m.y + 8.0f, 16.0f, 16.0f, 1.5f,
                         visage::Path::Join::Round, visage::Path::EndCap::Round, {2.0f, 2.0f});
                if (!L.matchGlyphOnly) text(c, F.label13, label, m.x + 36.0f, m.y, m.w - 40.0f, m.h, tok::ink);
            }
            add(m, kEqMatch);
        }
        if (x_.matchMode == false && x_.L.top.w > 0 && false) {}
        // Analyzer frozen: a "Frozen" chip left of the right cluster (Analyzer board).
        if (!x_.matchMode && x_.gval(GlobalParam::AnalyzerFreeze) >= 0.5) {
            const float w = readoutWidth("Frozen");
            const Rect fr{rx - (x_.c.matchHost() ? 140.0f : 0.0f) - w - 8.0f, cy - 11.0f, w, 22.0f};
            c.setColor(tok::accent);
            c.roundedRectangle(fr.x, fr.y, fr.w, fr.h, 4.0f);
            text(c, F.semi13, "Frozen", fr.x, fr.y, fr.w, fr.h, tok::accentInk, visage::Font::kCenter);
            add(fr, kFreeze);
        }
    }
    void mouseDown(const visage::MouseEvent& e) override {
        const int h = hitAt(hits_, e.position.x, e.position.y);
        if (h < 0) return;
        const Hit hit = hits_[static_cast<size_t>(h)];
        switch (hit.kind) {
            case kUndo: x_.c.undo(); break;
            case kRedo: x_.c.redo(); break;
            case kHelp: x_.hintMode = !x_.hintMode; break;
            case kSettings: o_.openPopover(Pop::Settings, hit.r); break;
            case kEqMatch:
                if (x_.matchMode) g_.exitMatch();
                else g_.enterMatch();
                break;
            case kFreeze: x_.toggle(params::globalParam(GlobalParam::AnalyzerFreeze)); break;
            default: break;
        }
        x_.redrawAll();
    }
    // Visage sends only mouseEnter on the move that enters a frame: treat it as a move.
    void mouseEnter(const visage::MouseEvent& e) override { mouseMove(e); }
    void mouseMove(const visage::MouseEvent& e) override {
        const int h = hitAt(hits_, e.position.x, e.position.y);
        const Hit nh = h >= 0 ? hits_[static_cast<size_t>(h)] : Hit{};
        if (nh.kind != hover_.kind || nh.arg != hover_.arg) hover_ = nh, redraw();
        Rect a = nh.r;
        a.y += 0.0f;
        x_.hoverTip(h >= 0 ? tipFor(x_, nh) : std::string(), a);
    }
    void mouseExit(const visage::MouseEvent&) override {
        hover_ = {};
        x_.clearTip();
        redraw();
    }

private:
    void add(Rect r, int kind) { hits_.push_back({r, kind, 0, 0}); }
    [[nodiscard]] bool hov(int kind) const { return hover_.kind == kind; }
    void button(visage::Canvas& c, Rect r, const std::string& svg, int kind, bool enabled) {
        iconButton(c, r, svg, {hov(kind) && enabled, false, false, enabled});
        if (enabled) add(r, kind);
    }
    Ctx& x_;
    Overlay& o_;
    GraphTop& g_;
    std::vector<Hit> hits_;
    Hit hover_;
};

class StatusBar : public visage::Frame {
public:
    StatusBar(Ctx& x, Overlay& o) : x_(x), o_(o) {}
    void draw(visage::Canvas& c) override {
        hits_.clear();
        const auto& F = fonts();
        const auto& L = x_.L;
        c.setColor(tok::surface200);
        c.fill(0, 0, width(), height());
        const float cy = height() * 0.5f;
        const std::string dot = " \xC2\xB7 ";
        if (x_.matchMode) {
            const auto* mu = x_.match.get();
            if (mu && mu->st.fitting) {
                text(c, F.label13, "Fitting bands", 24.0f, 0.0f, 100.0f, height(), tok::ink);
                c.setColor(tok::surface000);
                c.roundedRectangle(124.0f, cy - 3.0f, 160.0f, 6.0f, 3.0f);
                c.setColor(tok::accent);
                c.roundedRectangle(124.0f, cy - 3.0f, 160.0f * mu->st.fitProgress, 6.0f, 3.0f);
                text(c, F.label13, fmt::pct(100.0 * mu->st.fitProgress), 296.0f, 0.0f, 60.0f, height(), tok::inkMuted);
            } else {
                const bool ready = mu && mu->st.curveValid;
                text(c, F.label13, ready ? "Ready to match" : "Waiting for both captures", 24.0f, 0.0f, 300.0f, height(),
                     ready ? tok::ink : tok::inkMuted);
            }
            text(c, F.caption12, "Power average" + dot + "frames below " + std::string(fmt::kMinus) + "90 dBFS skipped" + dot + "Apply = one undo step",
                 0.0f, 0.0f, width() - 24.0f, height(), tok::inkMuted, visage::Font::kRight);
            return;
        }
        if (x_.hintMode) {  // hint mode (spec §4.13, §6.4)
            const int n = static_cast<int>(x_.interaction.selection().size());
            std::string left = n > 1 ? std::to_string(n) + " bands selected" : "Double-click to add a band";
            std::string rest = n > 1 ? dot + (n == 2 ? "drag moves both" : "drag moves all") + dot + "Shift: fine 0.1\xC3\x97"
                                     : dot + "Right-click for the band menu";
            text(c, F.semi12, left, 24.0f, 0.0f, textWidth(F.semi12, left) + 2.0f, height(), tok::ink);
            text(c, F.caption12, rest, 24.0f + textWidth(F.semi12, left), 0.0f, 400.0f, height(), tok::inkMuted);
            text(c, F.caption12,
                 "Wheel: Q" + dot + "Ctrl: gain" + dot + "Alt: range" + dot + "Ctrl+Alt: slope" + dot + "Alt-click: enable" + dot +
                     "Del/\xE2\x8C\xAB: delete",
                 0.0f, 0.0f, width() - 24.0f, height(), tok::inkMuted, visage::Font::kRight);
            return;
        }
        // Left group.
        float x = 24.0f;
        const std::string used = std::to_string(x_.usedCount()) + " / 24";
        float w = selectWidth("", "Bands", used, 0);
        sel(c, {x, cy - 14.0f, w, 28.0f}, "Bands", used, 0, kBands, x_.bandListOpen, false);
        x += w + 16.0f;
        separator(c, x, cy);
        x += 17.0f;
        const int mode = std::clamp(static_cast<int>(x_.gval(GlobalParam::Mode)), 0, 2);
        const std::string lat = L.hideLatencyText ? "" : fmt::latency(x_.latency(mode, static_cast<int>(x_.gval(GlobalParam::Resolution))));
        w = selectWidth("", kModeNames[mode], lat, -1);
        sel(c, {x, cy - 14.0f, w, 28.0f}, kModeNames[mode], lat, -1, kProcessing, o_.kind() == Pop::Processing, false);
        x += w + 16.0f;
        const std::string summary = x_.analyzerSummary();
        const std::string cap = L.hideAnalyzerCaption ? "" : "Analyzer";
        w = selectWidth("", summary, cap, 0);
        sel(c, {x, cy - 14.0f, w, 28.0f}, summary, cap, 0, kAnalyzer, o_.kind() == Pop::Analyzer || o_.kind() == Pop::Settings, true);
        x += w + 16.0f;
        const auto frz = params::globalParam(GlobalParam::AnalyzerFreeze);
        const Rect fr{x, cy - 14.0f, 28.0f, 28.0f};
        iconButton(c, fr, icons::freeze, {hov(kToggle, static_cast<int>(frz)), false, x_.on(frz)});
        hits_.push_back({fr, kToggle, static_cast<int>(frz), frz});
        // Right group, laid out from the right edge.
        float rx = width() - 24.0f;
        auto gid = [](GlobalParam g) { return params::globalParam(g); };
        auto iconToggle = [&](GlobalParam g, const std::string& svg, bool circle) {
            const Rect r{rx - 28.0f, cy - 14.0f, 28.0f, 28.0f};
            const auto id = gid(g);
            iconButton(c, r, svg, {hov(kToggle, static_cast<int>(id)), false, x_.on(id)});
            if (circle) glyphCircle(c, r.x + 14.0f, r.y + 14.0f, 4.5f, x_.on(id) ? tok::accent : tok::ink);
            hits_.push_back({r, kToggle, static_cast<int>(id), id});
            rx -= 28.0f + 16.0f;
        };
        iconToggle(GlobalParam::Bypass, icons::power, false);
        if (!L.hideOutputCaption) iconToggle(GlobalParam::Invert, icons::invertSlash, true);
        {
            const auto id = gid(GlobalParam::OutputLevel);
            const std::string t = fmt::outputDb(x_.gval(GlobalParam::OutputLevel), params::kOutputSilenceDb);
            const float rw = readoutWidth(t);
            const Rect r{rx - rw, cy - 11.0f, rw, 22.0f};
            readout(c, r, t, true, hov(kDragValue, static_cast<int>(id)));
            hits_.push_back({r, kDragValue, static_cast<int>(id), id});
            rx -= rw + 16.0f;
        }
        {
            const std::string t = x_.gval(GlobalParam::PanMode) >= 0.5 ? "M/S" : "L/R";
            const float cw = chipWidth(t);
            const Rect r{rx - cw, cy - 14.0f, cw, 28.0f};
            chip(c, r, t, {hov(kOutputMenu), false, false, true, o_.kind() == Pop::Output});
            hits_.push_back({r, kOutputMenu, 0, 0});
            rx -= cw + 16.0f;
        }
        if (!L.hideOutputCaption) {
            text(c, F.label13, "Output", rx - textWidth(F.label13, "Output"), 0.0f, textWidth(F.label13, "Output") + 2.0f, height(), tok::inkMuted);
            rx -= textWidth(F.label13, "Output") + 16.0f;
        }
        if (!L.hideGainScale) {
            separator(c, rx, cy);
            rx -= 17.0f;
            const auto id = gid(GlobalParam::GainScale);
            const std::string v = fmt::pct(x_.gval(GlobalParam::GainScale));
            const float vw = textWidth(F.label13, v);
            text(c, F.label13, v, rx - vw, 0.0f, vw + 2.0f, height(), hov(kDragValue, static_cast<int>(id)) ? tok::accentHover : tok::ink);
            hits_.push_back({{rx - vw, cy - 10.0f, vw, 20.0f}, kDragValue, static_cast<int>(id), id});
            rx -= vw + 8.0f;
            const float lw = textWidth(F.label13, "Gain Scale");
            text(c, F.label13, "Gain Scale", rx - lw, 0.0f, lw + 2.0f, height(), tok::inkMuted);
            rx -= lw + 16.0f;
        }
        if (!L.hideAutoGain) {
            const auto id = gid(GlobalParam::AutoGain);
            const float cw = chipWidth("Auto Gain");
            const Rect r{rx - cw, cy - 14.0f, cw, 28.0f};
            chip(c, r, "Auto Gain", {hov(kToggle, static_cast<int>(id)), false, x_.on(id)});
            hits_.push_back({r, kToggle, static_cast<int>(id), id});
        }
    }
    void mouseDown(const visage::MouseEvent& e) override {
        const int h = hitAt(hits_, e.position.x, e.position.y);
        if (h < 0) return;
        const Hit hit = hits_[static_cast<size_t>(h)];
        // Popovers anchor to the control in window coordinates (the bar is at the bottom).
        Rect anchor = hit.r;
        anchor.y += x_.L.status.y;
        switch (hit.kind) {
            case kBands: x_.bandListOpen = !x_.bandListOpen; break;
            case kProcessing:
                if (o_.kind() == Pop::Processing) o_.close();
                else o_.openPopover(Pop::Processing, anchor);
                break;
            case kAnalyzer:
                if (o_.kind() == Pop::Analyzer) o_.close();
                else o_.openPopover(Pop::Analyzer, anchor);
                break;
            case kOutputMenu:
                if (o_.kind() == Pop::Output) o_.close();
                else o_.openPopover(Pop::Output, {anchor.x, anchor.y, x_.L.width - 24.0f - anchor.x, anchor.h});
                break;
            case kToggle: x_.toggle(hit.id); break;
            case kDragValue:
                if (e.repeatClickCount() >= 2) x_.resetToDefault(hit.id);
                else x_.beginDrag(hit.id, e.position.y), dragging_ = true;
                break;
            default: break;
        }
        x_.redrawAll();
    }
    void mouseDrag(const visage::MouseEvent& e) override {
        if (!dragging_) return;
        x_.dragTo(e.position.y, e.isShiftDown());
        redraw();
    }
    void mouseUp(const visage::MouseEvent&) override {
        if (dragging_) x_.endDrag();
        dragging_ = false;
    }
    // Visage sends only mouseEnter on the move that enters a frame: treat it as a move.
    void mouseEnter(const visage::MouseEvent& e) override { mouseMove(e); }
    void mouseMove(const visage::MouseEvent& e) override {
        const int h = hitAt(hits_, e.position.x, e.position.y);
        const Hit nh = h >= 0 ? hits_[static_cast<size_t>(h)] : Hit{};
        if (nh.kind != hover_.kind || nh.arg != hover_.arg) hover_ = nh, redraw();
        Rect a = nh.r;
        a.y += x_.L.status.y;
        x_.hoverTip(h >= 0 ? tipFor(x_, nh) : std::string(), a);
    }
    void mouseExit(const visage::MouseEvent&) override {
        hover_ = {};
        x_.clearTip();
        redraw();
    }

private:
    [[nodiscard]] bool hov(int kind, int arg = 0) const { return hover_.kind == kind && hover_.arg == arg; }
    void sel(visage::Canvas& c, Rect r, const std::string& t, const std::string& muted, int chev, int kind, bool open, bool mutedFirst) {
        select(c, r, "", t, muted, chev, {hov(kind), false, false, true, open}, mutedFirst);
        hits_.push_back({r, kind, 0, 0});
    }
    Ctx& x_;
    Overlay& o_;
    std::vector<Hit> hits_;
    Hit hover_;
    bool dragging_ = false;
};

// Tooltip (spec §4.0 layer 9): a readout chip under the hovered control, after 600 ms.
class TipFrame : public visage::Frame {
public:
    explicit TipFrame(Ctx& x) : x_(x) {
        setIgnoresMouseEvents(true, false);
        setVisible(false);
    }
    void draw(visage::Canvas& c) override {
        if (!x_.tipShown || x_.tipText.empty()) return;
        const auto& F = fonts();
        const float w = textWidth(F.label13, x_.tipText) + 16.0f, h = 24.0f;
        float tx = x_.tipAnchor.x, ty = x_.tipAnchor.bottom() + 6.0f;
        if (ty + h > height() - 4.0f) ty = x_.tipAnchor.y - 6.0f - h;
        tx = std::clamp(tx, 4.0f, std::max(4.0f, width() - 4.0f - w));
        c.setColor(tok::lineHairline);
        c.roundedRectangle(tx - 1.0f, ty - 1.0f, w + 2.0f, h + 2.0f, 5.0f);
        c.setColor(tok::surface300);
        c.roundedRectangle(tx, ty, w, h, 4.0f);
        text(c, F.label13, x_.tipText, tx + 8.0f, ty, w - 12.0f, h, tok::ink);
    }

private:
    Ctx& x_;
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
        const auto f = params::bandParam(band_, BandField::Freq);
        const auto g = params::bandParam(band_, BandField::Gain);
        return params::formatPlain(*params::specAt(f), c_.table().value(f)) + ", " +
               params::formatPlain(*params::specAt(g), c_.table().value(g));
    }
    double accessibleValueNormalised() const override {
        const auto f = params::bandParam(band_, BandField::Freq);
        return params::toNormalised(*params::specAt(f), c_.table().value(f));
    }
    std::vector<AccessibleAction> accessibleActions() const override {
        return {AccessibleAction::Press, AccessibleAction::Increment, AccessibleAction::Decrement, AccessibleAction::SetValue};
    }
    // Press selects; Increment/Decrement nudge gain by 1 dB (PROVISIONAL step); SetValue sets
    // the normalised frequency. All through gestures.
    bool performAccessibleAction(AccessibleAction a, double value) override {
        if (!accessibleVisible()) return false;
        const auto g = params::bandParam(band_, BandField::Gain);
        const auto f = params::bandParam(band_, BandField::Freq);
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
    Ctx x;
    visage::ApplicationWindow app;
    visage::EventTimer pollTimer;
    Overlay overlay;
    TipFrame tip;
    visage::EventTimer animTimer;
    double lastAnim = 0.0;
    bool animating = false;
    visage::Frame graphRegion;
    GroundFrame ground;
    SpectrumFrame spectrum;
    CurvesFrame curves;
    MeterFrame meters;
    GraphTop top;
    TopBar topBar;
    StatusBar statusBar;
    std::array<std::unique_ptr<BandNodeAccessible>, params::kBandCount> nodeAccess;
    GraphAccessible graphAccess;
    RootAccessible rootAccess;

    explicit Impl(UndoController& ctl)
        : x(ctl),
          overlay(x),
          tip(x),
          ground(x),
          spectrum(x),
          curves(x),
          meters(x),
          top(x, overlay),
          topBar(x, overlay, top),
          statusBar(x, overlay),
          graphAccess(nodeAccess),
          rootAccess(graphAccess) {
        for (int b = 0; b < params::kBandCount; ++b)
            nodeAccess[static_cast<size_t>(b)] = std::make_unique<BandNodeAccessible>(ctl, x.interaction, b);
        x.keys.writeSystemClipboard = [](const std::string& s) { visage::setClipboardText(s); };
        x.keys.readSystemClipboard = [] { return visage::readClipboardText(); };
        x.redrawGraph = [this] {
            x.kickAnimation();
            curves.redraw();
            top.redraw();
            statusBar.redraw();
        };
        x.redrawBars = [this] {
            topBar.redraw();
            statusBar.redraw();
        };
        x.redrawAll = [this] {
            if (x.kickAnimation) x.kickAnimation();
            const auto before = x.L.panel.rect;
            const auto mode = x.L.panel.mode;
            computeLayout();
            if (before.w != x.L.panel.rect.w || before.h != x.L.panel.rect.h || mode != x.L.panel.mode) relayoutCurve();
            ground.redraw();
            spectrum.redraw();
            curves.redraw();
            meters.redraw();
            top.redraw();
            topBar.redraw();
            statusBar.redraw();
            overlay.redraw();
        };
        x.relayout = [this] { layout(); };
        // Motion (spec §7): one timer drives every transition while any is moving; reduced
        // motion jumps straight to the end.
        x.kickAnimation = [this] {
            if (animating) return;
            animating = true;
            lastAnim = theme::nowMs();
            animTimer.startTimer(8);
        };
        animTimer.onTimerCallback() += [this] {
            const double now = theme::nowMs();
            const double dt = theme::reducedMotion() ? 1e6 : now - lastAnim;
            lastAnim = now;
            bool moving = overlay.animate(dt);
            moving = top.animate(dt) || moving;
            moving = curves.animate(dt) || moving;
            if (!moving) {
                animTimer.stopTimer();
                animating = false;
            }
        };
        x.tipChanged = [this] {
            tip.setVisible(false);
            tip.redraw();
        };
        x.interaction.onSelectionChanged = [this] {
            x.stripExpanded = false;
            x.redrawAll();
        };
        top.onViewChanged = [this] { relayoutCurve(); };
        x.c.addListener(this);
        pollTimer.onTimerCallback() += [this] {
            const unsigned changed = x.poller.poll(x.c);
            if (changed & SnapshotPoller::kSpectrum) spectrum.redraw();
            if (changed & SnapshotPoller::kLevels) meters.redraw();
            if (changed & SnapshotPoller::kGainChange) {
                curves.redraw();
                top.redraw();
            }
            top.pollMatch();  // EQ Match status and fits, only while EQ Match mode is on
            if (!x.tipShown && !x.tipText.empty() && theme::nowMs() - x.tipSince >= 600.0 && overlay.kind() == Pop::None) {
                x.tipShown = true;
                tip.setVisible(true);
                tip.redraw();
            }
            checkShotRequest();
        };
        // Design-review screenshots (tests/plugin/plugin_gui_shot.cpp): when AMQ_GUI_SHOT names a
        // trigger file, a PNG path written to it is filled with the editor's rendered frame. Unset
        // in normal use, so nothing is checked.
        char* env = nullptr;
        size_t len = 0;
#if defined(_WIN32)
        if (_dupenv_s(&env, &len, "AMQ_GUI_SHOT") == 0 && env) shotTrigger = env;
        std::free(env);
#else
        if (const char* e = std::getenv("AMQ_GUI_SHOT")) shotTrigger = e;
        (void)env, (void)len;
#endif
    }
    ~Impl() override {
        pollTimer.stopTimer();
        animTimer.stopTimer();
        x.c.removeListener(this);
    }

    std::string shotTrigger;
    void checkShotRequest() {
        if (shotTrigger.empty()) return;
        std::FILE* f = nullptr;
#if defined(_WIN32)
        if (fopen_s(&f, shotTrigger.c_str(), "rb") != 0) f = nullptr;
#else
        f = std::fopen(shotTrigger.c_str(), "rb");
#endif
        if (!f) return;
        char buf[1024] = {};
        const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
        std::fclose(f);
        std::remove(shotTrigger.c_str());
        std::string path(buf, n);
        while (!path.empty() && static_cast<unsigned char>(path.back()) <= 32) path.pop_back();
        if (!path.empty()) app.takeScreenshot().save(path);
    }

    Editor* owner = nullptr;
    double lastScale = 0.0;            // host-unit scale at the last layout
    float wantW = 0.0f, wantH = 0.0f;  // logical size to keep across DPI changes
    bool dpiResizePending = false;     // asked the host to resize after a DPI change

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

    // The curve's frequency grid follows the zoomed span and the plot width.
    void relayoutCurve() {
        std::vector<double> hz;
        const auto& g = x.geometry;
        const int n = std::max(64, static_cast<int>((g.width() - GraphGeometry::kRight) / 2.0f));
        for (int i = 0; i <= n; ++i) hz.push_back(g.viewLo() * std::pow(g.viewHi() / g.viewLo(), i / double(n)));
        x.curve.setFrequencies(std::move(hz));
        ground.redraw();
        spectrum.redraw();
        curves.redraw();
        top.redraw();
    }

    void computeLayout() {
        const float w = app.width() > 0 ? app.width() : static_cast<float>(kDefaultWidth);
        const float h = app.height() > 0 ? app.height() : static_cast<float>(kDefaultHeight);
        x.L = EditorLayout::compute(w, h, x.panelContent());
    }

    void layout() {
        checkDpiChange();
        computeLayout();
        const auto& L = x.L;
        if (!dpiResizePending) {
            wantW = L.width;
            wantH = L.height;
        }
        x.geometry.setSize(L.graph.w, L.graph.h);
        x.geometry.setRangeDb(x.displayRange());
        topBar.setBounds(L.top.x, L.top.y, L.top.w, L.top.h);
        statusBar.setBounds(L.status.x, L.status.y, L.status.w, L.status.h);
        graphRegion.setBounds(L.graph.x, L.graph.y, L.graph.w, L.graph.h);
        ground.setBounds(0, 0, L.graph.w, L.graph.h);
        spectrum.setBounds(0, 0, L.graph.w, L.graph.h);
        curves.setBounds(0, 0, L.graph.w, L.graph.h);
        meters.setBounds(L.graph.w - GraphGeometry::kRight, 0, GraphGeometry::kRight, L.graph.h);
        top.setBounds(0, 0, L.graph.w, L.graph.h);
        overlay.setBounds(0, 0, L.width, L.height);
        tip.setBounds(0, 0, L.width, L.height);
        relayoutCurve();
        topBar.redraw();
        statusBar.redraw();
        meters.redraw();
        if (owner && owner->onSizeChanged) {
            uint32_t hw = 0, hh = 0;
            hostSize(&hw, &hh);
            owner->onSizeChanged(hw, hh, hostScale());
        }
    }

    // Host, preset or GUI changes: redraw only what depends on them (area 12 R7, area 13 R22).
    void parameterChanged(params::ParamIndex i, double, params::ParamSource) override {
        if (i < static_cast<params::ParamIndex>(params::kBandCount * params::kBandFieldCount)) {
            x.curve.markDirty(static_cast<int>(i) / params::kBandFieldCount);
            const auto before = x.L.panel.rect;
            const auto mode = x.L.panel.mode;
            computeLayout();
            if (before.w != x.L.panel.rect.w || before.h != x.L.panel.rect.h || mode != x.L.panel.mode) top.redraw();
            curves.redraw();
            top.redraw();
            topBar.redraw();
            statusBar.redraw();
            overlay.redraw();
            return;
        }
        // Display settings redraw only their own layer (area 13 "redraw only on change").
        if (i == params::globalParam(GlobalParam::DisplayRange)) {
            x.geometry.setRangeDb(x.displayRange());
            ground.redraw();
            curves.redraw();
            top.redraw();
        } else if (i == params::globalParam(GlobalParam::GainScale)) {
            x.curve.markAllDirty();
            curves.redraw();
            statusBar.redraw();
        } else if ((i >= params::globalParam(GlobalParam::AnalyzerShowPre) && i <= params::globalParam(GlobalParam::AnalyzerGrab)) ||
                   i == params::globalParam(GlobalParam::AnalyzerPeakHold)) {
            spectrum.redraw();
            ground.redraw();
            statusBar.redraw();
            topBar.redraw();
        } else {
            statusBar.redraw();
            topBar.redraw();
            top.redraw();
        }
        overlay.redraw();
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
    auto& m = *impl_;
    app.addChild(&m.graphRegion);
    m.graphRegion.addChild(&m.ground);
    m.graphRegion.addChild(&m.spectrum);
    m.graphRegion.addChild(&m.curves);
    m.graphRegion.addChild(&m.meters);
    m.graphRegion.addChild(&m.top);
    app.addChild(&m.topBar);
    app.addChild(&m.statusBar);
    app.addChild(&m.overlay);
    m.overlay.setOnTop(true);
    app.addChild(&m.tip);
    m.tip.setOnTop(true);
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
