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
    static constexpr double kMinHz = 20.0, kMaxHz = 20000.0;  // displayed span
    // PROVISIONAL plot margins (Product designer): room for the axis labels.
    static constexpr float kLeft = 16.0f, kRight = 56.0f, kTop = 16.0f, kBottom = 32.0f;

    void setSize(float w, float h) noexcept { w_ = w, h_ = h; }
    void setRangeDb(double r) noexcept { rangeDb_ = r > 0.0 ? r : 12.0; }
    [[nodiscard]] float width() const noexcept { return w_; }
    [[nodiscard]] float height() const noexcept { return h_; }
    [[nodiscard]] double rangeDb() const noexcept { return rangeDb_; }

    [[nodiscard]] float xOfHz(double hz) const noexcept;
    [[nodiscard]] double hzOfX(float x) const noexcept;
    [[nodiscard]] float yOfDb(double db) const noexcept;
    [[nodiscard]] double dbOfY(float y) const noexcept;

private:
    float w_ = 1180.0f, h_ = 672.0f;
    double rangeDb_ = 12.0;
};

// Response curve in dB at fixed frequencies, computed from parameter values with the analog
// prototypes. A settled band's audio is within R12 of these. Only bands marked dirty are
// recomputed (area 12 R7).
// PROVISIONAL: one combined curve over all placements; per-placement curves and the exact
// shared GUI/DSP curve code are area 12's Phase 2 task.
class ResponseCurve {
public:
    void setFrequencies(std::vector<double> hz);
    [[nodiscard]] const std::vector<double>& frequencies() const noexcept { return hz_; }
    void markDirty(int band) noexcept { dirty_[static_cast<size_t>(band)] = true; }
    void markAllDirty() noexcept { dirty_.fill(true); }
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
    std::vector<double> combined_;
    double lastScale_ = -1.0;
};

struct NodeInfo {
    bool used = false, enabled = false, hasGain = true;
    double hz = 1000.0, gainDb = 0.0;
    float x = 0.0f, y = 0.0f;
};

class GraphInteraction {
public:
    static constexpr float kDragThresholdPx = 3.0f;  // PROVISIONAL (area 13 R2; Product designer)
    static constexpr float kNodeRadiusPx = 11.0f;
    static constexpr double kWheelQStep = 0.02;      // Q position per wheel unit, PROVISIONAL
    static constexpr double kDragQPerPx = 0.004;     // Q position per pixel for gainless shapes

    GraphInteraction(EditController& c, const GraphGeometry& g) : c_(c), g_(g) {}

    [[nodiscard]] NodeInfo node(int band) const;
    [[nodiscard]] int hitTest(float x, float y) const;  // band index or -1; topmost (highest index) wins
    [[nodiscard]] int selected() const noexcept { return selected_; }
    [[nodiscard]] bool dragging() const noexcept { return gesture_.isOpen(); }

    void pointerDown(float x, float y, Button b, Modifiers m, int clickCount);
    void pointerDrag(float x, float y, Modifiers m);
    void pointerUp(float x, float y, Modifiers m);
    void wheel(float x, float y, double delta, Modifiers m);

    // Creates a band at a graph position; returns its index or -1 if all 24 are in use.
    int createBandAt(float x, float y);

    std::function<void()> onSelectionChanged;

private:
    void select(int band);
    [[nodiscard]] double value(params::ParamIndex i) const { return c_.table().value(i); }

    EditController& c_;
    const GraphGeometry& g_;
    int selected_ = -1;
    int pressed_ = -1;
    bool pending_ = false;
    float downX_ = 0, downY_ = 0, grabDx_ = 0, grabDy_ = 0;
    double startQ_ = 0.5;
    EditGesture gesture_;
};

}  // namespace amq::ui
