# Am-Q Engineering Plan

Nov 13, 2026 · @Roger Barlow

Am-Q is a 24-band parametric EQ. v1 offers zero-latency, natural-phase and linear-phase modes, per-band dynamics, and an analyzer, in VST3 and CLAP. It is built from public DSP techniques and published sources. The plan below takes an estimated 9 months with a 4-person team, in five phases with a gate at each.

## Goals and non-goals

**v1 ships:**

- **Bands.** 24 bands; 10 shapes (Bell, Low/High Shelf, Low/High Cut, Notch, Band Pass, Tilt Shelf, Flat Tilt, All Pass); slopes 6–96 dB/oct plus Brickwall on cuts.
- **Routing.** Per-band Stereo/Left/Right/Mid/Side placement.
- **Processing modes.** Zero Latency, Natural Phase and Linear Phase, with five resolution steps.
- **Dynamic EQ.** Per band, with auto threshold, attack/release, internal or external side chain and side-chain filtering.
- **Spectral dynamics.** A per-band option.
- **Output.** Gain Scale, Output Level/Pan (L/R or M/S), Invert, Bypass and Auto Gain.
- **Analyzer.** Real-time pre/post analyzer with freeze and collision display.
- **EQ Match.** Against a sidechain, another instance or an audio file.
- **Formats.** VST3 and CLAP on Windows x64 and macOS (Apple Silicon + Intel). Presets, undo/redo, full host automation.

**Not in v1:**

- **Surround/immersive layouts** beyond stereo (the speaker-group model is designed in, but not exposed).
- **AU, AAX and VST2.** AAX needs Avid signing; VST2 is no longer licensable for new products.
- **Oversampling, Character modes, MIDI learn.**
- **Inter-instance features** (instance list, cross-instance EQ Match).

## Clean-room rules

Am-Q implements public techniques from published sources, never from any other product's code, data or presentation.

| Allowed | Not allowed |
| --- | --- |
| Architecture ideas: one shared core, a single parameter setter path, a request/reply display feed | Decompiled pseudocode, assembly or copied constants tables in our repo |
| Public techniques: TPT state-variable filters, matched/fitted analog-to-digital design, FFT convolution, greedy EQ fitting | Porting any third-party product's code, even rewritten line by line |
| Behaviour targets measured by black-box listening and plots (latency, smoothing feel) | Another product's images, icons, fonts, colours, layout or interaction design |
| Our own parameter names, ranges and preset format | Another product's preset files, parameter IDs or state format, unless needed for documented import later and cleared by counsel |

How the wall is kept:

1. **Two groups.** Any reference-product research stays with an analyst who does not write Am-Q code. Engineers get a **spec** written in our own words: behaviours, ranges and cited papers, never addresses or pseudocode.
2. **Sources.** Every DSP component cites a public source in its design doc: papers, textbooks or open-source references under compatible licences.
3. **Review gate.** Legal reviews the spec and the UI design before Phase 2 starts and again before release.
4. **Licences.** Track every third-party dependency (plugin SDKs, framework, FFT library) and its licence terms in `THIRD_PARTY.md`.

This is engineering guidance, not legal advice. Any use of reference-product research should be checked with counsel for our jurisdictions and the relevant licence terms.

## Architecture

Three thread domains share one parameter table. The audio thread never waits on the GUI or the workers, and every writer goes through the same setter.

&#91;embedded content: Am-Q architecture · 8 components, 3 thread domains\]

- **Source tree.** `core/dsp` (framework-free library), `core/params` (table, state, undo), `plugin/` (processor and wrappers), `ui/` (editor), `analysis/` (analyzer, EQ Match), `tests/` (unit, golden-response, fuzz).
- **Core patterns.** A single setter path for every parameter write, block splitting at parameter events, and a display feed paced by the editor. Audio-to-GUI data moves through a lock-free snapshot queue.

## Parameter model and state

All user-facing state lives in one flat, versioned parameter table; the GUI, host automation, presets and EQ Match all write it through a single setter.

- **Layout.** 24 band blocks with a fixed field order, then globals. IDs are stable strings (`band03.freq`) mapped to fixed host indices that never change after release. Leave 8 spare fields per band and 32 spare globals so v2 can add features without renumbering.
- **Band fields.** Used, Enabled, Frequency, Gain, Q, Shape, Slope, Placement, Speakers (reserved), Dyn Range, Dyn Enabled, Dyn Auto, Threshold, Attack, Release, SC External, SC Mode (Band/Free), SC Low, SC High, SC Listen, Spectral, Spectral Density, Spectral Tilt, Solo. Spectral Tilt sits inside the band block.
- **Globals.** Mode, Resolution, Gain Scale, Output Level, Output Pan, Pan Mode, Invert, Bypass, Auto Gain, Solo Gain, analyzer settings.
- **Encodings.** Store in perceptual units so host automation feels linear:

