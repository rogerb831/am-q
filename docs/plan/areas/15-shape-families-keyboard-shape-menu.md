# 15 Shape families, keyboard, Shape menu

Am-Q defines its own shape-family helper and uses it to drive controls, not to export anything to the host. Delete and Backspace delete the selected bands, Esc cancels, and every other key goes to the host. The Shape menu writes the band's Shape parameter through the standard setter.

## Requirements

1. R1. With the graph focused and at least one band selected, Delete and Backspace delete the selected bands as one undo step.
2. R2. Esc cancels the active drag or lasso, closes an open popup, or clears the selection, in that order of priority.
3. R3. Arrow, Home and End keys move focus between bands and controls for keyboard navigation. With a modifier, the arrows nudge the focused value by one step. Keyboard navigation ships in v1. Screen-reader support for it (announcing names, roles and values) is post-v1 ([D1](../decisions.md)); see R12.
4. R4. A focused text field (value entry, preset search) takes all keys. No key reaches the graph or the host meanwhile.
5. R5. Am-Q forwards keys it does not use to the host on both Windows and macOS, so transport shortcuts keep working.
6. R6. The band panel Shape menu lists all 10 Am-Q shapes and writes the band's Shape through set(id, plain, GUI) as one undo step.
7. R7. Shapes are stored by stable Am-Q ids. Adding a shape later never renumbers saved states or presets.
8. R8. One helper reports, for every shape: whether it has gain, Q and slope, and its family (bell-type, shelf, low cut, high cut, notch, band pass, tilt, all pass).
9. R9. Controls show only fields the current shape uses. Changing shape keeps the values of hidden fields.
10. R10. Bell is the default shape for a new band.
11. R11. Am-Q does not keep unreachable shape variants or dead host-export hooks in v1.
12. R12. Accessible-control model (design commitment, v1). Every interactive control and every band node exposes, through one Am-Q UI interface:
    - a stable name;
    - a role (button, toggle, slider, choice, text field, graph node, group);
    - its current value as text and as a normalised number;
    - its actions (press, increment, decrement, set value, open menu);
    - its focus order;
    - its enabled and visible state.
   The value text comes from the parameter formatters (area 10), and actions go through set(id, plain, GUI). Keyboard navigation (R3) is built on this model. A post-v1 platform layer (UIA on Windows, NSAccessibility on macOS) only maps the model to the OS and needs no change to controls.

## Am-Q design

core/params defines a shape enum with fixed ids and a ShapeInfo table: name, family, hasGain, hasQ, hasSlope and valid slopes. ui/ and core/dsp both read this table, so the GUI never hard-codes per-shape rules. The menu index and the stored id are the same thing, so no remapping table is needed. If a later format (for example AAX) needs an EQ-type export, it adds a mapping from family. It does not add a hidden slot.

Key handling lives in ui/keyboard. The editor root takes key events from Visage. It asks the focused frame first, then the graph's command map (delete, cancel, navigate), and returns "not handled" for anything else. Visage passes unhandled keys back to the host: on Windows it posts them to the parent window; on macOS they continue up the NSResponder chain. Text fields take keyboard focus only while editing. The Product designer owns the key map, and Counsel reviews it.

Visage provides key events, keyboard focus and handled/unhandled results, but no general Tab focus traversal (only between text fields). Am-Q builds focus order itself, from the accessible-control model (R12): ui/ keeps an ordered list of focusable controls per region and moves Visage keyboard focus along it. This needs no accessibility layer, so keyboard navigation stays in v1.

ui/access holds the accessible-control model: an `AccessibleControl` interface implemented by every widget, plus a tree that mirrors the editor's regions. In v1 its users are keyboard navigation, focus drawing and GUI tests (scripted tests find controls by name and role). After v1, a platform bridge publishes the same tree to UIA and NSAccessibility.

## Implementation tasks

