# 15 Shape families, keyboard, Shape menu

Am-Q defines its own shape-family helper and uses it to drive controls, not to export anything to the host. Delete and Backspace delete the selected bands, Esc cancels, and every other key goes to the host. The Shape menu writes the band's Shape parameter through the standard setter.

## Requirements

1. R1. With the graph focused and at least one band selected, Delete and Backspace delete the selected bands as one undo step.
2. R2. Esc cancels the active drag or lasso, closes an open popup, or clears the selection, in that order of priority.
3. R3. Arrow, Home and End keys move focus between bands and controls for keyboard navigation. With a modifier, the arrows nudge the focused value by one step.
4. R4. A focused text field (value entry, preset search) takes all keys. No key reaches the graph or the host meanwhile.
5. R5. Am-Q forwards keys it does not use to the host on both Windows and macOS, so transport shortcuts keep working.
6. R6. The band panel Shape menu lists all 10 Am-Q shapes and writes the band's Shape through set(id, plain, GUI) as one undo step.
7. R7. Shapes are stored by stable Am-Q ids. Adding a shape later never renumbers saved states or presets.
8. R8. One helper reports, for every shape: whether it has gain, Q and slope, and its family (bell-type, shelf, low cut, high cut, notch, band pass, tilt, all pass).
9. R9. Controls show only fields the current shape uses. Changing shape keeps the values of hidden fields.
10. R10. Bell is the default shape for a new band.
11. R11. Am-Q does not keep unreachable shape variants or dead host-export hooks in v1.

## Am-Q design

core/params defines a shape enum with fixed ids and a ShapeInfo table: name, family, hasGain, hasQ, hasSlope and valid slopes. ui/ and core/dsp both read this table, so the GUI never hard-codes per-shape rules. The menu index and the stored id are the same thing, so no remapping table is needed. If a later format (for example AAX) needs an EQ-type export, it adds a mapping from family. It does not add a hidden slot.

Key handling lives in ui/keyboard. The editor root takes key events from JUCE. It asks the focused component first, then the graph's command map (delete, cancel, navigate), and returns "not handled" for anything else. JUCE's wrappers then pass unhandled keys back to the host. Text fields take keyboard focus only while editing. The Product designer owns the key map, and Counsel reviews it.

## Implementation tasks

- [ ] Define the shape enum, fixed ids and ShapeInfo table (DSP lead, Phase 1)
- [ ] Add a state-chunk test that pins shape ids across versions (Build and QA engineer, Phase 1)
- [ ] Write the Am-Q key map: delete, cancel, navigation, nudge and pass-through (Product designer, Phase 0)
- [ ] Implement the key dispatcher with focus-first, graph commands and host fall-through (GUI engineer, Phase 2)
- [ ] Wire Delete and Backspace to the band delete service (Tab 14) (GUI engineer, Phase 2)
- [ ] Implement keyboard focus traversal and value nudge for accessibility (GUI engineer, Phase 3)
- [ ] Build the band panel Shape menu from ShapeInfo with one undo step per change (GUI engineer, Phase 2)
- [ ] Hide or disable controls by ShapeInfo while keeping hidden values (GUI engineer, Phase 2)
- [ ] Verify key pass-through in every host in the matrix on both platforms (Build and QA engineer, Phase 4)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Delete key | Select two bands; press Delete, then Backspace on another selection | Selected bands deleted; one undo step each; others untouched |
| No selection | Press Delete with no band selected | No state change; key forwarded to the host |
| Esc order | Press Esc during a drag, with a popup open, and with a selection | Cancels the drag, closes the popup, or clears the selection respectively |
| Text focus | Type "Delete" and Backspace in the value entry field | Field edits only; no band deleted; host gets nothing |
| Host pass-through | Press Space in each matrix host with the editor focused | Host transport toggles |
| Navigation | Tab and arrow through bands and controls | Focus visits every control; the screen reader announces names |
| Shape menu | Pick each of the 10 shapes from the menu | Shape id written once per pick; one undo step; curve updates |
| Shape flags | Unit test every shape against ShapeInfo | DSP and GUI agree on gain, Q and slope use |
| Hidden values | Set Gain, change to a cut shape and back | Gain is restored unchanged |
| Id stability | Load v1 state files in later builds | All shapes load unchanged |

## Open questions

- Which modifier nudges values with the arrows, and by how much?
- Should Delete with only a focused (not selected) band delete that band?
