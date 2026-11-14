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

- [ ] Choose window, FFT library and size table; document scaling for amplitude and power (DSP lead, Phase 0)
- [ ] Implement the engine core: taps, framer, FFT plans and frame fan-out to consumers (DSP lead, Phase 1)
- [ ] Implement the display follower with millisecond rise and fall, freeze, peak-hold and tilt (DSP lead, Phase 2)
- [ ] Hook the EQ Match accumulators into the engine at highest resolution (DSP lead, Phase 3)
- [ ] Run the engine on the worker with lifecycle tied to the plugin, not the editor (Plugin and host engineer, Phase 1)
- [ ] Render pre, post and side-chain spectra with pixel interpolation and visual smoothing (GUI engineer, Phase 2)
- [ ] Add CPU budget and real-time safety checks for the engine in CI (Build and QA engineer, Phase 2)
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
- Should the match path use more than one FFT size (for example longer frames below 200 Hz) for better low-frequency resolution?
- Is the CPU budget per instance acceptable for sessions with 50+ instances, or should idle instances throttle the engine?
