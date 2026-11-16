# 06 Dynamic EQ timing

Am-Q's dynamic EQ uses a log-domain side-chain detector, a soft-knee gain computer and gain-domain smoothing with a program-dependent release. Attack and Release (0–100) map to our own frequency-aware time curves, which we tune by listening and verify to ±5 %.

## Requirements

1. R1. Attack and Release are 0–100 parameters with a default of 50. Each maps monotonically to a time in seconds.
2. R2. Attack covers at least 0.25 ms to 250 ms across the knob. Reference behaviour spans about this range and is longer at low band frequencies.
3. R3. Release covers at least 5 ms to 1 s across the knob. Reference behaviour tops out a little under 1 s.
4. R4. At the same knob value, attack is longer for low-frequency bands than for high-frequency bands. The scaling is fixed by design and documented.
5. R5. Release is program-dependent. Short peaks recover in tens of milliseconds. Sustained gain reduction recovers at the Release time or slower.
6. R6. With Dyn Auto on, the threshold is automatic (Tab 07) and the default timing applies. The stored Attack/Release values are kept and come back when Auto is turned off.
7. R7. Gain change never exceeds |Dyn Range|. Dyn Range is limited to ±30 dB, and its sign selects cut or boost.
8. R8. The automatic threshold offset is capped at 30 dB, even when a global scale such as Gain Scale enlarges the effective range.
9. R9. A stated time is the time the gain takes to cover 90 % of a dB step. Measured times are within ±5 % of the documented value.
10. R10. A band on Stereo placement uses linked detection, so both channels get the same gain.
11. R11. Times do not depend on sample rate: they measure the same at 44.1, 48, 96 and 192 kHz.
12. R12. Moving Attack, Release, Threshold or Range during playback causes no clicks: zipper residual stays below −90 dBFS.

## Am-Q design

- **Signal chain.** We follow the structure in Giannoulis, Massberg and Reiss:
  - Side-chain level in dB, from the band-filtered internal or external side chain (Tab 07 handles the Free mode with SC Low/High).
  - A soft-knee gain computer gives gain reduction of zero or more, capped at |range|.
  - The result is smoothed in the dB domain, then signed by the sign of the range.
  - The result drives the per-sample blend between the static filter and the gain+range filter (Tab 01).
- **Program-dependent release.** Two release paths run in parallel: a short fixed release and a slow release set by the Release knob. We take the larger gain reduction of the two. A final attack/release stage then shapes the output. This is a known dual-release technique; all times are ours.
- **Frequency-aware attack.** Attack time is the larger of two values: the knob-mapped time, and a minimum of a few periods of the band centre frequency. This limits waveform-rate gain modulation, which is audible as distortion on bass. The DSP lead tunes the cycle count by listening.
- **Knob tapers.** Both knobs map to log-time between their minimum and maximum, so the scale is perceptually even. The product designer sets the midpoint so that 50 gives musical defaults. Two candidates at Phase 0: about 10–20 ms attack on low bands and about 200–300 ms release.
- **Coefficients.** We use textbook one-pole smoothers. Each coefficient comes from the 90 % settling time and the sample rate. Coefficients are recomputed only when an input changes, at most every 32 samples.
- **Stereo link.** For Stereo placement the detector takes the louder channel each sample. L/R/M/S bands detect on their own channel only.
- **Auto mode.** Dyn Auto selects the auto threshold and the default timing. The GUI greys the Attack/Release knobs but keeps their values.
- **Shared timing.** Spectral dynamics (Tab 07) reads the same Attack/Release parameters and derives its per-band times from them.
- **Release structure.** Use only the dual-release structure until listening tests show a need for more. Knob tooltips show the real time in ms at the band's current frequency.

## Implementation tasks