| Value | Stored as | Range | Host 0–1 mapping |
| --- | --- | --- | --- |
| Frequency | log2 Hz | 10 Hz – 30 kHz | linear in log2 |
| Gain, Dyn Range | dB | ±30 dB | linear |
| Q | normalized n, Q = Qmin·(Qmax/Qmin)^n | 0.025 – 40 | n itself |
| Slope | index | 6 … 96 dB/oct, Brickwall | stepped |
| Attack, Release, Density | 0 – 100 | default 50 | linear |
| Output Level | dB | −∞, −36 … +36 dB | curve with a fade below −36 dB |

- **Write path.** `ParameterTable::set(id, plain, source)` clamps, stores and notifies listeners. Sources are GUI, host, preset, EQ Match or undo. The GUI path wraps it in begin/perform/end edit for the host. Undo/redo records only GUI and EQ Match sources.
- **Audio side.** Each parameter feeds a lock-free value slot read once per block. Per-sample ramps run on the audio thread: linear, with a short time for small moves and an instant jump for large ones.
- **State and presets.** A versioned binary chunk (header + version + id/value pairs) for host state, and the same data as JSON for user presets. Unknown ids are ignored on load and missing ids take defaults, so old sessions keep loading.

## DSP design

The DSP core is a standalone C++20 library with no framework dependency, so it can be unit-tested and benchmarked outside any host. Each part below names the public technique it uses.

| Component | Design | Public basis |
| --- | --- | --- |
| Analog prototypes | Each shape as cascaded 2nd-order s-domain sections; order = slope ÷ 6 dB/oct, up to 16 | Standard filter design texts; RBJ Audio EQ Cookbook for the basic shapes |
| Analog → digital | Default: magnitude-matched design that tracks the analog curve to Nyquist (no cramping). Fallback: bilinear with pre-warp | Orfanidis, “Digital Parametric Equalizer Design With Prescribed Nyquist-Frequency Gain”; Vicanek, “Matched Second Order Digital Filters” |
| Filter kernel | TPT/ZDF state-variable filter, double-precision state, float I/O, SIMD across stereo channels | Zavalishin, *The Art of VA Filter Design*; Simper's SVF notes |
| Coefficient updates | Read parameter ramps every 4 samples; redesign at most every 32 samples, or at once on a large frequency jump | Measured against zipper-noise tests (Testing) |
| Order changes | Run old and new cascades together and crossfade over 100 ms | Standard crossfade |
| Natural Phase | IIR band, then a short FIR (512 taps) equal to analog response ÷ digital response, so phase follows the analog filter | Frequency-sampling FIR design; uniform partitioned convolution |
| Linear Phase | Combined zero-phase magnitude of all bands → FIR (4096–131072 taps by resolution) → partitioned FFT convolution | Gardner, “Efficient Convolution without Input-Output Delay”; Wefers, *Partitioned Convolution Algorithms* |
| Dynamic EQ | Band holds static and gain+range filters; a side-chain detector with soft knee picks the blend per sample; auto threshold relative to a slow envelope | Giannoulis, Massberg and Reiss, “Digital Dynamic Range Compressor Design” |
| Spectral dynamics | STFT of the side chain, log-spaced analysis bands set by Density, per-band gain with frequency-scaled times, applied on the FFT path | Standard STFT processing |
| Mid/Side | M = (L+R)/2, S = (R−L)/2, exact inverse; inserted only when a band uses M or S | — |
| Auto Gain | Weighted average of the chain's magnitude over a log grid, inverted and capped at +30 dB | Our own weighting; validate against ITU-R BS.1770 loudness |

Latency rules, set when the mode or the band set changes and reported to the host:

- **Zero Latency:** 0 samples unless a spectral band is active.
- **Natural Phase:** kernel/2 + partition, e.g. 256 + 64 = 320 samples.
- **Linear Phase:** FIR/2 + partition, scaled with sample rate.

Rebuilds happen off the audio thread. The audio thread swaps in the new kernel at a block boundary, with no allocation or locks in `process`.

The papers and books above are cited from memory, not looked up; the DSP lead confirms each title and edition when writing the spec.

