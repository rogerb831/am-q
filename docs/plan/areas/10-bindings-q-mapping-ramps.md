# 10 Bindings, Q mapping, ramps

Am-Q propagates parameter changes through one explicit path: setter, then listeners, then lock-free slots read by the DSP once per block. Q uses Q = 0.025·1600^n (Q = 1 at the centre). Continuous DSP parameters ramp automatically when their slot changes.

## Requirements

1. R1. A parameter change reaches every consumer through the table. The DSP reads it once per block; GUI listeners get it on the message thread, coalesced. Unchanged values trigger nothing.
2. R2. Q = Qmin·(Qmax/Qmin)^n with Qmin 0.025 and Qmax 40. Default n = 0.5 gives Q = 1. The inverse is exact.
3. R3. Each continuous DSP parameter has a ramp that retargets when its slot changes, with an automatic ramp time (Tab 09 R13).
4. R4. When a ramp is linked to a new band or shape, it seeds from the current value. It never jumps from a default.
5. R5. Derived structures, such as the spectral grid or filter coefficients, rebuild only when an input value actually changes.
6. R6. GUI controls bind to parameters by stable id. Visual style is chosen separately and never carries binding data.
7. R7. Display value = unit function of the stored value, with optional scale and offset. Every parameter has a tested formatter and parser.

## Am-Q design

- **Propagation.**
  - set() stores into the table and pushes the slot value for the audio thread.
  - It posts a message-thread notification, coalesced per frame for the GUI.
  - Host notification happens only for GUI-sourced edits inside a gesture.
  - This is one table-wide mechanism, not a per-parameter list.
- **Q mapping.** Implemented in core/params as a pure function pair, using a standard pow/log pair. The encoding is a generic log mapping already decided for Am-Q. The design-Q convention per shape is in Tab 09 R12.
- **Ramps.** The audio side keeps a ramp per band field (frequency, gain, Q, range, threshold) and per output field. On a slot change it calls retarget(value, auto). Shape or band activation reseeds the ramp at the current value.
- **Change detection.** Each derived structure keeps the last inputs it was built from. It compares them per block and rebuilds on difference.
- **GUI binding.** An attachment object links a control to an id. It handles gestures and listens for external changes. Style comes from the theme (Tab 13).

## Implementation tasks

- [x] Implement the Q mapping pair and its unit test (Plugin and host engineer, Phase 1)
- [x] Implement message-thread notification coalescing and audio slot publishing in set() (Plugin and host engineer, Phase 1)
- [x] Implement the per-field audio ramp bank with retarget and reseed (DSP lead, Phase 1). Ramps exist for frequency, gain, Q, range and threshold on all 24 bands, plus the output fields, with retarget, and reseed on prepare and state load; since 2026-11-16 also on band activation (a band switched on starts at its values, not mid-ramp; tested) and on a shape change (frequency, gain and Q start at the new shape's values; the R17 structure crossfade hides the jump)
- [x] Implement last-input change detection for coefficients and the spectral grid (DSP lead, Phase 2). Done: each band keeps the inputs of its current design (frequency, gain, Q, shape, slope) and redesigns only when one differs (`readBands`); the dynamic filter, side-chain filter, attack/release timing and knee keep their own last inputs (2026-11-16); the spectral grid rebuilds only when its span or Density changes, and each band's relative response only when its design inputs change (`SpectralStage`); Natural Phase and Auto Gain caches compare design inputs by value
- [x] Implement the GUI parameter attachment class bound by id (GUI engineer, Phase 1). `ui/include/amq/ui/param_attachment.hpp`: string ids, gestures, text entry, change callback
- [ ] Implement formatters and parsers for every parameter (GUI engineer, Phase 1). Progress: host-facing formatters and parsers exist in `core/params`, round-trip tested for all 573 parameters. Still to do: editor display styles

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Q mapping | Evaluate n = 0, 0.5 and 1, then round-trip 10,000 points | 0.025, 1.0 and 40 exactly to float precision; round-trip relative error < 1e-6 |
| No-op writes | Write the same value 1,000 times | Zero listener calls after the first; no rebuilds |
| Coalescing | 10,000 host changes in one second | GUI repaints at most at frame rate; final value correct |
| Ramp reseed | Change band shape during playback | No click above −90 dBFS |
| Binding by id | Reorder GUI layout in a test build | Every control still drives its id (automated sweep) |
| Formatter/parser | Round-trip display strings for all parameters | Parse of format returns the value within display precision |

## Open questions

None.
