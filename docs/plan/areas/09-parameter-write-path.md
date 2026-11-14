# 09 Parameter write path

Every Am-Q parameter write goes through one clamped setter. GUI edits are wrapped in begin/perform/end gestures, and each parameter maps one-to-one to a fixed host index. Host changes reach the DSP by sample-accurate block splitting and short per-sample ramps.

## Requirements

1. R1. set(id, plain, source) clamps to range. It ignores changes smaller than one representable step, stores the value and notifies listeners with the source.
2. R2. Indexed parameters (Shape, Slope, Placement, SC Mode and so on) clamp to valid indices. An optional table separates menu order from stored value, so menus can be reordered without breaking state.
3. R3. Normalized and plain values relate linearly in the stored encoding:
   - Frequency is log2 Hz over 10 Hz–30 kHz.
   - Gain and range are dB over ±30.
   - Q is n over 0–1.
   - Indexed values use index/(count−1).
4. R4. A GUI drag issues one begin, any number of performs and one end. Hosts record a single automation gesture.
5. R5. Each Am-Q parameter maps to exactly one host parameter at a fixed index.
6. R6. Host changes split the block at each change offset. Am-Q then ramps from the previous value instead of stepping.
7. R7. Bypass uses the format's own bypass parameter (VST3 bypass flag, CLAP bypass) with a click-free crossfade, latency-compensated.
8. R8. No MIDI-mapped parameters in v1.
9. R9. The processor never emits parameter changes in v1. EQ Match, undo and presets write from the message thread through set().
10. R10. Non-parameter traffic between processor and controller is defined: latency change, preset name and bulk data (analyzer, EQ Match). It works with VST3 split components and with CLAP.
11. R11. Filter design reads ramped frequency, gain and Q. Gain is read only for shapes that use it. Frequency and gain are clamped to their ranges.
12. R12. Displayed Q maps to the filter's design Q by a documented per-shape convention. Tilt shapes use their own Q-to-slope mapping (Tab 15).
13. R13. Ramps retarget from the current ramp value. Small moves get a short ramp; large jumps are instant. Times are in ms and independent of sample rate.
14. R14. On prepare and on state restore, ramps start at the current values. They never ramp from defaults.
15. R15. A full state restore applies all values together, without ramps, before the next processed block.

## Am-Q design

- **Setter.** core/params owns the table, the clamp and the epsilon compare. Listeners are typed by thread: the message thread gets notifications, the audio side gets lock-free slots. Source tags drive undo (GUI and EQ Match only).
- **Gestures.**
  - Controls call beginEdit on mouse-down, perform on each move and endEdit on release.
  - Double-click reset and typed entry wrap one perform in begin/end.
  - Multi-parameter drags open one gesture per parameter, all closed together. A graph node drag is an example (frequency and gain).
- **Host mapping.** Stable string ids (band03.freq) map to fixed indices through a versioned table. JUCE 8 wrappers expose these; CLAP uses the same indices as param ids.
- **Host to DSP.**
  - The processor scans incoming parameter events and splits the block at each offset.
  - It writes the slot, and the audio-side ramp retargets.
  - Host bypass and latency use the standard format mechanisms (VST3 restartComponent with the latency flag; CLAP latency-changed notification).
- **Processor-controller channel.** One small typed message layer carries latency, preset name and blob payloads. Analyzer data uses the lock-free snapshot queue (Tab 11), not this channel.
- **Ramps.**
  - Per-sample linear ramps are read every 4 samples, and the filter is redesigned at most every 32 samples (decided).
  - Ramp time shrinks as the move grows, down to instant above a jump threshold. The DSP lead sets this by zipper measurement.
  - Host jumps detected by the split logic and preset loads are instant.
- **Design commitments.** Am-Q ramps between host automation points and keeps a strict one-to-one map between parameters and host ids.

## Implementation tasks

- [ ] Write the parameter-table spec: encodings, ranges, menu-order tables, fixed host indices, versioning (Plugin and host engineer, Phase 0)
- [ ] Implement set() with clamp, epsilon compare, source tags and listener notification (Plugin and host engineer, Phase 1)
- [ ] Implement the GUI gesture helper for single and multi-parameter drags (GUI engineer, Phase 1)
- [ ] Implement VST3 and CLAP parameter export with fixed indices and linear normalized mapping (Plugin and host engineer, Phase 1)
- [ ] Implement the event scan and block splitter with slot writes (Plugin and host engineer, Phase 1)
- [ ] Implement audio-side ramps with size-dependent time, instant jumps and seed-on-prepare (DSP lead, Phase 1)
- [ ] Implement format bypass with latency-compensated crossfade (Plugin and host engineer, Phase 2)
- [ ] Implement the processor-controller message layer for latency, preset name and blobs (Plugin and host engineer, Phase 1)
- [ ] Document the per-shape displayed-Q to design-Q convention (DSP lead, Phase 0)
- [ ] Build host automation tests in at least three DAWs (Build and QA engineer, Phase 3)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Clamp and epsilon | Unit test: out-of-range and sub-epsilon writes from every source | Stored value clamped; sub-epsilon writes send no notification |
| Normalized mapping | Round-trip every parameter: plain to normalized to plain at 1,000 points | Error < 1e-6 of range; indexed values exact |
| Gesture integrity | Record GUI drags in host automation; log begin/perform/end | Exactly one begin and one end per drag; none left open |
| One-to-one mapping | Validators and an id/index dump | Each id has one fixed index across versions |
| Sample-accurate automation | Gain step automated at offset 37 in a 512-sample block | Change starts at sample 37 ±0 |
| Zipper | Fast automation sweeps of frequency, gain and Q on a sine | Residual below −90 dBFS |
| Bypass | Toggle bypass during playback in each phase mode | No click above −90 dBFS; output null against dry when bypassed |
| State restore | Load a state during playback | No ramp from defaults; first block uses the restored values |
| Validators | pluginval, VST3 validator, clap-validator | Clean |

## Open questions

- Ramp time for moves below the jump threshold: a starting range will be set from zipper tests in Phase 1.
- Should the processor ever emit parameter changes, for example for Auto-gain-driven values in future? Not in v1.