## GUI design

The editor is a resizable, GPU-rendered view built around one large response graph. Every interaction ends in the same parameter setter, and the editor never touches DSP state directly.

**Layout (our own design, reviewed by Legal before build):**

- **Response graph.** Bands are drawn as draggable nodes over the analyzer, with the combined curve and per-band curves computed in the GUI from parameter values.
- **Band panel.** Opens for the selected band: shape, slope, placement, Freq/Gain/Q, the dynamics section (threshold/auto, attack, release, range, side chain, listen) and the spectral section (on, density, tilt).
- **Header and footer.** Presets, undo/redo, mode and resolution, analyzer options, output level/pan/pan mode, Auto Gain, bypass.

**Interactions to implement and test:**

| Input | Action |
| --- | --- |
| Double-click empty graph | Create a band at that frequency and gain |
| Drag node | Frequency (x) and gain (y); Q for gainless shapes; modifier for fine 0.1× |
| Drag range handle | Dynamic range |
| Wheel on node | Q; modifiers for gain, range, slope |
| Click, Shift/Ctrl-click, lasso | Select, add, toggle, box-select; drags apply to the whole selection |
| Alt-click node | Enable/disable band |
| Right-click | Band menu: enable, invert gain, make dynamic/spectral, shape, slope, placement, copy/paste, split, delete |
| Delete / Backspace | Delete selected bands. All other keys pass on to the host |

**Rendering and threads.**

- **Drawing.** A render loop paced to the display refresh rate, drawing only dirty regions through the framework's GPU backend.
- **Analyzer data** comes from the processor by pull: about 30 times a second the editor asks the processor for a snapshot (spectra, meters, per-band gain reduction). In-process this is a lock-free FIFO read; across processes it is a VST3 message or a CLAP timer read. The processor never blocks on the GUI.
- **Scaling and accessibility.** HiDPI scaling at 100–300 %, keyboard navigation of the band list, and screen-reader names for every control.

## Analyzer and EQ Match

One analyzer engine serves the live display and EQ Match. It runs on a worker thread fed by a lock-free copy of the audio, never in `process`.

**Analyzer.**

- **Windowing.** STFT with 75 % overlap and a Kaiser or Blackman-Harris window. FFT size is set by the analyzer resolution (2048–16384).
- **Display.** Log-frequency resampling, a selectable tilt (default 4.5 dB/oct), and rise/fall ballistics set by the speed setting.
- **Sources.** Pre, post and side-chain spectra; freeze and peak-hold.
- **Collisions.** Where pre and side-chain energy overlap, shown as a separate layer.

**EQ Match.** Averaging can forget old frames (whole capture or last N seconds), and the two captures are loudness-aligned before the difference is taken.

1. **Capture.** Input and reference (sidechain, another instance in v2, or an audio file) are averaged by power, not linear magnitude. Frames below −90 dBFS are skipped. A selectable window (whole capture or last N seconds) replaces never-forgetting averaging.
2. **Level alignment.** Subtract the difference in loudness-weighted mean level before fitting, so a louder reference doesn't become a broadband shelf. Make this a visible option.
3. **Smoothing.** Fractional-octave smoothing (1/3 to 1/24, user selectable) of the difference curve in dB.
4. **Fit.** Greedy band fitting: each round tries cuts, shelves and bells at the largest residual peaks. It keeps the candidate that most reduces mean-squared dB error, refines Q, frequency and gain by line search, and stops at a band limit or when the error stops improving. Respect the band limits from Parameter model and state.
5. **Apply.** An Amount slider (0–100 %) scales the fitted gains, plus a Bands slider (strongest first). Results are written as normal bands through the setter, as one undo step.

EQ Match runs on a worker thread and shows progress, so the GUI never stalls.

## Formats, framework and host integration

Recommendation: JUCE 8 for wrappers and GUI, with the DSP core and parameter table as our own framework-free libraries. JUCE gets VST3 and CLAP shipping fastest and leaves AU and AAX open for v2. The alternative is a CLAP-native core wrapped to VST3 with clap-wrapper and a custom GPU UI: no framework licence, but about 2 extra months.

