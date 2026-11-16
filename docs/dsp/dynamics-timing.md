# Dynamic EQ timing spec

Status: PROVISIONAL constants, 2026-11-16. Code: `core/dsp/include/amq/dsp/dynamics.hpp`. Plan: area 06 (timing), area 07 R7–R11 (auto threshold, knee), area 08 R1–R2 (reference envelope). The constants are frozen by the area 06 listening tests (Product designer and DSP lead, Phase 3).

## Time definition

Every attack, release and smoothing time is a **90 % time**: the time the gain (in dB) takes to cover 90 % of a step (area 01 R20). The one-pole coefficient for a time T at sample rate fs is `c = -expm1(ln(0.1) / (T fs))`, so measured times do not depend on the sample rate (area 06 R11).

## Knob tapers

Attack and Release are 0–100 (default 50). Each maps to time on a piecewise log curve through three anchors:

| Knob | Attack (band at 1 kHz) | Release |
| --- | --- | --- |
| 0 | 0.25 ms | 5 ms |
| 50 | 10 ms | 250 ms |
| 100 | 250 ms | 1000 ms |

Between anchors the time is log-interpolated, so equal knob steps are equal time ratios.

## Frequency-aware attack

- The attack anchors are for a band at 1 kHz. A band at f scales them by `(1 kHz / f)^0.25`: ×1.78 at 100 Hz, ×0.56 at 10 kHz. At the same knob the 100 Hz attack is `100^0.25 = 3.16` times the 10 kHz attack (area 06 R4).
- **Minimum-cycles rule:** the attack is never shorter than one period of the band centre. This stops the gain moving at waveform rate on bass. At 100 Hz the rule binds below knob 42; there the attack is 10 ms.
- Release is not frequency-scaled.

## Program-dependent release (dual release)

Two smoothers run in parallel on the target gain reduction, and the larger reduction wins:

- **Fast path:** the attack time, then a 40 ms release (or the Release time, if that is shorter).
- **Slow path:** an attack of at least 150 ms, then the Release time.

A short peak barely fills the slow path, so it recovers at the fast release, in tens of milliseconds. Sustained reduction fills the slow path, so it recovers at the Release time (area 06 R5). Measured: a 5 ms burst recovers in 40 ms; a 2 s tone recovers at the Release time ±5 %.

A final attack/release stage follows. It is the identity (both times 0) until listening asks for more.

## Gain computer and knee

- The gain reduction grows one dB per dB the side-chain level is over the threshold. The band holds the side-chain level at the threshold until the range is used up. The reduction is then capped at |range| (area 06 R7).
- The sign of the range makes it a cut (negative) or a boost (positive).
- The knee is quadratic, centred on the threshold. Its width is `3 dB + 1.5 dB × band width in octaves + 0.2 × |range|` (area 07 R11).
  - Band width is the octave width of the band's Q for Bell, Notch and Band Pass, and 2 octaves for shelves and tilts.

## Detector

- The side chain is the plugin input, or the external side-chain bus with SC External on. It is encoded to the band's placement:
  - **Stereo:** linked, using the louder of L and R (area 06 R10).
  - **Left, Right:** that channel.
  - **Mid, Side:** the encoded M or S (area 03).
- It is then filtered:
  - **Band mode:** the band's region. A band-pass with the band's Q for Bell, Notch, Band Pass and All Pass; a low-pass or high-pass at the band frequency for shelves and cuts; no filter for tilts.
  - **Free mode:** a high-pass at SC Low and a low-pass at SC High.
  - Both use the helper filters (area 02 R15).
- **Level:** a peak follower with instant attack and a release of 4 band periods (at least 1 ms). For a band-pass side chain it follows the SVF quadrature envelope, which has no ripple on a tone; otherwise it follows |x|.
  - The level is in dB, floored at −150 dBFS.
  - Peak versus short-RMS detection is an area 06 open question.

## Auto threshold

- **Reference envelope** of the side-chain level:
  - Instant attack, and a 10 s release.
  - For the first 0.5 s after a reset, activation or switching Auto on, it releases in 0.25 s instead, so it settles within 0.5 s (area 07 R9).
  - It never falls below −100 dBFS, so silence and very low input give no gain change (R10).
  - It is independent of the Attack and Release knobs (area 08 R2).
- **Threshold:** the reference minus |range|, with the offset capped at 30 dB even when Gain Scale enlarges the range (area 06 R8, area 07 R7).
- **Timing:** Dyn Auto uses the knob-50 timing. The stored knob values are kept and come back when Auto is turned off (area 06 R6).

## Application

- The band's output blends per sample between its static filter and the same band designed at static gain + range (after Gain Scale, clamped to ±30 dB). The blend weight is the gain reduction divided by |range| (area 01 R27).
- The gain + range filter glides between designs like the static one.
- A band whose dynamics run is never bypassed as near-zero (area 01 R16).

## Measured (tests)

The harness in `tests/test_dynamics.cpp` applies dB steps to the dynamics core and reports 90 % times. Every case is within ±5 % of this spec at 44.1, 48, 96 and 192 kHz:

- attack and release at several knob values;
- bands at 100 Hz, 1 kHz and 5 kHz;
- the 100 Hz : 10 kHz attack ratio.

`tests/engine/test_dynamic_eq.cpp` checks the engine:

- range cap and sign;
- threshold;
- stereo link;
- placement and M/S detection;
- the external side chain;
- auto threshold, its offset cap and silence;
- Auto keeping the knobs;
- the reference envelope's independence;
- Side Chain Listen;
- smoothness while every dynamics control moves.
