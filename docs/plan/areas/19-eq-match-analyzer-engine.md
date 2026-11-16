# 19 EQ Match analyzer engine

Am-Q has one analyzer engine, on a worker thread, that serves both the live display and EQ Match. It has two consumers on shared FFT frames: a ballistic display follower and a power accumulator for matching. EQ Match always uses the highest resolution, independent of display settings.

## Requirements

1. R1. One analyzer engine produces pre, post and side-chain spectra plus the EQ Match captures. It runs on a worker thread fed by lock-free audio copies, never in the audio callback.
2. R2. FFT size follows the analyzer resolution setting (2048 to 16384), scaled with sample rate to keep bin width roughly constant.
3. R3. EQ Match captures always use the highest resolution the engine offers, whatever the display resolution.
4. R4. Window is Kaiser or Blackman-Harris with 75 % overlap. Scaling is defined for both amplitude display and power averaging.
5. R5. Display ballistics use rise and fall times in milliseconds that do not depend on FFT size or sample rate.
6. R6. The display path supports tilt (default 4.5 dB/oct), freeze, peak-hold and the collisions layer. None of these alter EQ Match data.
7. R7. The match path accumulates power with a −90 dBFS frame gate and has no warm-up ramp. Readiness is handled by the minimum-capture rule instead (tab 17 R8).
8. R8. Display post-processing (interpolation to pixels, smoothing) happens on the GUI side or in a display stage, never in the match path.
9. R9. Analyzer settings (resolution, window, ballistics, tilt) are owned by the engine and readable by all consumers.
10. R10. Engine CPU use stays under a set budget at the highest resolution with all consumers active. Planning value: 3 % of one core at 48 kHz stereo.

## Am-Q design

**Structure.** A single engine object owns the taps (tab 17), the framer and one FFT plan per active size. Consumers subscribe to frames: the display follower, the match accumulators and the collisions detector. Frames are shared; only consumer state is separate.

**FFT and window.** Am-Q uses a public FFT library chosen by the DSP lead, with licence review by Counsel. The window is periodic Blackman-Harris or Kaiser, decided in Phase 0 by sidelobe and resolution tests. Two scale factors are kept: coherent gain, so a sine reads its true level on the display, and energy, for power averaging.

**Display follower.** Per log point: rise and fall smoothing of the dB value with time constants in milliseconds, computed from the actual hop period. Ballistics feel the same at every resolution.

**Match accumulator.** Described in tab 18. Am-Q excludes partial frames (tab 17 R4) and enforces a minimum capture time, so no ramp is needed.

**Display post-processing.** Pixel interpolation (cubic or monotone spline), optional max-pooling for readability and visual smoothing belong to the display stage (tab 12). They never feed EQ Match.

## Implementation tasks

