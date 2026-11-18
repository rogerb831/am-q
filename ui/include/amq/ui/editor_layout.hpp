#pragma once
// Editor region map and reflow (docs/design/editor-spec.md §1; panels §2.3). Framework-free and
// unit-tested: the Visage frames in editor.cpp only place themselves where this says.
//
// Coordinates are logical pixels at 100 % interface scale, origin at the window's top-left.
// Graph-local rectangles (plot, panel) have their origin at the graph region's top-left.

namespace amq::ui {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] float right() const noexcept { return x + w; }
    [[nodiscard]] float bottom() const noexcept { return y + h; }
    [[nodiscard]] bool contains(float px, float py) const noexcept { return px >= x && px < x + w && py >= y && py < y + h; }
};

enum class PanelMode : unsigned char { Hidden, Full, Compact, Strip };

// What the focused band shows (panels §2.3 "How the panel changes with the band's state").
struct PanelContent {
    bool band = false;  // a band is focused
    bool gain = true, q = true, slope = true;
    bool dynamic = false, spectral = false;
    bool rangeKnob = true;  // false for a dynamic band without gain
};

struct PanelGeometry {
    PanelMode mode = PanelMode::Hidden;
    Rect rect;                         // graph-local
    bool wrapped = false;              // dynamics and SIDE CHAIN on a second row
    float knobLg = 88, knobSm = 64, knobDyn = 48, control = 32;
    float padX = 20, padY = 16;
    float row1H = 0, row2H = 0;        // content heights of the rows
    float mainRowW = 0, dynRowW = 0;   // content widths (without padding)
};

struct EditorLayout {
    static constexpr float kBar = 44.0f;
    static constexpr float kPlotTop = 24.0f, kPlotBottom = 24.0f, kGutter = 100.0f;
    static constexpr float kColumn = 148.0f, kColGap = 24.0f, kKnobGap = 16.0f, kDynGap = 12.0f;
    static constexpr float kKnobText = 8.0f + 14.0f + 8.0f + 16.0f;  // label and value under a knob

    float width = 0, height = 0;
    Rect top, graph, status;  // window coordinates
    Rect plot;                // graph-local
    PanelGeometry panel;

    // Top-bar collapse (main §1.2, P-1).
    bool matchGlyphOnly = false, abInOverflow = false, fsHelpInOverflow = false, hidePrevNext = false;
    float presetMinW = 220.0f;
    // Status-bar collapse (P-2).
    bool hideGainScale = false, hideOutputCaption = false, hideAutoGain = false, hideLatencyText = false,
         hideAnalyzerCaption = false;

    static EditorLayout compute(float width, float height, const PanelContent& content);
    // A knob's column is as wide as its knob or its caption, whichever is wider (the board's flex
    // columns). Captions are 12 px semibold capitals with 0.06em tracking: about 8 px a character.
    static float knobSlot(float size, int captionChars) { return size > captionChars * 8.0f ? size : captionChars * 8.0f; }
};

}  // namespace amq::ui
