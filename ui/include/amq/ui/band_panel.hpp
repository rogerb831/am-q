#pragma once
// Band panel model (area 12 tasks "band panel dynamics section", "spectral section"; area 13 "bind
// every band-panel control to its parameter id"; area 15 R7-R9 "Shape menu from ShapeInfo", "hide or
// disable controls by ShapeInfo while keeping hidden values"; area 06 R6 grey knobs in Auto and the
// effective-time tooltip). Framework-free: the editor lays the controls out and draws them.
//
// Each control is bound by parameter id (string ids through the table, area 10 R6). Visibility
// follows the shape descriptor (gain, Q, slope) and the band's state (SC Low/High only in Free mode,
// the dynamics and spectral sections only for gain shapes); hidden controls keep their values
// (nothing writes them). Enabled state greys controls that do nothing now (dynamics off; threshold,
// attack and release while Auto is on, R6; spectral controls while Spectral is off). The Delete and
// Clear Dynamics buttons call BandOps (area 14 R8). Layout and wording are PROVISIONAL until the
// Product designer's region map (area 12 Phase 0 task).

#include "amq/params/parameter_table.hpp"

#include <string>
#include <vector>

namespace amq::ui {

enum class ControlKind : uint8_t { Knob, Toggle, Choice, Button };
enum class PanelButton : uint8_t { None, Delete, ClearDynamics };

struct PanelControl {
    std::string section;          // "Band", "Dynamics", "Side Chain", "Spectral"
    std::string label;
    ControlKind kind = ControlKind::Knob;
    params::ParamIndex id = 0;    // for Knob, Toggle, Choice
    PanelButton button = PanelButton::None;
    bool visible = true, enabled = true;
    std::string text;             // the value as displayed (area 10 R7)
    std::string tooltip;          // Attack / Release: the effective time in ms at the band frequency
};

struct ChoiceItem {
    int value = 0;
    std::string label;
    bool checked = false, enabled = true;
};

// The controls of band b (b < 0: none).
[[nodiscard]] std::vector<PanelControl> bandPanel(const params::ParameterTable& t, int band);
// The items of a Choice control: labels from the table (area 11 menus); for Slope only the slopes
// the shape allows are enabled; for Shape the ShapeInfo names (area 15 R8).
[[nodiscard]] std::vector<ChoiceItem> choiceItems(const params::ParameterTable& t, params::ParamIndex id);

}  // namespace amq::ui
