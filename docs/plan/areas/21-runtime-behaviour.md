# 21 Runtime behaviour

Every GUI edit uses the host edit path, and the analyzer is pulled about 30 times a second. Mode and spectral changes alter latency at block boundaries. It always notifies the host of latency changes, deletes bands from the keyboard, and keeps every band field in its band block.

## Requirements

1. R1. Am-Q's parameter table has 24 band blocks with the agreed band fields, Spectral Tilt included, plus a global block. Tab 09 owns the table. Lookups are bounds-checked, and an out-of-range id or index returns an error.
2. R2. Global parameters cover: processing mode and resolution, gain scale, output level, pan and pan mode, invert phase, bypass, Auto Gain, analyzer pre/post/external options, range, resolution, speed, tilt, freeze, collisions, spectrum grab, display range and solo level.
3. R3. Character and MIDI receive are not in v1. Their ids are reserved, not exposed.
4. R4. Parameters are typed as continuous, two-state or choice. Each type has one setter path and a defined plain-to-normalised mapping.
5. R5. Every GUI edit goes through set(id, plain, GUI) and the host edit transaction on the GUI thread. That includes graph drags, panel knobs and toggles, menu actions, solo and band creation.
6. R6. Global toggles (bypass, analyzer show options, freeze, collisions, grab) use the same control binding as band toggles.
7. R7. Making a band dynamic writes Dyn Range. Toggling solo writes Solo. Both go through the setter.
8. R8. Creating a band writes all its defaults in one undo step. The defaults are: side-chain filter fully open, Spectral Tilt off, then the gain from the gesture.
9. R9. Delete and Backspace delete the selected bands (Tab 15). The test runs with bands actually selected.
10. R10. The GUI pulls analyzer and meter snapshots about 30 times a second. The processor never serialises data on demand on the audio thread.
11. R11. Zero Latency mode reports 0 samples. Natural Phase and Linear Phase report Am-Q's exact latency for the current mode, resolution and sample rate (Tabs 02 and 05).
12. R12. Mode and resolution changes apply at a block boundary on the audio thread. The host is always notified of the new latency.
13. R13. If spectral processing adds latency in Zero Latency mode, Am-Q reports that latency exactly and notifies the host on every toggle. Tab 07 decides whether it adds latency at all.
14. R14. A state or preset load writes all parameters in one batch with source "preset". It sends no per-parameter undo entries and produces no audible zipper.
15. R15. Choice parameters such as Shape and SC Mode keep readable values in host automation lists and in the state.
16. R16. Am-Q has no dead parameter hooks. Every parameter interface method has a caller or a test.

## Am-Q design

The parameter table follows the decisions in Tab 09. Spectral Tilt sits inside each band block (band03.spectralTilt). Host ids are stable hashes of the string id, so block layout never affects automation. Typed parameter classes share one setter, which converts from normalised values in-process too, so the GUI and host paths are the same code.

Mode, resolution and spectral changes post a request to the audio thread. The thread rebuilds its kernels off-line and swaps them in at the next block start. It then raises the format's latency-changed notification (the VST3 restart flag or CLAP latency changed). A missing notification is a bug. Preset loads use a batch API on the setter that marks the source and coalesces host notifications.

The snapshot pull runs on the GUI render schedule. In-process it reads the lock-free FIFO. In separate processes it uses the VST3 message or the CLAP timer, as decided. The processor writes snapshots during process() without allocating, and never serialises in response to a request.

## Implementation tasks

- [ ] Finalise the parameter table with Spectral Tilt in the band block, reserved ids and stable host ids (Plugin and host engineer, Phase 1)
- [ ] Add bounds-checked lookups with error results (Plugin and host engineer, Phase 1)
- [ ] Implement typed parameters (continuous, two-state, choice) behind one setter (Plugin and host engineer, Phase 1)
- [ ] Route all GUI controls, global toggles, menu actions, solo and band creation through the setter (GUI engineer, Phase 2)
- [ ] Implement band-create defaults in one undo step (Plugin and host engineer, Phase 2)
- [ ] Implement block-boundary mode, resolution and spectral switching with a latency-changed notification (DSP lead, Phase 2)
- [ ] Implement batch preset and state load with coalesced notifications (Plugin and host engineer, Phase 2)
- [ ] Implement snapshot pull at about 30 per second in-process and across processes (Plugin and host engineer, Phase 2)
- [ ] Add a write-path trace test that asserts the source and transaction for every control (Build and QA engineer, Phase 3)
- [ ] Add a latency matrix test across modes, resolutions, sample rates and spectral on/off (Build and QA engineer, Phase 3)
- [ ] Add a coverage check that flags parameter interface methods with no caller or test (Build and QA engineer, Phase 3)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Table shape | Enumerate parameters through VST3 and CLAP | 24 complete band blocks plus globals; no reserved ids exposed |
| Bounds | Query ids and indices past the end | Error result; no crash or garbage |
| Write path | Scripted edit of every control with a setter trace | Each write has source GUI, one transaction, on the GUI thread |
| Global toggles | Toggle bypass and every analyzer option | Same binding path; host sees one edit each |
| Band create | Create a band by double-click and by panel control | Same defaults; one undo step |
| Keyboard delete | Select bands, press Delete and Backspace in each matrix host | Bands deleted |
| Snapshot rate | Count snapshot pulls over 10 s, in-process and sandboxed | 27 to 33 per second |
| Latency values | Measure impulse delay for each mode, resolution and sample rate | Reported latency equals measured delay exactly |
| Latency notify | Switch modes and toggle spectral during playback in each host | Host receives a latency-changed notice each time; compensation is correct |
| Block-boundary switch | Switch modes mid-block under a sample-accurate test harness | Change takes effect at a block start; no clicks above -90 dBFS |
| Preset load | Load a preset during playback | One undo-free batch; no zipper noise; all values correct |
| Choice display | Read Shape and SC Mode in host automation lists | Readable names shown |

## Open questions

- Should spectral bands in Zero Latency mode add latency, or should Am-Q use a zero-latency spectral method? Tab 07 decides.
- Should the solo listening level be a host-automatable parameter or a GUI-only setting?