- [ ] Choose window, FFT library and size table; document scaling for amplitude and power (DSP lead, Phase 0). Progress: FFT library is PFFFT ([D2](../decisions.md)). Size table: 2048, 4096, 8192, 16384 at 44.1/48 kHz, times 2, 4 or 8 above 50, 100 and 200 kHz (default 4096 at 48 kHz, PROVISIONAL). Scaling is documented in `amq/analysis/analyzer.hpp`: amplitude (2 / sum w)^2, power as the one-sided Welch PSD 2 / (fs sum w^2). Window data: periodic 4-term Blackman-Harris and Kaiser (beta 12, PROVISIONAL) both pass the sidelobe test and both scallop 0.83 dB half a bin off-centre. Still to do: pick the default window
- [x] Implement the engine core: taps, framer, FFT plans and frame fan-out to consumers (DSP lead, Phase 1). `amq/analysis/analyzer.hpp`: three taps (pre-EQ, post-EQ, side chain), full frames only with hop N/4, one framer per FFT size in use (display or highest; shared when equal), per-channel |X|^2 and the mean-power merge, frames handed to subscribed consumers. First consumer: `DisplaySpectrum` maps the latest frame onto the shared 512-point log grid (largest bin per point, interpolation below one bin) and publishes the pre, post and external spectra through the spectrum snapshot. Tested: a -20 dBFS sine at a bin centre reads -20 dBFS at every resolution; the white-noise PSD is within 0.07 dB RMS (20 Hz to 20 kHz, 60 s); a tone 60 dB down 10 bins away reads its level; hard-left, centre and hard-right noise agree within 1e-6 dB; the worker pass does not allocate. Worker CPU with three stereo sources at 16384 points plus a display consumer: about 0.6 % of one core at 48 kHz (R10 budget 3 %)
- [ ] Implement the display follower with millisecond rise and fall, freeze, peak-hold and tilt (DSP lead, Phase 2)
- [ ] Hook the EQ Match accumulators into the engine at highest resolution (DSP lead, Phase 3)
- [x] Run the engine on the worker with lifecycle tied to the plugin, not the editor (Plugin and host engineer, Phase 1). The engine owns the analyzer: its worker starts in `Engine::prepare` (plugin activate) and stops in `release` (deactivate). The worker flushes denormals to zero, so it can't slow the audio thread (clap-validator's denormal test is clean 6 runs out of 6; without flush-to-zero it warned on every run). Tested: spectra arrive with no editor attached
- [ ] Render pre, post and side-chain spectra with pixel interpolation and visual smoothing (GUI engineer, Phase 2). Progress (2026-11-16): the editor draws the pre, post and side-chain display spectra on the shared log grid, following the zoomed span, the analyzer range (60/90/120 dB) and tilt, for the sources switched on. Still to do: visual smoothing, with the display follower (rise, fall, peak hold) task
- [x] Add CPU budget and real-time safety checks for the engine in CI (Build and QA engineer, Phase 2). Done on 2026-11-16. `tests/engine/test_rt_safety.cpp` runs in ctest (so in CI): an allocation detector (the test binary replaces the global allocation functions and counts on the marked audio thread, with a self-check) over 3000 random-size blocks with random band, shape, slope, solo, pan-mode, bypass and Solo Gain edits, in Zero Latency and Natural Phase, expects zero allocations; and a fuzz run (3 s in CI, `AMQ_FUZZ_SECONDS` for the 1-hour acceptance run) over random parameters, all eight rates, mono and stereo, random block sizes and silence, square and denormal-range inputs, expects finite output. The CPU budget is `tests/engine/test_cpu.cpp`: a report and a 2x-margin check, both skipped by default because shared CI machines time unreliably; run them on a quiet machine. The analyzer (`amq_analysis_tests`) has its own allocation checks
- [ ] Review the FFT library licence (Counsel, Phase 0)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Sine calibration | Feed a 1 kHz sine at −20 dBFS at every resolution. | Display peak reads −20 dBFS within 0.2 dB |
| Noise calibration | Feed white noise; compare the match power spectrum to the analytical PSD. | Within 0.2 dB RMS over 20 Hz to 20 kHz |
| Resolution independence of match | Capture the same audio with the display at each resolution. | Bit-identical EQ Match captures |
| Ballistics independence | Measure rise time to a step at each resolution and sample rate. | Within 10 % of the set time |
| Tilt and freeze isolation | Toggle tilt, freeze and peak-hold during a capture. | Match data unchanged |
| Sidelobes | Two tones 60 dB apart, 10 bins apart. | Weaker tone visible and within 1 dB of its level |
| CPU budget | Profile at highest resolution with all consumers, 48 kHz stereo. | Below the agreed budget |
| Editor lifecycle | Close and reopen the editor during a capture. | Capture continues; display resumes without a glitch |

## Open questions

- Blackman-Harris or Kaiser as the single default? Should the window be user-selectable?
- Sine calibration at any frequency: half a bin off-centre both windows read 0.83 dB low, so the 0.2 dB target holds only at bin centres. The display follower (Phase 2) needs peak interpolation, or a flat-top display window.
- Should the match path use more than one FFT size (for example longer frames below 200 Hz) for better low-frequency resolution?
- Is the CPU budget per instance acceptable for sessions with 50+ instances, or should idle instances throttle the engine?
