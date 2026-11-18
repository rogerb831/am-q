# 14 Graph menu, band delete, gain-reduction feed

Am-Q provides a right-click band menu whose bulk actions are each one undo step. Bands can be deleted from the menu, from the band panel and from the keyboard. Per-band gain reduction reaches the GUI in the same lock-free snapshot as the analyzer.

## Requirements

1. R1. Right-clicking a band or an existing selection opens a band menu at the pointer. A right-click on an empty area with no selection opens a graph menu the Product designer defines.
2. R2. Menu labels show the current state. For example, an action reads "Enable" or "Disable" depending on the targets, and delete reads in the singular or plural as fits.
3. R3. The menu covers these action groups: selection, enable and disable, invert gain, add or clear dynamics, add or clear spectral, shape, slope and placement, clipboard (cut, copy, paste), split, and delete. The Product designer sets the final list and order.
4. R4. Every menu action that changes several bands or fields is one undo step.
5. R5. Making a band dynamic gives it a non-zero default range if its range is zero, so the effect can be heard at once.
6. R6. Deleting marks each targeted band Used = off and resets it to defaults, as one undo step. Bands not targeted are untouched.
7. R7. Delete targets the selected bands. If nothing is selected, it targets the focused band. "Delete all" and "delete disabled" target all used bands or all disabled bands.
8. R8. The band panel has a delete-band control and a clear-dynamics control that use the same delete and clear code as the menu.
9. R9. Delete and Backspace delete the selected bands (Tab 15).
10. R10. Paste that replaces bands reuses the delete code. Paste into a full 24-band set fails with a visible message.
11. R11. The Speakers field stays reserved. No speaker menu is shown in v1 because surround is not v1.
12. R12. The processor publishes every used band's live gain reduction in each snapshot. The GUI draws it on the node and on the band panel indicator within two frames.
13. R13. Spectrum snapshots carry a sequence number and a bin count. Level snapshots carry per-channel peak and RMS values.
14. R14. When the editor is closed, the processor keeps writing snapshots without allocating, and drops snapshots nobody reads.
15. R15. The host-side gain-reduction report and the track name and colour hooks are optional. They are decided per format (see open questions).

## Am-Q design

The menu model lives in ui/menus. Each entry is an action id with an enabled-state function and a label function, so labels update on open. Actions call a band-operations service in plugin/ (select, enable, invert, make dynamic, make spectral, set shape, set slope, set placement, copy, paste, split, delete). That service wraps each call in one undo group and writes through set(id, plain, GUI).

Delete has one code path with three entry points: menu, band panel and keyboard. Menu text, order and grouping are Am-Q's own design.

Gain reduction is measured on the audio thread per band, after the dynamics detector. It is written into the snapshot struct together with the spectra and meters (Tab 12). In-process, the processor pushes into a lock-free FIFO. Across processes, VST3 sends a message and CLAP reads on a timer, as already decided. Host extensions such as Studio One's gain-reduction query are read from a cached atomic value and never touch the FIFO.

## Implementation tasks

