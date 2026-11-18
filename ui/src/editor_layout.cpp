#include "amq/ui/editor_layout.hpp"

#include <algorithm>

namespace amq::ui {

namespace {

void panelBlock(PanelGeometry& p, const PanelContent& c, float gw, float gh, float offset);

void panelGeometry(EditorLayout& L, const PanelContent& c) {
    auto& p = L.panel;
    const float gw = L.graph.w, gh = L.graph.h;
    if (!c.band) {
        p.mode = PanelMode::Hidden;
        return;
    }
    p.mode = gh < 400.0f ? PanelMode::Strip : (gh < 520.0f || gw < 680.0f) ? PanelMode::Compact : PanelMode::Full;
    const bool compact = p.mode != PanelMode::Full;
    p.knobLg = compact ? 64.0f : 88.0f;
    p.knobSm = compact ? 48.0f : 64.0f;
    p.knobDyn = compact ? 40.0f : 48.0f;
    p.control = compact ? 28.0f : 32.0f;
    p.padX = compact ? 16.0f : 20.0f;
    p.padY = compact ? 12.0f : 16.0f;
    p.rowGap = 16.0f;
    const float offset = compact ? 16.0f : 32.0f;
    panelBlock(p, c, gw, gh, offset);
    if (p.mode == PanelMode::Strip) {
        // The expanded strip: the compact panel over the plot, above the bottom offset if it fits;
        // if not, tightened until it fits 4 from the graph's top and bottom.
        const float room = p.rect.h <= gh - offset - 4.0f ? gh - offset - 4.0f : gh - 8.0f;
        for (int step = 0; step < 4 && p.rect.h > room; ++step) {
            if (step == 0) p.padY = 8.0f;
            if (step == 1) p.rowGap = 8.0f;
            if (step == 2) p.knobDyn = 32.0f;
            if (step == 3) p.rowGap = 6.0f;
            panelBlock(p, c, gw, gh, offset);
        }
        p.expanded = p.rect;
        p.expanded.y = std::max(4.0f, gh - offset - p.rect.h);
        const float w = std::min(420.0f, gw - 32.0f);
        p.rect = {(gw - w) * 0.5f, gh - offset - 44.0f, w, 44.0f};
    }
}

// The panel's rows, size and rect for the current knob sizes and spacing.
void panelBlock(PanelGeometry& p, const PanelContent& c, float gw, float gh, float offset) {
    using E = EditorLayout;
    // Row 1: A | knobs | D. Row 2 (or inline): dynamics group | SIDE CHAIN.
    float knobs = p.knobSm;  // Freq
    float knobH = p.knobSm;
    if (c.gain) knobs += E::kKnobGap + p.knobLg, knobH = p.knobLg;
    if (c.q) knobs += E::kKnobGap + p.knobSm;
    const float colA = 8.0f + 3.0f * p.control;  // title row (28 ~ control) + two selects, gaps 8
    const float colD = p.control + 8.0f + p.control + 8.0f + 28.0f;
    p.mainRowW = E::kColumn + E::kColGap + knobs + E::kColGap + E::kColumn;
    p.row1H = std::max({colA, colD, knobH + E::kKnobText});
    p.dynRowW = 0.0f, p.row2H = 0.0f;
    if (c.dynamic) {
        // RANGE, THRESHOLD, ATTACK, RELEASE, DENSITY
        float dyn = 24.0f - E::kDynGap;
        if (c.rangeKnob) dyn += E::kDynGap + E::knobSlot(p.knobDyn, 5);
        dyn += 3.0f * E::kDynGap + E::knobSlot(p.knobDyn, 9) + E::knobSlot(p.knobDyn, 6) + E::knobSlot(p.knobDyn, 7);
        if (c.spectral) dyn += E::kDynGap + E::knobSlot(p.knobDyn, 7);
        const float sc = 14.0f + 8.0f + 28.0f + 8.0f + 28.0f + (c.spectral ? 8.0f + 28.0f : 0.0f);
        p.dynRowW = dyn + E::kColGap + E::kColumn;
        p.row2H = std::max(sc, 28.0f + 8.0f + p.knobDyn + E::kKnobText);
    }
    const float oneRow = p.mainRowW + (c.dynamic ? E::kColGap + p.dynRowW : 0.0f);
    p.wrapped = c.dynamic && oneRow + 2.0f * p.padX > gw - 48.0f;
    float w = 0.0f, h = 0.0f;
    if (p.wrapped) {
        w = std::max(p.mainRowW, p.dynRowW);
        h = p.row1H + p.rowGap + 1.0f + p.rowGap + p.row2H;
    } else {
        w = oneRow;
        h = std::max(p.row1H, p.row2H);
    }
    w += 2.0f * p.padX;
    h += 2.0f * p.padY;
    p.rect = {std::max(0.0f, (gw - w) * 0.5f), gh - offset - h, w, h};
}

}  // namespace

EditorLayout EditorLayout::compute(float width, float height, const PanelContent& content) {
    EditorLayout L;
    L.width = width, L.height = height;
    L.top = {0.0f, 0.0f, width, kBar};
    L.status = {0.0f, height - kBar, width, kBar};
    L.graph = {0.0f, kBar, width, std::max(0.0f, height - 2.0f * kBar)};
    L.plot = {0.0f, kPlotTop, std::max(1.0f, width - kGutter), std::max(1.0f, L.graph.h - kPlotTop - kPlotBottom)};
    L.matchGlyphOnly = width < 1100.0f;
    L.abInOverflow = width < 960.0f;
    L.fsHelpInOverflow = width < 800.0f;
    L.presetMinW = width < 800.0f ? 140.0f : 220.0f;
    L.hidePrevNext = width < 700.0f;
    L.hideGainScale = width < 1180.0f;
    L.hideOutputCaption = width < 1020.0f;
    L.hideAutoGain = L.hideLatencyText = width < 900.0f;
    L.hideAnalyzerCaption = width < 760.0f;
    panelGeometry(L, content);
    return L;
}

}  // namespace amq::ui
