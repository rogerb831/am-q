# 01 Core DSP and host path

Am-Q is one framework-free C++20 DSP core behind a CLAP-native plugin, wrapped to VST3 by clap-wrapper ([D1](../decisions.md)). It runs 24 serial bands built from cascaded analog prototypes, converts them to digital with a magnitude-matched design, and applies every parameter change sample-accurately and without clicks.

## Requirements

1. **R1 Formats.** Ship VST3 and CLAP for Windows x64 and macOS universal from one shared core. Category is EQ. AU, AAX and VST2 are not in v1.
2. **R2 Side-chain bus.** Both formats expose a dedicated, optional stereo side-chain input bus. Am-Q never treats extra main inputs as side chain.
3. **R3 Sample-accurate changes.** Split every host block at each parameter-change offset in both VST3 and CLAP. Inside each piece, values move by per-sample ramps.
4. **R4 Precision.** Process 32-bit float I/O with double-precision filter state. If a host requests 64-bit I/O, convert at the boundary and still produce correct audio.
5. **R5 Rates and blocks.** Support 44.1–384 kHz and any block size, including 1 sample and sizes that vary from call to call.
6. **R6 Global bypass.** Bypass is a smoothed wet/dry crossfade. The dry signal is delayed to match the reported latency. Host bypass maps to the same mechanism.
7. **R7 Band order.** The 24 bands run in series, in band order. A band runs on a channel only when it is enabled and its placement matches that channel. Turning a band on or off is crossfaded.
8. **R8 Band parameters.** These ranges are user-visible and match the agreed encodings: Frequency 10 Hz – 30 kHz (default 1 kHz); Gain ±30 dB; Q 0.025–40 (default 1); Dynamic Range ±30 dB; Threshold −90 to 0 dB, with Auto; Attack, Release and Density 0–100 (default 50); side-chain filter Band or Free, 10 Hz – 20 kHz.
9. **R9 Shapes.** There are ten shapes: Bell (the default), Low Shelf, Low Cut, High Shelf, High Cut, Notch, Band Pass, Tilt Shelf, Flat Tilt and All Pass. The parameter table holds no hidden shapes.
10. **R10 Slopes.** Slopes are 6, 12, 18, 24, 30, 36, 48, 72 and 96 dB/oct, plus Brickwall on Low Cut and High Cut only. Filter order is slope ÷ 6. Bell, Band Pass and Notch have a minimum order of 2.
11. **R11 Global modes.** Processing mode is Zero Latency, Natural Phase or Linear Phase. Resolution is Low, Medium, High, Very High or Maximum.
12. **R12 Design accuracy.** (Revised 2026-11-14, approved by the project owner after the Phase 0 accuracy study.) In Zero Latency, with no high-frequency cramping:
    - For a band with f0 ≤ 0.9·Nyquist, the magnitude is within 0.1 dB of its analog prototype at every frequency below 0.9·Nyquist.
    - Wherever the analog response is below −60 dB, the digital response only has to stay below −60 dB.
    - For a band with f0 > 0.9·Nyquist and Q ≤ 3, the tolerance is 1.1 dB below 0.9·Nyquist. For f0 > 0.9·Nyquist and Q > 3 the tolerance is **TBD** until R14 fixes the Q mapping.
    - All Pass keeps its phase exact at the centre frequency (within 2°).
    - The digital design may add one correction biquad to a band (digital order N + 2, analog prototype order unchanged at slope ÷ 6) only when the order-N design misses this tolerance.
13. **R13 Gain law.** dB converts to linear as 10^(dB/20) everywhere. Effective band gain, after Gain Scale, is clamped to ±30 dB.
14. **R14 Q convention.** Am-Q documents one mapping from displayed Q to section Q per shape. Tilt Shelf and Flat Tilt use their own Q curves. Flat Tilt limits its gain relative to a fixed mid-band reference frequency.
15. **R15 Coefficient updates.**
    - Parameter streams are read every 4 samples.
    - Coefficients are redesigned at most every 32 samples, or at once on a large frequency jump.
    - The zipper residual stays below −90 dBFS.
