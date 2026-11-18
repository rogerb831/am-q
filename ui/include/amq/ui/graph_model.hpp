#pragma once
// Framework-free model of the response graph (areas 12, 13): geometry, the response curve and
// pointer interaction. The Visage frames in editor.cpp only draw it and forward input, so all
// of the behaviour here is unit-tested without a window.
//
// Interaction scope (Phase 1): the generic EQ gestures needed to use the plugin.
//   - Double-click an empty spot: create a band there, with all fields at their defaults, in
//     one gesture (area 13 R3, R19; area 21 R8).
//   - Press a node: select it. Dragging beyond the threshold moves frequency (x) and gain (y),
//     or Q (y) for shapes without gain (R2, R9).
//   - Plain wheel over a node, or over the graph with a selection: Q (R15).
//   - Alt-click a node: toggle Enabled (R7).
//   - Click an empty spot: clear the selection.
// The modifier table, fine factor, lasso and multi-select belong to the Product designer's
// interaction spec (area 13 Phase 0 task) and arrive with it.

#include "amq/ui/edit_gesture.hpp"

#include <array>
#include <functional>
#include <vector>

namespace amq::ui {

// Platform-neutral modifiers (area 13 R1): command is Ctrl on Windows and Cmd on macOS.
struct Modifiers {
    bool shift = false, alt = false, command = false;
};
enum class Button : uint8_t { Left, Right, Middle };

class GraphGeometry {
public:
    // Full displayed span = the band frequency range (editor-spec-graph §5.1).
    static constexpr double kMinHz = 10.0, kMaxHz = 30000.0;
    // Plot insets inside the graph region (editor-spec §1.1): the plot runs to the left edge; the
    // right gutter holds the gain and analyzer scales and the meters.
    static constexpr float kLeft = 0.0f, kRight = 100.0f, kTop = 24.0f, kBottom = 24.0f;

    void setSize(float w, float h) noexcept { w_ = w, h_ = h; }
    // Horizontal zoom (area 13 wheel bindings): the displayed span, within kMinHz .. kMaxHz.
    void setView(double loHz, double hiHz) noexcept;
    void zoomAt(float x, double factor) noexcept;  // factor < 1 zooms in, around the pointer
    [[nodiscard]] double viewLo() const noexcept { return lo_; }
    [[nodiscard]] double viewHi() const noexcept { return hi_; }
    [[nodiscard]] double octavesPerPixel() const noexcept;
    void setRangeDb(double r) noexcept { rangeDb_ = r > 0.0 ? r : 12.0; }
    [[nodiscard]] float width() const noexcept { return w_; }
    [[nodiscard]] float height() const noexcept { return h_; }
    [[nodiscard]] double rangeDb() const noexcept { return rangeDb_; }

    [[nodiscard]] float xOfHz(double hz) const noexcept;
    [[nodiscard]] double hzOfX(float x) const noexcept;
    [[nodiscard]] float yOfDb(double db) const noexcept;
    [[nodiscard]] double dbOfY(float y) const noexcept;

private:
    float w_ = 1280.0f, h_ = 632.0f;
    double rangeDb_ = 12.0;
    double lo_ = kMinHz, hi_ = kMaxHz;
};

// Response curve in dB at fixed frequencies, computed from parameter values with the analog
// prototypes while a band moves, and replaced by the band's exact digital response once it settles
// (setSettled). Only bands marked dirty are recomputed (area 12 R7).
// PROVISIONAL: one combined curve over all placements; per-placement curves and the exact
// shared GUI/DSP curve code are area 12's Phase 2 task.
class ResponseCurve {
public:
    void setFrequencies(std::vector<double> hz);
    [[nodiscard]] const std::vector<double>& frequencies() const noexcept { return hz_; }
    void markDirty(int band) noexcept {
        dirty_[static_cast<size_t>(band)] = true;
        settled_[static_cast<size_t>(band)] = false;  // back to the fast analog curve until it settles again
    }
    void markAllDirty() noexcept {
        dirty_.fill(true);
        settled_.fill(false);
    }
    // The exact digital response of a band that has stopped moving (area 12 R12: the refined design
    // the audio settles on, from engine::bandMagnitudeDb), at frequencies(). It replaces the analog
    // curve until the band changes. Ignored if the sizes differ or the band is not audible.
    void setSettled(int band, const std::vector<double>& db);
    [[nodiscard]] bool settled(int band) const noexcept { return settled_[static_cast<size_t>(band)]; }
    // Recomputes dirty bands; returns true if the curve changed.
    bool update(const params::ParameterTable& t);
    [[nodiscard]] const std::vector<double>& combinedDb() const noexcept { return combined_; }
    [[nodiscard]] bool bandAudible(int band) const noexcept { return audible_[static_cast<size_t>(band)]; }
    [[nodiscard]] const std::vector<double>& bandDb(int band) const noexcept { return band_[static_cast<size_t>(band)]; }

private:
    std::vector<double> hz_;
    std::array<std::vector<double>, params::kBandCount> band_;
    std::array<bool, params::kBandCount> audible_{};
    std::array<bool, params::kBandCount> dirty_{};
    std::array<bool, params::kBandCount> settled_{};
    bool recombine_ = false;
    std::vector<double> combined_;
    double lastScale_ = -1.0;
};

struct NodeInfo {
    bool used = false, enabled = false, hasGain = true;
    bool selected = false, focused = false, dynamic = false, soloed = false;
    double hz = 1000.0, gainDb = 0.0, rangeDb = 0.0;
    float x = 0.0f, y = 0.0f;
    bool hasRangeHandle = false;  // a dynamic gain band: the handle sits at gain + range
    float rangeY = 0.0f;
};

// Pointer and wheel interaction (area 13), per the modifier table in docs/design/editor-spec.md §6
// (counsel review marked complete 2026-11-18):
//   click a node: select it (and focus it); Shift+click: add; Command+click: toggle
//   click empty: clear the selection (Shift keeps it); drag on empty: lasso (Shift adds)
//   drag a node: moves every selected band by the same octaves and dB (gainless shapes: Q);
//     Shift: fine (x0.1); Command held during the drag: lock to the first dominant axis (at the
//     press Command toggles the selection); Alt: vertical moves Q
//   a soloed band's vertical drag sets Solo Gain (R11; the editor shows soloDragDb())
//   range handle (dynamic gain bands, at gain + range): drag sets Dyn Range; Alt-click toggles
//     Dynamics Enabled (R7); Alt-click a node toggles Enabled
//   wheel over a node or the selection: Q; Command: gain; Alt: Dyn Range; Command+Alt: slope
//     (steps through the slopes the shape allows); Shift: fine (x0.1, not for slope); over empty
//     space with no selection: horizontal zoom (editor-spec §6.2)
//   optional Gain-Q coupling (off by default, area 13 Phase 3): dragging gain narrows Q by
//     2^(change in |gain| / 12)
// Every change is one gesture (one undo step) across all affected bands.
class GraphInteraction {
public:
    static constexpr float kDragThresholdPx = 3.0f;  // area 13 R2; editor spec §6.1, approved 2026-11-18
    static constexpr float kNodeRadiusPx = 11.0f;
    static constexpr float kHandleRadiusPx = 7.0f;
    static constexpr double kWheelQStep = 0.02;      // Q position per wheel unit (spec §6.2)
    static constexpr double kWheelGainDb = 0.5;      // dB per wheel unit (gain and range; spec §6.2)
    static constexpr double kDragQPerPx = 0.004;     // Q position per pixel for gainless shapes
    static constexpr double kFineFactor = 0.1;       // Shift: fine (spec §6.1)
    static constexpr double kZoomPerWheel = 0.85;    // span factor per wheel unit (spec §6.2)

