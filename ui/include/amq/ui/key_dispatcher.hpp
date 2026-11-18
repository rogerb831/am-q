#pragma once
// Key dispatcher (area 15 R1-R6; area 14 R9; area 21 R9). Focus first: the editor offers a key to
// a focused text field before calling keyDown(). Then graph commands; anything not handled returns
// false and the editor passes the key to the host (transport keys, Space, function keys).
//
// The key map (area 15; editor spec §6.3, approved 2026-11-18):
//   Delete, Backspace     delete the selected bands (or the focused band), one undo step (R9)
//   Escape                cancel a drag; else clear the selection
//   Left / Right          frequency -/+ 1/12 octave (Shift: 1/96)
//   Up / Down             gain +/- 0.5 dB (Shift: 0.1); gainless shapes: Q
//   Tab / Shift+Tab       focus the next / previous band by frequency
//   Command+Z             undo; Command+Shift+Z or Command+Y: redo
//   Command+A             select all bands
//   Command+C / X / V     copy, cut, paste bands (JSON; the editor syncs the system clipboard)
// Nudges act on the selection, or the focused band when nothing is selected; each key press is
// one undo step.

#include "amq/ui/band_ops.hpp"
#include "amq/ui/graph_model.hpp"

#include <functional>
#include <string>

namespace amq::ui {

enum class Key : uint8_t { Delete, Backspace, Escape, Left, Right, Up, Down, Tab, Character, Other };

struct KeyEvent {
    Key key = Key::Other;
    char32_t ch = 0;  // for Key::Character, lower case
    Modifiers mods;
};

class KeyDispatcher {
public:
    static constexpr double kNudgeOct = 1.0 / 12.0, kFineOct = 1.0 / 96.0;
    static constexpr double kNudgeDb = 0.5, kFineDb = 0.1, kNudgeQ = 0.02;

    KeyDispatcher(GraphInteraction& graph, BandOps& ops) : g_(graph), ops_(ops) {}

    // True when the key was handled; false: give it to the host.
    bool keyDown(const KeyEvent& e);

    std::string clipboard;  // our band clipboard (JSON)
    std::string message;    // the last visible message (paste into a full set, R10)
    std::function<void(const std::string&)> writeSystemClipboard;
    std::function<std::string()> readSystemClipboard;

private:
    bool nudge(double dOct, double dDb, double dQ);
    [[nodiscard]] BandOps::Bands targets() const { return BandOps::targets(g_.selection(), g_.focused()); }

    GraphInteraction& g_;
    BandOps& ops_;
};

}  // namespace amq::ui