16. **R16 Near-zero bands.** A band whose gain is near 0 dB fades to bypass instead of running a near-flat filter. A bypassed band is bit-transparent.
17. **R17 Structure changes.** When a band's section count changes, the old and new filters run in parallel and crossfade over 100 ms with no click.
18. **R18 Filter kernel.** Use a TPT/ZDF state-variable filter with double state and float I/O. It has mono, linked-stereo and independent-stereo variants, using SIMD across channels.
19. **R19 Real-time safety.** The process call never allocates memory, takes a lock or blocks. Redesigns and kernel builds that may block run off the audio thread.
20. **R20 Time definition.** Every attack, release and smoothing time means the time to cover 90 % of a step. This is documented for users and tests.
21. **R21 CLAP.** The CLAP build supports the params extension with gestures; state; latency, with a changed notification; audio ports, including the side chain; and render. It passes clap-validator with no errors.
22. **R22 Own FFT and convolution.** FFT and partitioned convolution sit behind our own interface in core/dsp. No GPL or framework dependency is allowed.
23. **R23 Routing and latency (summary).** Latency depends only on processing mode, resolution, sample rate and whether a spectral band is active. It is reported exactly. Detail: Tab 02 “Routing, phase modes, latency”.
24. **R24 Mid/Side and output stage (summary).** The Mid/Side matrix is exact and inserted only when needed. The output stage has Gain Scale, Level, Pan, Invert and Auto Gain. Detail: Tab 03 “Mid/Side, output stage, Natural Phase”.
25. **R25 Output Pan Mode (summary).** Pan works as an L/R or an M/S balance. Detail: Tab 04 “Output Pan Mode and Natural Phase target”.
26. **R26 Natural Phase and Auto Gain weighting (summary).** Natural Phase is the IIR plus a short correction FIR towards the analog response. Auto Gain uses our own loudness-validated weighting. Detail: Tab 03, Tab 04 and Tab 05 “Natural Phase kernel and Auto Gain weights”.
27. **R27 Dynamic EQ (summary).** Each band has its own dynamics: auto threshold relative to the band's own slow envelope; soft knee and range cap; attack and release that adapt to the band; and interpolation between the static and dynamic responses. Detail: Tab 06 “Dynamic EQ timing”.
28. **R28 Spectral dynamics (summary).** An STFT side chain uses Density-controlled log bands with per-band, frequency-dependent times. Detail: Tab 07 “Spectral dynamics and auto-threshold”.
29. **R29 EQ Match (summary).** EQ Match captures long-term reference and input spectra, takes their difference, fits bands greedily into free bands, and lets the user choose how many fitted bands to apply. Detail: Tabs 16–20.
30. **R30 GUI (summary).** The editor covers graph editing with mouse and wheel, a context menu and band delete; a band panel with the Shape menu; analyzer feedback at display rate; and gain-reduction meters. Detail: Tabs 12–15.
31. **R31 Parameter path (summary).** There is one setter with gestures and a fixed host index table. Normalised values map to plain values through the agreed encodings. Detail: Tabs 09–11.
32. **R32 Runtime behaviour (summary).** Host-visible behaviour (latency reporting, parameter count, keyboard handling, preset load) meets the runtime requirements. Detail: Tab 21 “Runtime behaviour”.
33. **R33 Surround-ready model.** Each band carries a speaker-group field (default All), and channels carry a speaker descriptor. v1 ships mono and stereo only.

## Am-Q design

**Formats and host path.** plugin/ is a CLAP plugin built on clap-helpers. clap-wrapper's clap-first build produces the VST3 from the same static library, and both formats call one core/dsp processor entry. The plugin walks the host's event queue and splits the block at each change offset. It then calls the core per piece, with values already applied as ramp targets. Side chain is a real auxiliary bus in both formats. Double-precision host buffers are converted to float at the wrapper edge.

**Bypass.** Bypass uses a latency-compensated wet/dry crossfade inside the core. The dry path uses a delay line sized to the reported latency, so toggling bypass never shifts timing. VST3 bypass is a dedicated parameter with the bypass flag. CLAP bypass is a dedicated parameter with the bypass flag set.