    GraphInteraction(EditController& c, GraphGeometry& g) : c_(c), g_(g) {}

    [[nodiscard]] NodeInfo node(int band) const;
    [[nodiscard]] int hitTest(float x, float y) const;  // band index or -1; topmost (highest index) wins
    [[nodiscard]] int hitRangeHandle(float x, float y) const;
    // Focus: the band last clicked (the band panel shows it); -1 for none.
    [[nodiscard]] int selected() const noexcept { return focused_; }
    [[nodiscard]] int focused() const noexcept { return focused_; }
    [[nodiscard]] const std::vector<int>& selection() const noexcept { return selection_; }
    [[nodiscard]] bool isSelected(int band) const noexcept;
    [[nodiscard]] bool dragging() const noexcept { return gesture_.isOpen(); }
    [[nodiscard]] bool lassoActive() const noexcept { return mode_ == Mode::Lasso; }
    [[nodiscard]] std::array<float, 4> lassoRect() const noexcept;  // x0, y0, x1, y1
    [[nodiscard]] bool soloDragActive() const noexcept { return mode_ == Mode::Solo && gesture_.isOpen(); }
    [[nodiscard]] double soloDragDb() const;

    void setSelection(std::vector<int> bands, int focus = -2);
    void selectAll();
    void clearSelection();
    void focusNext(int direction);  // Tab / Shift+Tab over the used bands (area 15)
    // Right-click: the bands a menu acts on (a node outside the selection becomes the selection).
    std::vector<int> contextTargets(float x, float y);
    // Ends a drag without keeping it: the values return to where the drag began (Escape).
    void cancelDrag();

    void pointerDown(float x, float y, Button b, Modifiers m, int clickCount);
    void pointerDrag(float x, float y, Modifiers m);
    void pointerUp(float x, float y, Modifiers m);
    void wheel(float x, float y, double delta, Modifiers m);

    // Creates a band at a graph position; returns its index or -1 if all 24 are in use.
    int createBandAt(float x, float y);

    bool gainQCoupling = false;  // area 13 Phase 3 option, off by default
    std::function<void()> onSelectionChanged;

private:
    enum class Mode : uint8_t { None, Pending, Nodes, Range, Lasso, Solo };
    struct Item {
        int band = 0;
        double f0 = 0.0, v0 = 0.0, q0 = 0.0, g0 = 0.0;
        bool hasGain = true;
    };
    void changed();
    void beginDrag(Modifiers m);
    [[nodiscard]] double value(params::ParamIndex i) const { return c_.table().value(i); }
    [[nodiscard]] bool soloed(int band) const;

    EditController& c_;
    GraphGeometry& g_;
    std::vector<int> selection_;
    int focused_ = -1;
    Mode mode_ = Mode::None;
    Mode pendingMode_ = Mode::None;
    int pressed_ = -1;
    bool pendingAltQ_ = false;
    bool clickSelectsOnly_ = false;  // a plain click inside a multi-selection reduces it on release
    int axis_ = -1;                  // Command-drag: 0 x, 1 y
    float downX_ = 0, downY_ = 0, curX_ = 0, curY_ = 0;
    double solo0_ = 0.0, range0_ = 0.0;
    bool lassoAdd_ = false;
    std::vector<Item> items_;
    std::vector<std::pair<params::ParamIndex, double>> undoValues_;  // for cancelDrag
    EditGesture gesture_;
};

}  // namespace amq::ui
