#pragma once
// Band menu and graph menu model (area 14 R1-R5, R7, R10). Framework-free: the editor draws the
// items and calls activate(). Labels and enabled states are computed when the menu opens, from the
// targets and the table (R2). Every action goes through BandOps, so each is one undo step (R4).
//
// Entries, order and wording follow docs/design/editor-spec-graph.md §4.1 (Counsel review pending):
// Enabled, Invert Gain, Make Dynamic, Make Spectral | Shape, Slope, Placement | Cut, Copy, Paste,
// Select All | Split (disabled until defined, area 14), Delete.

#include "amq/ui/band_ops.hpp"

#include <string>
#include <vector>

namespace amq::ui {

namespace menu {
enum Action : int {
    SelectAll = 1, ClearSelection, Enable, Disable, InvertGain, MakeDynamic, ClearDynamics, MakeSpectral,
    ClearSpectral, Cut, Copy, Paste, Split, Delete, DeleteAll, DeleteDisabled,
    ShapeBase = 100,      // + dsp::Shape
    SlopeBase = 200,      // + dsp::Slope
    PlacementBase = 300,  // + placement (Stereo, Left, Right, Mid, Side)
};
}

struct MenuItem {
    int action = 0;
    std::string label;
    bool enabled = true;
    bool checked = false;         // the current value of a choice
    bool separatorBefore = false;
    std::string submenu;          // items with the same submenu name group under it
    bool checkbox = false;        // drawn with a check (Enabled, Make Dynamic, Make Spectral)
    bool mixed = false;           // a checkbox whose targets differ
    std::string hint;             // shortcut hint, right-aligned ("Ctrl C")
};
// The menu caption (spec §4.1): "BAND #3 · BELL" or "2 BANDS · #3, #4".
[[nodiscard]] std::string bandMenuCaption(const BandOps& ops, const BandOps::Bands& targets);

// Right-click on bands or a selection (R1): actions on `targets`.
[[nodiscard]] std::vector<MenuItem> bandMenu(const BandOps& ops, const BandOps::Bands& targets, bool clipboardHasBands);
// Right-click on an empty area with no selection (R1).
[[nodiscard]] std::vector<MenuItem> graphMenu(const BandOps& ops, bool clipboardHasBands);

// Runs an action. Selection actions return false (the editor owns the selection). clipboard is read
// for Paste and written by Cut and Copy; message receives a visible message on failure (R10).
bool activate(BandOps& ops, int action, const BandOps::Bands& targets, std::string& clipboard, std::string* message);

}  // namespace amq::ui