- [x] Define the band menu and graph menu: entries, grouping, labels and the disabled-state rules (Product designer, Phase 0). Done 2026-11-18: the product owner approved the editor spec as built (decisions.md D4) (graph §4.1)
- [x] Have Counsel review the menu spec for similarity to existing products (Counsel, Phase 0). Done 2026-11-18: marked complete by the product owner
- [x] Implement the band-operations service with one undo group per call (Plugin and host engineer, Phase 2). Done on 2026-11-16 (`ui/include/amq/ui/band_ops.hpp`): create, remove, enable/disable, invert gain, make/clear dynamics (a -6 dB default range when the range is 0, R5, PROVISIONAL), make/clear spectral, shape (resetting a slope the new shape does not allow), slope (only where allowed), placement, copy, cut, paste; each call one undo group, written through the controller with host gestures. Tested in `tests/ui/test_band_ops.cpp`
- [x] Implement the menu model with state-dependent labels and enabled states (GUI engineer, Phase 2). Done on 2026-11-16 (`ui/include/amq/ui/band_menu.hpp`): band menu and graph menu with labels from the targets' state ("Enable" or "Disable", "Make Dynamic" or "Clear Dynamics", "Delete Band" or "Delete 3 Bands"), checked shape/slope/placement, slopes offered only where every target allows them, dynamics only for gain shapes, Paste only with bands on the clipboard; Split shown disabled. Entries, order and wording PROVISIONAL until the Product designer's menu spec
- [x] Implement single-path band delete and clear dynamics with selection and focus rules (Plugin and host engineer, Phase 2). Done on 2026-11-16: `BandOps::remove` (Used off and every field to its default, one step; untouched bands unchanged) and `clearDynamics` are the only paths; `BandOps::targets` gives the selection or, with nothing selected, the focused band (R7); delete all and delete disabled target the used and the disabled bands
- [x] Add band panel delete and clear-dynamics controls wired to the same service (GUI engineer, Phase 2). Done on 2026-11-16: the panel's Delete and Clear Dynamics buttons call `BandOps::remove` and `clearDynamics`, the same code as the menu and the keys; right-click opens the band or graph menu from the menu model
- [x] Implement clipboard copy, cut and paste of bands as JSON, with a full-set check (Plugin and host engineer, Phase 3). Done on 2026-11-16: JSON with every band field by its string id; paste fills free bands in order, can replace targets (reusing delete, R10), and fails with "Not enough free bands: N needed, M free." changing nothing; cut is copy plus delete in one step. The system clipboard hook comes with the editor's key map
- [ ] Implement band split (Product designer to define the behaviour; DSP lead to implement) (DSP lead, Phase 3) Deferred by the product owner on 2026-11-18: Split stays a disabled menu entry until it is defined.
- [x] Add per-band gain-reduction measurement and snapshot publishing (DSP lead, Phase 2). Done on 2026-11-16: every block the engine writes each band's smoothed, signed gain change (0 dB when its dynamics are off) to the gain-change snapshot (current and peak since the last read)
- [x] Draw gain reduction on the node and the band panel indicator (GUI engineer, Phase 2). Done on 2026-11-16: an arc around the node whose length follows |change| / 30 dB, and a "GR" readout in the panel, both from the gain-change snapshot, redrawn when it changes
- [x] Make snapshot publishing allocation-free with drop-when-full (Plugin and host engineer, Phase 1). Fixed memory; an unread frame is replaced by the next. Tested: 10 s of publishing with no reader allocates nothing
- [x] Evaluate the optional host gain-reduction and track-context extensions (Plugin and host engineer, Phase 3). Evaluated 2026-11-16. CLAP has a track-info extension (track name, colour, channel layout) that a host may provide; reading it on the main thread when the host notifies would let the editor show the track name and colour at little cost: recommended for v1.x. CLAP has no standard gain-reduction query, and the VST3 equivalents are host-specific extensions that clap-wrapper does not expose, so a host gain-reduction meter would mean a VST3-only path outside clap-wrapper: not recommended for v1. If added later, it reads the cached gain-change value (area 14 design), never the snapshot FIFO. A product decision

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Menu labels | Open the menu over enabled, disabled and mixed selections | Labels and enabled states match the spec table |
| Bulk undo | Run each multi-band menu action, then undo once | State is byte-identical to the state before the action |
| Make dynamic | Make a zero-range band dynamic | Range becomes the non-zero default; one undo step |
| Delete targets | Delete with a selection, with a focus only, and with "delete all" or "delete disabled" | Exactly the expected bands go to Used = off |
| Delete paths | Delete the same band from the menu, the panel and the keyboard | Identical resulting state and undo entry |
| Full paste | Paste 3 bands into an instance with 23 used | Paste refused with a message; no partial change |
| Gain-reduction latency | Step the input level through the threshold; log the GUI indicator | Indicator reflects the change within two frames of the snapshot |
| Gain-reduction accuracy | Compare the indicator value with offline measured gain reduction | Within 0.5 dB |
| Closed editor | Run 10 min of audio with the editor closed under an allocation tracker | Zero audio-thread allocations; no FIFO growth |
| Cross-process | Run in a sandboxed host (Bitwig) | Meters and gain reduction update at about 30 per second |

## Open questions

- Should Am-Q answer Studio One's gain-reduction query and read the host's track name and colour in v1?
- What exactly does "split" do for an Am-Q band? The Product designer must specify it.
- Does the graph menu on an empty area exist, and what does it hold?