- [x] Write the timing spec: knob tapers, minimum-cycles attack rule, dual-release times and the 90 % time definition (DSP lead, Phase 0). Done on 2026-11-16: [docs/dsp/dynamics-timing.md](../../dsp/dynamics-timing.md), in code in `core/dsp/include/amq/dsp/dynamics.hpp`. All constants PROVISIONAL until the listening tests below
- [ ] Prototype the detector and gain-computer chain offline, and compare candidate tapers by ear on drums, bass and vocals (DSP lead, Phase 0)
- [x] Implement the dB-domain one-pole attack/release smoother with an instant mode for time 0, in core/dsp (DSP lead, Phase 1). `core/dsp/include/amq/dsp/db_smoother.hpp`: the decoupled form from Giannoulis, Massberg and Reiss, with coefficients from the shared 90 % time convention. Tested: 0.25 ms to 250 ms attack and 5 ms to 1 s release within ±5 % at 44.1 to 192 kHz; time 0 is instant; coefficients recomputed only on change; knob moves do not step the value; fuzz stays finite and a settled value snaps to its target (no denormal tail). The dual release, gain computer and knob tapers are the Phase 2 tasks
- [ ] Implement the dual-release stage and final attack/release stage per band, with SIMD across bands (DSP lead, Phase 2). Progress (2026-11-16): the dual release (fast path with a 40 ms release, slow path with a 150 ms minimum attack, the larger reduction wins) and the final stage (identity until tuned) run per band (`dsp::BandDynamics`); a 5 ms burst recovers in 40 ms and a 2 s tone at the Release time. CPU (`tests/engine/test_cpu.cpp`): one dynamic band costs about 0.3 % of a core at 48 kHz; all 24 bands dynamic cost 7.6 %. Still to do: SIMD across bands, to bring all-dynamic sessions under 3 %
- [x] Implement the soft-knee gain computer with range cap and range sign (DSP lead, Phase 2). Done on 2026-11-16 (`dsp::gainComputerDb`, `kneeWidthDb`): one dB of gain change per dB over the threshold, a quadratic knee whose width grows with band width and |range| (area 07 R11), capped at |range|; the range sign makes it a cut or a boost. In the engine, 34 dB over a threshold gives exactly the range (cut -12 dB, boost +6 dB, within 0.02 dB); the band's output blends per sample from its static filter to the band at gain + range (area 01 R27)
- [x] Implement stereo-linked detection for Stereo placement (DSP lead, Phase 2). Done on 2026-11-16: a Stereo band detects the louder channel; a loud left and a quiet right channel get the same gain within 1e-3 dB. Left and Right bands detect their own channel; Mid and Side bands detect the encoded M or S
- [ ] Wire Dyn Auto to timing defaults while keeping the stored knob values, and grey the knobs in the GUI (Plugin and host engineer, GUI engineer, Phase 2). Progress (2026-11-16): the engine uses the knob-50 timing and the auto threshold while Dyn Auto is on, and the stored Attack and Release values are untouched (tested: Attack 80 reads back and its timing returns with Auto off). Still to do: greying the knobs, with the band panel's dynamics section (area 12)
- [x] Add a knob tooltip that shows the effective time in ms (GUI engineer, Phase 3). Done on 2026-11-16: hovering Attack or Release shows the effective time at the band frequency in the panel's status line (the knob-50 timing in Auto)
- [x] Build a measurement harness that applies dB steps to the side chain and reports 90 % settling times (Build and QA engineer, Phase 2). Done on 2026-11-16 (`tests/test_dynamics.cpp`): it steps the dynamics core's input by 20 dB and interpolates the 90 % crossing. Attack and release match the spec within ±5 % at 44.1, 48, 96 and 192 kHz for bands at 100 Hz, 1 kHz and 5 kHz and knobs 0, 25, 50 and 100; the 100 Hz : 10 kHz attack ratio is 3.16 within ±5 %
- [ ] Run listening tests to tune tapers and dual-release times, and freeze the constants (Product designer, DSP lead, Phase 3)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Attack range | Step the side chain by +20 dB at knob 0 and 100, on bands at 100 Hz and 5 kHz | Times match spec within ±5 %; knob 0 ≤ 0.3 ms at 5 kHz; knob 100 ≥ 250 ms at 100 Hz |
| Release range | Step the side chain by −20 dB after sustained reduction, at knob 0 and 100 | Within ±5 % of spec; maximum ≥ 1 s |
| Frequency scaling | Same knob value on bands at 100 Hz and 10 kHz | Low band attack is longer by the documented ratio, ±5 % |
| Program-dependent release | 5 ms burst versus 2 s sustained tone, same settings | Burst recovers within the fast-release spec; sustained tone recovers at the Release spec |
| Range cap | Range ±30 dB, input 60 dB over threshold | Gain change is never larger than 30 dB |
| Auto offset cap | Gain Scale at maximum with range 30 dB in Auto | Auto offset is never more than 30 dB |
| Sample-rate independence | Repeat the attack and release tests at 44.1, 96 and 192 kHz | All within ±5 % of the 48 kHz values |
| Stereo link | Signal on L only, band on Stereo | L and R gain curves are identical |
| Auto restores knobs | Set Attack to 80, toggle Auto on then off | Attack reads 80 and timing matches knob 80 |
| Zipper | Sweep Attack, Release, Threshold and Range during a sine | Residual below −90 dBFS |
| Fuzz | Random parameters and signals, including silence and full-scale noise | No NaN or Inf |

## Open questions

- Should Auto mode allow manual Attack/Release with an automatic threshold? The product designer decides at Phase 0.
- Peak or short-RMS level detection: we pick by listening in the Phase 0 prototype.
- What exact attack ratio from low to high frequency? To be tuned by listening; low frequencies are expected to need noticeably longer attacks.
- Should knobs show ms instead of 0–100? The current proposal is 0–100 with an ms tooltip.