- [x] Define the shape enum, fixed ids and ShapeInfo table (DSP lead, Phase 1). `dsp::Shape` ids plus `params::shapeInfo`
- [x] Add a state-chunk test that pins shape ids across versions (Build and QA engineer, Phase 1). `tests/plugin/fixtures/shape_ids_v2.amqs` is a frozen version-2 state chunk. A script writes it from the documented chunk format and the golden host ids, not from the plugin; it is committed once and never regenerated. Band k holds shape id k − 1 for bands 1–10, and bands 11–24 are absent. The plugin host test loads it into a fresh instance and checks every band's value and its text (Bell to All Pass), with absent bands loading Bell. It also checks that saving writes each shape pair back bit-identical, and that every Shape parameter is stepped from 0 to at least 9, so shapes can only be appended. A params unit test pins the enum values and the choice labels for ids 0–9
- [ ] Write the Am-Q key map: delete, cancel, navigation, nudge and pass-through (Product designer, Phase 0)
- [x] Implement the key dispatcher with focus-first, graph commands and host fall-through (GUI engineer, Phase 2). Done on 2026-11-16 (`ui/key_dispatcher`, PROVISIONAL key map in the header; tested): the graph frame takes keyboard focus on click and hands keys to the dispatcher; unhandled keys return false to Visage, which passes them on to the host. Pass-through in every host is the Phase 4 matrix task
- [x] Wire Delete and Backspace to the band delete service (Tab 14) (GUI engineer, Phase 2). Done in the dispatcher (2026-11-16): both call `BandOps::remove` on the selection or the focused band, one undo step; tested with bands actually selected (area 21 R9)
- [ ] Define the accessible-control model (R12) and make every widget implement it from its first version (GUI engineer, Phase 1). Progress: the model (`ui/include/amq/ui/accessible.h`), the editor root and graph groups, and band nodes (name, role, value text, press, ±1 dB nudge, set frequency) are done. Every new widget implements it as it is built
- [ ] Implement keyboard focus traversal and value nudge on the accessible-control model (GUI engineer, Phase 3). Progress (2026-11-16): keyboard focus traversal (Tab / Shift+Tab by frequency) and nudges (arrows, Shift fine) exist in the graph model and the key dispatcher; the accessible band nodes offer Increment / Decrement and SetValue. Still to do: a focus API on the accessible-control model itself, so assistive tools and the keys share one focus
- [ ] Post-v1: platform accessibility bridge (UIA, NSAccessibility) over the accessible-control model (GUI engineer, after v1)
- [x] Build the band panel Shape menu from ShapeInfo with one undo step per change (GUI engineer, Phase 2). Done on 2026-11-16: `choiceItems` gives the Shape items with ShapeInfo names (and Slope items enabled only where the shape allows them); a pick calls `BandOps::setShape`, one undo step; tested
- [x] Hide or disable controls by ShapeInfo while keeping hidden values (GUI engineer, Phase 2). Done on 2026-11-16 (`ui/band_panel`): gain, Q and slope controls follow the shape descriptor, the dynamics and spectral sections show for gain shapes, SC Low/High only in Free mode; hidden controls are never written, so a hidden gain survives a shape change (tested)
- [ ] Verify key pass-through in every host in the matrix on both platforms (Build and QA engineer, Phase 4)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Delete key | Select two bands; press Delete, then Backspace on another selection | Selected bands deleted; one undo step each; others untouched |
| No selection | Press Delete with no band selected | No state change; key forwarded to the host |
| Esc order | Press Esc during a drag, with a popup open, and with a selection | Cancels the drag, closes the popup, or clears the selection respectively |
| Text focus | Type "Delete" and Backspace in the value entry field | Field edits only; no band deleted; host gets nothing |
| Host pass-through | Press Space in each matrix host with the editor focused | Host transport toggles |
| Navigation | Tab and arrow through bands and controls | Focus visits every control in the defined order; the focus indicator is visible (screen-reader announcement is a post-v1 test) |
| Accessible model | Unit test: walk the accessible-control tree of a fully built editor | Every interactive control has a non-empty name, a role, value text matching the parameter formatter, and at least one action |
| Shape menu | Pick each of the 10 shapes from the menu | Shape id written once per pick; one undo step; curve updates |
| Shape flags | Unit test every shape against ShapeInfo | DSP and GUI agree on gain, Q and slope use |
| Hidden values | Set Gain, change to a cut shape and back | Gain is restored unchanged |
| Id stability | Load v1 state files in later builds | All shapes load unchanged |

## Open questions

- Which modifier nudges values with the arrows, and by how much?
- Should Delete with only a focused (not selected) band delete that band?
