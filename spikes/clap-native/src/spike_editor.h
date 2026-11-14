#pragma once
// Am-Q spike editor: Visage (GPU, Direct3D 11 on Windows). Draws the design reference's
// dark ground and grid, the 0 dB line, one amber response curve, one draggable band handle
// and a small Freq / Gain / Q readout panel. Geometry and colours come from
// docs/design/spike-reference/ (tokens.json, editor-main.dc.html).

#include <cstdint>
#include <memory>
#include <string>

namespace visage {
class ApplicationWindow;
}

namespace amq::spike {

class SpikePlugin;
class GraphFrame;
class CurveFrame;
class HandleFrame;
class PanelFrame;

// Render statistics for the spike's evidence (dirty-region redraw, display rate, input lag).
struct EditorStats {
    uint64_t graphDraws = 0, curveDraws = 0, handleDraws = 0, panelDraws = 0;
    uint64_t mouseEvents = 0;
    double curveIntervalMeanMs = 0.0, curveIntervalP95Ms = 0.0;
    double inputToDrawMeanMs = 0.0, inputToDrawMaxMs = 0.0;
    double refreshHz = 0.0;
};

class SpikeEditor {
public:
    static constexpr int kWidth = 1180;
    static constexpr int kHeight = 672;

    explicit SpikeEditor(SpikePlugin& plugin);
    ~SpikeEditor();

    void attach(void* parentHwnd);
    void nativeSize(uint32_t* w, uint32_t* h) const;
    void refreshFromPlugin();  // main thread: parameter values changed elsewhere

    static EditorStats& stats();
    static std::string statsJson();
    static void resetStats();

private:
    SpikePlugin& plugin_;
    std::unique_ptr<visage::ApplicationWindow> app_;
    std::unique_ptr<GraphFrame> graph_;
    std::unique_ptr<CurveFrame> curve_;
    std::unique_ptr<HandleFrame> handle_;
    std::unique_ptr<PanelFrame> panel_;
    void layoutHandle();
};

}  // namespace amq::spike