**Band model.** Bands are a fixed array of 24, processed in index order. Each channel builds two serial lists per placement stage: zero-latency IIR bands and FFT bands. The lists are rebuilt off the audio thread and swapped at a block boundary. Band enable is a per-band, per-channel gain ramp.

**Filter design.** Each shape is a cascade of second-order analog sections, with order = slope ÷ 6. The default analog-to-digital map is a magnitude-matched design that matches the analog magnitude up to Nyquist (Orfanidis 2005; Vicanek 2016). The fallback is bilinear with frequency pre-warp. All Pass uses the bilinear design, pre-warped at its centre frequency, because only its phase matters. Brickwall is our own steep design, tuned by measurement.

**Kernel.** The kernel is a TPT/ZDF SVF (Zavalishin 2012; Simper 2013) with double state, float I/O, and SIMD lanes across channels. Each designed digital biquad maps exactly onto an SVF plus output mix: substitute z = (1 + σ)/(1 − σ) and match the denominator and numerator (`core/dsp/include/amq/dsp/svf.hpp`). A bilinear first-order section's cancelling pair at z = −1 is divided out first.

**Design tiers (Phase 1).** The joint-refined design that meets R12 costs 1–66 ms per band, and the least-squares design 0.1–3 ms. Neither fits the audio thread at a 32-sample cadence.
- **Audio thread:** the allocation-free three-point matched design, about 1 µs per band (`designBandRealtime`). All Pass uses its bilinear design. A stability fallback to pre-warped bilinear exists, but no case in the 162,000-case sweep needed it.
- **Worker thread** (`core/engine/design_worker.hpp`): the refined design (the Phase 0 default, with the conditional correction section), requested once a band's ramps stop moving.
  - **Hand-off.** It goes through per-band sequence-locked slots: the audio thread only stores and checks an id, and never waits.
  - **Swap.** The result is swapped in 50 ms after the request (PROVISIONAL), so normal playback is repeatable. The coefficients glide over 20 ms (PROVISIONAL).
  - **Pairing.** Sections are paired by proximity in log g and log k. When the correction section adds or removes one, the shorter cascade is padded with identity SVF sections (mix 1, 0, 0: output equals input exactly), and padding left as identity is dropped exactly. A settled band therefore meets R12 about 70 ms after it stops moving.
  - **Measured.** Through the engine, a 6 kHz Bell (+12 dB, Q 0.7) at 44.1 kHz is 0.52 dB off with the fast design and 0.004 dB after the swap. The largest per-sample jump in the swap difference is 0.0025 (`tests/engine/test_refine.cpp`).
- **Offline render** (CLAP render mode offline): the refined design runs at once on the audio thread, so renders are exact and identical for any block size. Mono, linked-stereo and independent-stereo variants share one template.

**Smoothing.** Ramps are read every 4 samples, and coefficients are redesigned at most every 32 samples. Each redesign targets the ramp values one interval ahead (the ramps are deterministic), and the SVF's g, k and mix gains glide there per sample. Any g > 0, k > 0 is stable, so the glide is safe, and the coefficient path follows the ramp instead of stepping. Measured zipper residual against a per-sample design: −94 dBFS on a 100 Hz → 10 kHz sweep in 0.5 s (`tests/engine/test_bands.cpp`). A large frequency jump forces an immediate redesign and a short internal crossfade. A section-count change runs old and new cascades in parallel for a 100 ms equal-gain crossfade. Near-zero gain bands fade to bypass through a wet level.

**Conventions.** One header in core/dsp defines dB↔linear, time-to-coefficient (90 % step time) and the log2 frequency encoding. Every module uses it.

**CLAP and VST3.** The plugin implements the CLAP extensions directly. Latency changes go through the latency extension and a host restart request; clap-wrapper turns them into VST3 `restartComponent(kLatencyChanged)`.

