#pragma once
// The editor's view of EQ Match (areas 16-20). The plugin implements it over the engine's
// EqMatchStage; the editor only reads status and calls these controls (area 17 design: the GUI never
// writes match data). All calls on the GUI thread.

#include "amq/analysis/match_capture.hpp"
#include "amq/dsp/analog_prototype.hpp"

#include <array>
#include <string>
#include <vector>

namespace amq::ui {

struct MatchBand {
    dsp::Shape shape = dsp::Shape::Bell;
    dsp::Slope slope = dsp::Slope::Db12;
    double hz = 1000.0, gainDb = 0.0, q = 1.0;
};

struct MatchStatus {
    bool inputReady = false, referenceReady = false, inputRecording = false, referenceRecording = false;
    bool curveValid = false, fitting = false, fileBusy = false, referenceIsFile = false;
    double inputSeconds = 0.0, referenceSeconds = 0.0, levelOffsetDb = 0.0;
    double inputDroppedSeconds = 0.0, referenceDroppedSeconds = 0.0;  // audio the analysis worker skipped (area 17 R6)
    float fitProgress = 0.0f, fileProgress = 0.0f;
    std::string fileError;
    bool lastSeconds = false;  // averaging window: false whole capture, true last N seconds
    double windowSeconds = 10.0;
    int smoothing = 1;         // 0: 1/3, 1: 1/6, 2: 1/12, 3: 1/24 octave
    bool levelAlign = true;
    std::array<double, analysis::kMatchGridPoints> target{}, weight{};
};

class MatchHost {
public:
    virtual ~MatchHost() = default;
    virtual bool status(MatchStatus& out) = 0;
    virtual void setRecording(int capture, bool on) = 0;  // 0 input, 1 reference
    virtual void reset(int capture) = 0;
    virtual void setReferenceFromFile(bool file) = 0;
    virtual bool loadReferenceFile(const std::string& utf8Path, std::string* error) = 0;
    virtual void setWindow(bool lastSeconds, double seconds) = 0;
    virtual void setSmoothing(int index) = 0;
    virtual void setLevelAlign(bool on) = 0;
    virtual bool startFit() = 0;
    virtual void cancelFit() = 0;
    virtual bool takeFit(std::vector<MatchBand>& out) = 0;
};

}  // namespace amq::ui