| Concern | Requirement |
| --- | --- |
| Formats | VST3 and CLAP (CLAP via the clap-juce-extensions project if JUCE). Confirm current licence terms for the VST3 SDK, JUCE and every wrapper before Phase 1 ends. |
| Platforms | Windows 10+ x64; macOS 12+ universal binary, signed and notarized |
| Buses | Main stereo or mono in/out; optional stereo side-chain input bus |
| Parameter events | Sample-accurate: split each block at every parameter-change offset |
| Gestures | begin/perform/end edit (VST3) and gesture begin/end (CLAP) around every GUI drag, for clean automation |
| Latency | Report on load and on every change: VST3 `restartComponent(kLatencyChanged)`, CLAP latency extension with a host restart request |
| Bypass | Host bypass parameter mapped to our smoothed, latency-compensated wet/dry bypass |
| Precision and rates | 32-bit float processing with double-precision filter state; 44.1–384 kHz; any block size including 1 |
| Threads | No allocation, locks or I/O on the audio thread; kernel rebuilds and EQ Match on worker threads; CLAP thread-check passes |
| State | Versioned chunk saved and restored by host projects; presets as files in a user folder |
| GUI to processor | Pull model with a lock-free snapshot queue; nothing in the GUI may block `process` |

## Testing and verification

Every DSP block gets an automated accuracy test against its analog target, and every build runs host-conformance checks in CI on both platforms. These pass/fail thresholds are proposals for the DSP lead to confirm:

| Area | Test | Pass criterion (proposed) |
| --- | --- | --- |
| Filter accuracy | Measured magnitude of each shape × slope × frequency grid vs the analog prototype, at 44.1, 48, 96 kHz | Zero Latency, per revised area 01 R12: ≤ 0.1 dB below 0.9·Nyquist for f0 ≤ 0.9·Nyquist (levels below −60 dB need only stay below −60 dB); ≤ 1.1 dB for f0 > 0.9·Nyquist with Q ≤ 3; TBD for Q > 3. ≤ 0.05 dB (Natural, Linear) |
| Phase | Natural Phase vs analog phase; Linear Phase symmetric impulse | ≤ 2° below 0.9·Nyquist; exact symmetry |
| Latency | Impulse through each mode and resolution; compare with reported latency | Exact match in samples |
| Zipper noise | Fast automation sweeps of frequency, gain and Q; measure distortion above the clean output | Below −90 dBFS residual |
| Null tests | All bands flat, bypass and mode switches with latency compensation | Bit-exact or ≤ −140 dBFS |
| Dynamics | Step and tone-burst inputs vs specified attack, release, knee and range | Times within ±5 % |
| EQ Match | Match known synthetic EQ curves | RMS error ≤ 0.5 dB over 20 Hz–20 kHz |
| Stability | Fuzz parameters at random rates, denormal inputs, NaN injection | No NaN or Inf output, no denormal CPU spikes |
| CPU | 24 bands, Zero Latency, 48 kHz, 64-sample blocks | Target ≤ 3 % of one core on a 2020-era laptop |
| Host conformance | pluginval (strictest level), Steinberg VST3 validator, clap-validator | Clean pass in CI |
| Hosts | Manual matrix: Reaper, Ableton Live, Cubase, Bitwig, Studio One, FL Studio, Logic (v2 AU), Pro Tools (v2 AAX) | Load, automate, save/restore, offline render match |

- **Golden files.** Store measured responses per release and fail CI on unexpected drift.
- **GUI tests.** Scripted interaction tests drive the parameter setter path; a screenshot diff catches layout regressions.
- **Dogfooding.** A 4-week beta with about 20 engineers before release, tracking crashes through opt-in crash reports.

## Milestones

Five phases of about 9 months in total, each ending in a gate the next phase depends on. Durations are planning estimates for the four-engineer team, not commitments.

&#91;embedded content: Am-Q roadmap · 5 phases, 5 gates, about 9 months (estimates)\]

The riskiest technical work is front-loaded: the matched filter design and the accuracy harness in Phase 0, and the latency modes in Phase 2. A miss there shows up before the GUI and analysis work begins.

## Team, risks and open decisions

The plan assumes four full-time engineers plus part-time design, legal and a walled-off analyst.