**Threading across formats.** A VST3 host may call through clap-wrapper on threads CLAP would not allow (Blue Cat's Axiom calls `getLatencySamples()` off the main thread). Every callback a VST3 interface can reach is therefore safe on any thread:
- latency, parameter info, value and text, audio ports and GUI size read atomics or constant data;
- state load publishes values for the audio thread and defers host notifications and editor updates to the main thread.

Shipping builds use clap-helpers' `Ignore` handler with checking off: its checks allocate and log on the calling thread, and `Terminate` aborts the host. A development build option turns on `Terminate` with maximal checking. The spike's `clap_host_test` case 3 is the pattern for the regression test.

**Design commitments.**

- Sample-accurate splitting in every format.
- A working 64-bit I/O path.
- No hidden shape values.
- A documented time definition.
- Latency independent of the placement mix.
- Every VST3-reachable callback safe on any thread; nothing in a shipping build aborts the host.

## Implementation tasks

- [x] Set up the CMake project: CLAP plugin on clap-helpers, VST3 via clap-wrapper, Visage editor, framework-free core libraries, pinned dependency versions (Build and QA engineer, Phase 1). Done on Windows on 2026-11-14 (`plugin/`, `ui/`, `cmake/AmqPluginDeps.cmake`). macOS is the separate first-build task below.
- [x] Add the host-check build option: shipping uses `Ignore` with checks off, development uses `Terminate` with maximal checks; CI builds both (Build and QA engineer, Phase 1). Done on 2026-11-16: `AMQ_STRICT_HOST_CHECKS`; CI (`.github/workflows/ci.yml`) builds both configurations on Windows. The strict build passes ctest and clap-validator (33 passed, 0 failed, 1 warning: the known denormal timing warning)
- [ ] Audit every VST3-reachable callback for any-thread safety, and add an off-main-thread regression test in our CLAP and VST3 test hosts (Plugin and host engineer, Phase 1). Progress: the skeleton's callbacks are any-thread safe. `tests/plugin/plugin_host_test.cpp` covers them in CLAP, and `tests/plugin/vst3_host_test.cpp` (built in `tools/vst3_tools/`, run in CI) covers them in VST3 through clap-wrapper: latency, buses, parameter info, value text, getState and setState, and the editor size, called from a second thread, with the state intact afterwards. The VST3 test also checks buses, bit-exact pass-through, the side chain, the 573 frozen parameter ids, exact-sample automation and a state round trip. Findings: clap-wrapper v0.17.0 offers VST3 32-bit processing only, and does not convert between plain and normalized values (hosts use value text). Still to do: the latency-change notification once a mode can change the latency, and re-auditing as parameters and state are added.
- [ ] Check the editor in Reaper or Studio One on a local high-refresh display: open, drag, frame pacing, key pass-through (GUI engineer, Phase 1, early). Progress (2026-11-14): Am-Q's VST3 (format inferred: Fender Studio hosts VST3), with the Phase 1 editor, works in Fender Studio on the user's local display. The user reports that everything worked: load, editor, double-click create, drag, wheel Q, Alt-click enable, and automation record and playback. Still to do: frame-pacing measurement on a high-refresh display, and key pass-through once the key dispatcher exists (area 15)
- [ ] First macOS build (universal binary, CLAP and VST3) with the Visage Metal backend; load it in a host and in the validators (Build and QA engineer, Phase 1, early)
- [x] Write the core processor entry with block splitting at parameter-change offsets for VST3 and CLAP (Plugin and host engineer, Phase 1). `core/engine` is the entry, and `plugin/` splits at every CLAP event offset (VST3 via clap-wrapper). Exact-sample automation is tested on the plugin binary
- [x] Add a side-chain bus and 64-bit-to-float conversion at the wrapper edge (Plugin and host engineer, Phase 1). Input port 1 is a stereo, non-main side-chain bus (64-bit supported, never in place); clap-wrapper exposes it as a VST3 aux bus. Only that port is ever used as side chain. The engine takes it per piece, keeps it off the main path and meters it (levels snapshot, `sidechainChannels` 0 when absent); the dynamics (area 06) and the analyzer (area 19) will read it. Tested in our CLAP host: with dynamics off, a loud 32-bit, 64-bit, mono, absent or inactive side chain leaves the main output bit-exact. Note: clap-wrapper marks the VST3 aux bus default-active; hosts still decide whether to route anything to it
- [x] Implement latency-compensated smoothed global bypass and map host bypass to it (Plugin and host engineer, Phase 2). Done on 2026-11-16 in `core/engine`: the last stage crossfades from the processed output to the dry input, delayed by exactly the reported latency (a ring of that length per channel), over 20 ms (PROVISIONAL, 90 % time). Fully bypassed, the output is the delayed input bit for bit; the wet path keeps running, so un-bypassing crossfades cleanly. The host bypass parameter (CLAP_PARAM_IS_BYPASS, VST3 kIsBypass through clap-wrapper) drives it. Tested: bit-exact null in Zero Latency and Natural Phase; the crossfade matches the ideal linear blend within 1e-6; the CLAP and VST3 host tests bypass through the host parameter and get the input exactly 320 samples late in Natural Phase
- [x] Implement the analog prototype library for all ten shapes as cascaded second-order sections (DSP lead, Phase 0). Done in Phase 0 (`core/dsp/analog_prototype.{hpp,cpp}`, `tests/test_analog_prototype.cpp`; see `docs/dsp/phase0-design-notes.md`). Brickwall is the separate Phase 2 task below
- [x] Implement the magnitude-matched design with a bilinear pre-warp fallback, and the All Pass bilinear path (DSP lead, Phase 0). Done in Phase 0 (`core/dsp/digital_design.{hpp,cpp}`, `joint_fit.cpp`, `tests/test_digital_design.cpp`; accuracy in `docs/dsp/phase0-accuracy-report.md`)
- [ ] Design and tune a brickwall prototype for the cut shapes by measurement (DSP lead, Phase 2)
- [ ] Define and document the displayed-Q to section-Q mapping per shape, including the tilt Q curves and the Flat Tilt gain limit (DSP lead and Product designer, Phase 0)
- [ ] Implement the TPT SVF kernel with mono, linked and independent variants and SIMD lanes (DSP lead, Phase 1). Progress: the kernel, the exact biquad-to-SVF mapping, and linked-stereo and single-channel bands are done. Still to do: SIMD, and the independent-stereo variant (needed for M/S and per-channel dynamics)
- [x] Design bands on a worker thread with the refined (R12) design and swap them in when a band settles; crossfade if the section count changes (DSP lead, Phase 1). Section-count changes between design tiers glide, with identity padding and pairing by proximity. Shape and slope changes crossfade over 100 ms (the R17 task below)
- [x] Implement the redesign scheduler: read every 4 samples, redesign at most every 32, jump detection (DSP lead, Phase 1). Reads are on an absolute-time grid, so output is identical for any host block sizes; events force a read, so band changes are sample-accurate; designs look ahead and coefficients glide. Zipper −94 dBFS
- [x] Implement the 100 ms section-count crossfade and near-zero-gain bypass fade (DSP lead, Phase 2). Done on 2026-11-16 (`core/engine`). A shape or slope change on a sounding band keeps the old cascade running on its own frozen coefficients and state and crossfades linearly to the new one, which starts from rest, over exactly 100 ms (`kStructureFadeMs`); a further shape or slope change waits until the crossfade ends. A gain shape whose effective gain (after Gain Scale) is below 0.001 dB (`kNearZeroGainDb`, PROVISIONAL) fades to bypass through its enable crossfade and is then bit-transparent. Tested (`tests/engine/test_structure.cpp`): a 12 to 48 dB/oct slope change on a sine never steps beyond the sine's own movement and lasts 4800 samples at 48 kHz (within the 5 % window), then settles to the 48 dB/oct filter; a change during a crossfade starts when it ends; 0 dB and ±0.0005 dB Bell, shelf and tilt bands, and Gain Scale 0 %, are bit-identical to no band; moving a gain to 0 dB and back is smooth and exactly dry in between. When Dynamic EQ lands, a band with dynamics on must not count as near zero
- [x] Write the shared conventions header for dB, time constants and log2 frequency (DSP lead, Phase 1). `core/dsp/include/amq/dsp/conventions.hpp` (from Phase 0, with tests): dB law, 90 % time definition and one-pole coefficients, log2-Hz encoding. The dynamics smoother builds on it
- [x] Build the FFT and partitioned convolution interface with a permissive backend (DSP lead, Phase 1). `amq/dsp/fft.hpp` (`RealFft`, ordered and packed domains, `FftBuffer`) on PFFFT ([D2](../decisions.md)), and `amq/dsp/partitioned_convolver.hpp`. Tested against a direct DFT (2e-6 relative) and direct convolution (1e-5 relative to peak) for kernels of 1 to 1000 taps and random host block sizes
- [x] Add the speaker-group field and channel descriptor to the band model, inactive in v1 (DSP lead, Phase 1). Done on 2026-11-16: `core/dsp/include/amq/dsp/speakers.hpp` (descriptor, groups, v1 layouts, BS.1770 loudness weights for Auto Gain) and a per-band group in the engine (All; the `bandNN.speakers` id stays reserved). The engine masks each band's channels by its group; an M/S band needs both channels of the pair
- [ ] Implement the CLAP params, state, latency, audio-ports and render extensions (Plugin and host engineer, Phase 1: with a CLAP-native core these are the plugin's host interface, D1). Progress: params, state, latency, stereo audio ports and render (offline uses the refined designs at once) are done, plus param gestures from the editor and the side-chain port. Still to do: a mono main layout (audio-ports-config), when the host matrix needs it
- [ ] Add pluginval, the VST3 validator and clap-validator to CI on both platforms (Build and QA engineer, Phase 1). Progress: `.github/workflows/ci.yml` runs clap-validator 0.4.1, pluginval 1.0.4 at strictness 10 and the Steinberg VST3 validator on Windows and macOS. Release binaries are pinned by SHA-256 (`tools/ci/fetch_validators.sh`), and the VST3 validator is built from the pinned SDK (`tools/vst3_tools/`). First green Windows run on 2026-11-16: clap-validator 34 passed, 0 failed, 0 warnings, 10 skipped; pluginval SUCCESS; VST3 validator 47 of 47. Still to do: macOS, where the job is allowed to fail and stops at configure (Objective-C++ is not enabled); that is part of the first macOS build task
- [ ] Write a clean-room review checklist and review this plan and the code against it (Counsel, Phase 0)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Filter magnitude accuracy | Sweep every shape, slope, frequency, gain and Q at 44.1–384 kHz, comparing against the analog prototype (levels below −60 dB only need to stay below −60 dB) | Zero Latency, below 0.9·Nyquist: ≤ 0.1 dB for f0 ≤ 0.9·Nyquist; ≤ 1.1 dB for f0 > 0.9·Nyquist with Q ≤ 3; TBD (R14) for f0 > 0.9·Nyquist with Q > 3 |
| All Pass phase | Measure phase at the centre frequency | Within 2° of analog |
| Sample-accurate automation | Automate gain as a step at a known offset inside a block | Change starts at that exact sample |
| Zipper residual | Fast frequency and gain sweeps against an offline ideal | Residual < −90 dBFS |
| Section-count crossfade | Change slope while playing a sine | No discontinuity; crossfade lasts 100 ms ±5 % |
| Bypassed band transparency | Disabled band versus no band | Bit-identical |
| Global bypass null | Bypass on: output versus input delayed by the reported latency | ≤ −140 dBFS |
| Off-thread callbacks | Call latency, params, state, audio-ports and GUI-size callbacks from a non-main thread in our CLAP and VST3 test hosts | No abort; values correct; state reaches the audio thread; host notifications happen on the main thread |
| 64-bit I/O | Host requests double precision | Output equals the float path within float precision |
| Block-size robustness | Block sizes 1, 2, 3, 64, 4096 and random | Identical output within float precision |
| Real-time safety | Allocation and lock detector during process | Zero events |
| Fuzz | Random parameters, rates and blocks for 1 hour | No NaN or Inf; no crash |
| CPU | 24 bands, 48 kHz, 64-sample blocks | ≤3 % of one core |
| Validators | pluginval (strictness 10), VST3 validator, clap-validator | Clean |
| Side-chain bus | Feed signal only to the side chain | Main output unaffected while dynamics are off |

## Open questions

- ~~Which permissive FFT backend do we use, or do we write our own?~~ Decided: PFFFT, cleared by Counsel ([D2](../decisions.md)).
- Do we accept double-precision processing natively in a later version, or only by conversion?
- What tolerance target is right for brickwall passband ripple and stopband depth?
