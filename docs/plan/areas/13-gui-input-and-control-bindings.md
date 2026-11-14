# 13 GUI input and control bindings

Am-Q implements the standard EQ graph gestures: double-click to create, drag to move, wheel for Q, and click, modifier and lasso selection. Every edit goes through the one parameter setter with begin, perform and end. The Product designer owns the exact modifier assignments, and Counsel reviews them.

## Requirements

1. R1. Shift, Alt/Option, Ctrl/Cmd and the left, right and middle buttons are normalised into one platform-neutral modifier set. Ctrl on Windows and Cmd on macOS map to the same logical modifier.
2. R2. A press that moves less than a drag threshold counts as a click. Larger movement starts a drag.
3. R3. Double-clicking an empty graph area creates a band at that frequency and gain, as a single undo step.
4. R4. A modified double-click on an empty area may create a band with a variant (for example, dynamic from the start). The Product designer decides.
5. R5. A single click on an empty area clears the selection and starts a lasso. The selection modifier keeps the existing selection.
6. R6. Clicking a band selects and focuses it. The add modifier adds it to the selection, and the toggle modifier toggles it.
7. R7. Alt-click on a band node toggles Enabled. Alt-click on the range handle toggles Dynamics Enabled. Each toggle is one undo step.
8. R8. Right-click on a band, or with a selection present, opens the band context menu (Tab 14).
9. R9. Dragging a node changes Frequency horizontally on a log axis. Vertically it changes Gain for gain shapes, or Q for shapes without gain.
10. R10. Dragging the range handle changes Dyn Range only.
11. R11. While a band is soloed, vertical drag adjusts the solo audition level, not the band gain.
12. R12. The fine modifier scales drag and wheel steps down by a fixed factor between 5 and 20.
13. R13. A designer-assigned modifier gives vertical drag control of Q.
14. R14. Dragging one node of a multi-selection moves every selected band by the same offset. An optional axis lock follows the dominant direction.
15. R15. The plain wheel changes Q. Modifier wheels change Gain, Dyn Range and Slope, with Slope moving in whole steps.
16. R16. The wheel direction is the same for all shapes, unless a user setting says otherwise.
17. R17. The horizontal wheel or trackpad pan zooms or scrolls the frequency axis.
18. R18. Every value written from the GUI is clamped to its parameter range. It is sent to the host as begin, then one or more performs, then end. Each gesture is one undo step.
19. R19. A new band starts with Am-Q-defined defaults for every field, written in the same undo step as the creation.
20. R20. If Gain-Q interaction is on, it couples Q to gain for bell-type shapes. It is off by default.
21. R21. Display-only settings (analyzer density, range) trigger a redraw only when their value actually changes.
22. R22. Each band-panel control is bound to exactly one parameter id. Changing it from the host updates the control within one frame.

## Am-Q design

Input lives in ui/interaction. A platform adapter turns JUCE mouse events into an Am-Q event with a logical modifier set. A gesture state machine (idle, pending, dragging node, dragging range, lasso, menu) chooses which parameter ids to write. Each gesture opens one begin/perform/end transaction through set(id, plain, GUI), so undo groups it automatically.

Am-Q addresses band fields by string id (band03.freq, band03.gain, band03.q, band03.dynRange), never by numeric index. That avoids the index confusion the static study ran into. Shape capability flags come from one helper in core/params (Tab 15) that says whether a shape has gain, Q and slope.

Design commitments:

- The wheel direction is the same for every shape.
- Solo-level drag is shown on screen while it is active.
- A double-click on a band opens Am-Q's inline value editor, not a separate popup.

The exact modifier-to-action table is an original Am-Q design document.

## Implementation tasks

- [ ] Write the Am-Q interaction spec: gestures, modifier table, drag threshold, fine factor and new-band defaults (Product designer, Phase 0)
- [ ] Have Counsel review the interaction spec for similarity to existing products (Counsel, Phase 0)
- [ ] Implement platform modifier normalisation and the click-versus-drag threshold (GUI engineer, Phase 1)
- [ ] Implement the gesture state machine with begin/perform/end transactions and clamping (GUI engineer, Phase 2)
- [ ] Implement band creation by double-click, including variants and defaults in one undo step (GUI engineer, Phase 2)
- [ ] Implement selection: click, add, toggle, lasso and clear (GUI engineer, Phase 2)
- [ ] Implement node and range-handle drag, fine adjust, Q-drag modifier and multi-band drag with axis lock (GUI engineer, Phase 2)
- [ ] Implement wheel bindings for Q, Gain, Dyn Range and Slope, plus horizontal zoom (GUI engineer, Phase 2)
- [ ] Implement Alt-click toggles for Enabled and Dynamics Enabled (GUI engineer, Phase 2)
- [ ] Implement solo-level drag with an on-screen indicator (GUI engineer, Phase 2)
- [ ] Implement optional Gain-Q coupling, off by default (DSP lead, Phase 3)
- [ ] Bind every band-panel control to its parameter id, with host-to-GUI refresh (GUI engineer, Phase 2)
- [ ] Add redraw-only-on-change for display settings (GUI engineer, Phase 2)
- [ ] Write scripted interaction tests for every gesture (Build and QA engineer, Phase 2)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Create band | Scripted double-click at a known point | One new band at the expected Frequency and Gain within one pixel of resolution; one undo step |
| Click versus drag | Scripted press with movement just under and over the threshold | Under: selection only, no write. Over: drag writes |
| Selection | Scripted click, add, toggle, lasso and clear sequences | The selection set matches the expected set after each step |
| Node drag | Drag a gain shape and a gainless shape | The gain shape writes Frequency and Gain; the gainless shape writes Frequency and Q; nothing else written |
| Range drag | Drag the range handle | Only Dyn Range changes |
| Fine adjust | Same drag with and without the fine modifier | The value change ratio equals the spec factor within 1 % |
| Multi-drag | Drag one of three selected bands | All three move by the same offset; one undo step |
| Wheel | Wheel with each modifier | Q, Gain, Dyn Range and Slope change as specified; Slope changes in whole steps |
| Alt-click | Alt-click the node and the range handle | Enabled and Dynamics Enabled toggle; one undo step each |
| Host transaction | Log VST3 and CLAP edit calls during a drag | Exactly one begin and one end around the performs; values within the range |
| Host-to-GUI | Automate band03.gain from the host | The control updates within one frame |
| Display redraw | Set analyzer density to the same value twice | Exactly one redraw |

## Open questions

- Which double-click variants does Am-Q offer, and with which modifiers?
- Should horizontal wheel zoom the frequency axis or pan it?
- Does Am-Q expose Gain-Q coupling at all in v1?
