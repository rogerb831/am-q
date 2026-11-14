# 12 GUI structure and display feedback

Am-Q builds its editor as one GPU-rendered window with a display-paced render loop, dirty-region drawing, and curves computed in the GUI. The audio thread hands data to the GUI only through lock-free snapshots, so the processor never waits on the editor.

## Requirements

1. R1. Am-Q creates its editor only when the host asks for the plugin's editor view. Opening and closing it 100 times in a row leaks no memory, GPU resources or threads.
2. R2. The editor has these regions: a response graph over the analyzer, a band panel, a header and footer, output metering, and analyzer controls. The EQ Match panel opens on demand and is not built at editor start.
3. R3. The band panel's dynamics section shows the side-chain filter range, threshold, attack and release, and a live gain-reduction indicator. The spectral section shows density and tilt.
4. R4. Am-Q does not ship an instance list, minimap or cross-instance jump panel in v1, because inter-instance features are out of scope.
5. R5. The render loop runs at the display refresh rate. If the refresh rate cannot be read, it falls back to a fixed rate chosen by the GUI engineer.
6. R6. A frame that changes no pixels presents nothing. A hidden or minimised editor does not draw.
7. R7. Only dirty regions are redrawn. An idle editor with the analyzer off uses under 1 % of one core.
8. R8. When the editor closes, all GPU resources are released on the thread that owns the context. No GPU call runs after the window is destroyed.
9. R9. The processor never takes a lock shared with the GUI and never blocks on it. GUI stalls of 2 s cause no audio dropouts.
10. R10. The GUI pulls analyzer snapshots about 30 times a second: pre, post and external spectra, meter levels and per-band gain reduction. Each snapshot carries a sequence number so the GUI can skip stale or repeated data.
11. R11. Meters and spectra start at a silence floor. Garbage or stale values never appear when the editor opens.
12. R12. The GUI computes the combined and per-band response curves from parameter values. They match the DSP magnitude response within 0.1 dB from 20 Hz to 20 kHz for every shape and slope.
13. R13. The selected band's curve and node are visually distinct from unselected bands, in a style the Product designer defines.
14. R14. Undo history has a bounded depth, 100 steps or more. It records GUI and EQ Match edits only, as Tab 09 decided.
15. R15. Analyzer display settings (range, resolution, speed, tilt, frequency span) have Am-Q's own defaults and are saved with the state.
16. R16. The editor scales cleanly from 100 to 300 % and can be resized while running.

## Am-Q design

The editor is a JUCE 8 component tree hosted in a GPU context. The ui/ module owns the graph, band panel, header/footer and analyzer bar. EQ Match lives in its own lazily constructed component, as in Tab 16.

A dedicated render driver paces frames to the display refresh. On Windows it uses the compositor's timing. On macOS it uses the display link. Widgets mark rectangles dirty, and the driver skips presentation when the dirty set is empty. Am-Q polls snapshots once per frame inside the render driver at a capped rate, so polling and drawing share one schedule.

Processor-to-GUI data uses lock-free single-producer, single-consumer FIFOs in analysis/. Each snapshot has a sequence number. Curves are computed in the GUI from the same coefficient code in core/dsp, so the drawn curve and the audio cannot drift apart. The gain-reduction indicator reads the per-band gain-reduction array from the latest snapshot. Tab 14 covers this feed.

## Implementation tasks

- [ ] Define the editor region map and the visual hierarchy, including the selected-band style (Product designer, Phase 0)
- [ ] Build the editor shell: view creation, a resizable GPU context, and HiDPI scaling (GUI engineer, Phase 1)
- [ ] Implement a display-paced render driver with a fallback rate, dirty-region tracking and skip-when-idle (GUI engineer, Phase 1)
- [ ] Implement GPU resource teardown on editor close, plus a leak counter for tests (GUI engineer, Phase 1)
- [ ] Implement snapshot types (spectra, meters, per-band gain reduction) with sequence numbers and a silence-floor initial state (DSP lead, Phase 1)
- [ ] Implement lock-free snapshot FIFOs between the processor and the GUI (Plugin and host engineer, Phase 1)
- [ ] Share the curve evaluation code between core/dsp and ui/ (DSP lead, Phase 2)
- [ ] Build the band panel dynamics section with the side-chain range and gain-reduction indicator (GUI engineer, Phase 2)
- [ ] Build the spectral section with density and tilt controls (GUI engineer, Phase 2)
- [ ] Build output metering and analyzer controls bound to Am-Q parameters and settings (GUI engineer, Phase 2)
- [ ] Construct the EQ Match panel lazily on first open (GUI engineer, Phase 3)
- [ ] Add bounded undo history with a depth setting (Plugin and host engineer, Phase 2)
- [ ] Add an editor open/close soak test and an idle-CPU benchmark (Build and QA engineer, Phase 3)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Editor soak | Open and close the editor 100 times under pluginval and in Reaper | No growth in memory, thread count or GPU objects |
| Frame pacing | Log frame timestamps on 60 Hz and 120 Hz displays | Mean interval within 5 % of the refresh period; no frames presented while idle |
| Hidden editor | Minimise the host window for 10 s | Zero frames drawn |
| Idle CPU | Editor open, analyzer off, no edits, 60 s | Under 1 % of one core |
| GUI stall | Block the GUI thread for 2 s during playback | No audio dropouts; no processor lock waits |
| Snapshot freshness | Scripted run that checks sequence numbers | No snapshot drawn twice as new; stale snapshots skipped |
| Initial meters | Open the editor on a silent, freshly loaded instance | Meters and spectra show the floor only |
| Curve accuracy | Compare GUI curve data with DSP magnitude for all shapes, slopes and placements | Maximum error 0.1 dB from 20 Hz to 20 kHz |
| Selected style | Screenshot diff with one band selected | Matches the approved reference image |
| HiDPI | Screenshot diffs at 100, 150, 200 and 300 % | Within the diff threshold; no clipped controls |

## Open questions

- Does Am-Q need a piano-key frequency overlay? It is not in the v1 feature list. The Product designer decides.
- Does Am-Q offer an A/B compare of two states next to undo? This is not decided yet.
- What fallback frame rate applies when the display refresh cannot be read?