| Role | Owns |
| --- | --- |
| DSP lead | DSP core, filter design, modes, dynamics, EQ Match engine, accuracy tests |
| Plugin and host engineer | Parameter table, state, wrappers, latency and bypass, host matrix |
| GUI engineer | Editor, graph interactions, analyzer display, scaling and accessibility |
| Build and QA engineer | CI on Windows and macOS, signing and notarization, validators, fuzzing, beta |
| Product designer (part-time) | Visual and interaction design, original to Am-Q |
| Analyst (part-time, walled off) | Maintains the clean-room behaviour spec; writes no code |
| Counsel (as needed) | Clean-room process, licences, release review |

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Clean-room contamination | Legal exposure; possible rewrite | Analyst/engineer wall, cited sources, legal gates before Phase 2 and release |
| Matched design misses accuracy at high Q near Nyquist | Audible mismatch vs analog | Prototype the design in Phase 1; fall back to oversampled design for extreme settings |
| Linear Phase at Maximum resolution is too slow or too latent | CPU spikes; long latency | Non-uniform partitioned convolution; rebuild kernels off-thread; benchmark in Phase 2 |
| Spectral dynamics cost per band | CPU with many spectral bands | Share one side-chain STFT across bands with the same source |
| Host quirks (latency changes, bypass, threading) | Bugs only in some hosts | Validators in CI plus the manual host matrix from Phase 3 |
| GUI smoothness on low-end GPUs | Laggy graph | Dirty-region drawing, frame-rate cap, software fallback |
| Framework licence terms change | Cost or licence conflict | Keep DSP and parameter code framework-free; confirm terms in Phase 1 |

Open decisions:

- [ ] JUCE 8 or a CLAP-native core with clap-wrapper and a custom UI (Formats, framework and host integration)
- [ ] Is AU in v1 or v2? Logic users would need it at launch.
- [ ] Final CPU target and minimum hardware
- [ ] Visual identity and naming of shapes and modes (designer + counsel)
- [ ] Whether importing third-party EQ presets is ever in scope (counsel)

## Area plan index

This file is the index. The detail lives in the area plans under `areas/`: one plan per area, in signal-flow order. Each area plan has requirements, design, tasks, acceptance tests and open questions. Per the clean-room rules, area plans describe behaviour and Am-Q's own design.

| # | Area plan | Feeds index sections |
| --- | --- | --- |
| 01 | [01 Core DSP and host path](areas/01-core-dsp-and-host-path.md) | DSP design, Parameter model, Formats |
| 02 | [02 Routing, phase modes, latency](areas/02-routing-phase-modes-latency.md) | DSP design, Formats |
| 03 | [03 Mid/Side, output stage, Natural Phase](areas/03-mid-side-output-stage-natural-phase.md) | DSP design |
| 04 | [04 Output Pan Mode and Natural Phase target](areas/04-output-pan-mode-and-natural-phase-target.md) | DSP design |
| 05 | [05 Natural Phase kernel and Auto Gain weights](areas/05-natural-phase-kernel-and-auto-gain-weights.md) | DSP design |
| 06 | [06 Dynamic EQ timing](areas/06-dynamic-eq-timing.md) | DSP design |
| 07 | [07 Spectral dynamics and auto-threshold](areas/07-spectral-dynamics-and-auto-threshold.md) | DSP design |
| 08 | [08 Small gaps](areas/08-small-gaps.md) | DSP design, Parameter model |
| 09 | [09 Parameter write path](areas/09-parameter-write-path.md) | Parameter model, Architecture |
| 10 | [10 Bindings, Q mapping, ramps](areas/10-bindings-q-mapping-ramps.md) | Parameter model, GUI design |
| 11 | [11 Control bindings, hand-off, Output Level](areas/11-control-bindings-hand-off-output-level.md) | GUI design, Parameter model |
| 12 | [12 GUI structure and display feedback](areas/12-gui-structure-and-display-feedback.md) | GUI design, Architecture |
| 13 | [13 GUI input and control bindings](areas/13-gui-input-and-control-bindings.md) | GUI design |
| 14 | [14 Graph menu, band delete, gain-reduction feed](areas/14-graph-menu-band-delete-gain-reduction-feed.md) | GUI design, Formats |
| 15 | [15 Shape families, keyboard, Shape menu](areas/15-shape-families-keyboard-shape-menu.md) | GUI design |
| 16 | [16 EQ Match fitting and apply](areas/16-eq-match-fitting-and-apply.md) | Analyzer and EQ Match |
| 17 | [17 EQ Match spectrum capture](areas/17-eq-match-spectrum-capture.md) | Analyzer and EQ Match |
| 18 | [18 EQ Match averaging](areas/18-eq-match-averaging.md) | Analyzer and EQ Match |
| 19 | [19 EQ Match analyzer engine](areas/19-eq-match-analyzer-engine.md) | Analyzer and EQ Match |
| 20 | [20 EQ Match difference curve](areas/20-eq-match-difference-curve.md) | Analyzer and EQ Match |
| 21 | [21 Runtime behaviour](areas/21-runtime-behaviour.md) | Testing, GUI design, Formats |
