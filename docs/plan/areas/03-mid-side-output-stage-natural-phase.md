# 03 Mid/Side, output stage, Natural Phase

Am-Q uses the textbook Mid/Side matrix, inserted only when an M or S band is active. Its output stage applies Gain Scale, Auto Gain, Level, Pan and Invert. Natural Phase is the real IIR followed by a short correction FIR that pulls the cascade onto the analog response.

## Requirements

1. **R1 M/S matrix.** Encode is M = (L+R)/2 and S = (L−R)/2. Decode is L = M+S and R = M−S. There is no √2 scaling, so a Mid-only signal keeps its level. The S sign is the textbook one.
2. **R2 Exact round trip.** Encode followed by decode, with no M/S band active, nulls against the input to ≤ −140 dBFS.
3. **R3 Insert on demand.** Encode and decode are inserted only while an M or S band is active. Inserting or removing them causes no click.
4. **R4 Stage order.** The order per pair is: L/R IIR, L/R FFT, encode, M/S IIR, M/S FFT, decode. FFT stages are delay-aligned with IIR stages.
5. **R5 Side chain in the band's domain.** The side-chain detector for an M or S band reads the M/S-encoded side-chain signal. It does not use the main audio path.
6. **R6 Gain Scale.** Gain Scale runs 0–200 %, default 100 %. It multiplies static gain, and gain plus range for dynamic bands. The result is clamped to ±30 dB. It also applies to the drawn curve and to Auto Gain.
7. **R7 Auto Gain off.** When off, Auto Gain is exactly unity.
8. **R8 Auto Gain on.**
   - It derives a make-up gain from the combined magnitude response of the active bands, not from audio.
   - The gain is smoothed and clamped to ±30 dB.
   - Detail of the weighting: Tab 05.
9. **R9 Auto Gain with mixed placements.** Make-up gain combines across channels and stages with per-speaker weights. LFE is excluded, and side surrounds count more than front speakers (surround weights are designed in, inactive in v1).
10. **R10 Output Level.** Output Level is smoothed and applied as a linear gain. The knob mapping is in Tab 11.
11. **R11 Pan law.** Pan is a linear balance law that only attenuates: pan right lowers left, pan left lowers right, and the centre is unity. Pan exists only in stereo layouts.
12. **R12 Invert.** Invert negates both outputs.
13. **R13 Solo Gain.** Solo Gain applies only while a band is soloed. Otherwise it has no effect (Tab 02).
14. **R14 Natural Phase structure.** Audio passes the band IIR filters first, then a short correction FIR. The correction per frequency is the product of target responses divided by the product of the digital IIR responses actually used.
15. **R15 Robust correction.** The kernel must stay well-behaved where the IIR response is near zero or the phase wraps. DC must be real, and phase must fade to zero at Nyquist. Am-Q uses its own regularisation.
16. **R16 Brickwall in Natural Phase.** Brickwall bands are excluded from the correction and stay as their IIR. This is documented in the manual.
17. **R17 Linear Phase stage.** FFT stages use the zero-phase combined magnitude, sqrt of the product of |H|², so the result is linear phase.
18. **R18 Shared design.** The digital IIR response used in the correction comes from the same design call as the audio filter.

## Am-Q design

**M/S converters.** One small converter type has three modes: pass, encode and decode. It is SIMD-friendly and owned once per channel pair. The router inserts the pair when any band in the M/S stage is enabled. A crossfade is not needed, because the round trip is transparent. A second encoder feeds only the M/S side-chain detectors.

**Output stage.** The order is: Gain Scale (applied inside band design), bands, Auto Gain node, Level/Pan/Invert node, then the Pan Mode converters (Tab 04). Level, Pan and Auto Gain are per-sample ramps.

**Auto Gain.** Auto Gain is recomputed off the audio thread when any band changes. It is published through a lock-free slot and ramped on the audio thread.

**Natural Phase.** Each band caches two responses on a shared frequency grid: the analog target (Tab 04) and the digital IIR response. A worker thread builds the correction for all bands as one 2×2 frequency response per bin. It then converts that to FIR kernels (Tab 05) and swaps them at a block boundary with a short crossfade.

**Regularisation.** Bins with very small IIR magnitude use a capped ratio instead of copying a neighbour value. Unwrapped phase is checked for jumps. The phase taper near Nyquist is our own shape, tuned by measurement.

**Design commitments.**

- One combined Natural Phase correction, not one per stage, so latency is fixed (Tab 02).
- Our own regularisation and taper.
- Textbook S sign.

## Implementation tasks

- [ ] Implement the M/S converter (pass, encode, decode) with SIMD (DSP lead, Phase 1). Progress: the converter is done (`core/dsp/include/amq/dsp/mid_side.hpp`): textbook matrix, round trip ≤ −140 dBFS. Still to do: SIMD
- [ ] Insert M/S on demand in the router, with the M/S side-chain encoder (DSP lead, Phase 2). Progress: done early for the IIR path. Encode and decode run only while a Mid or Side band is audible; toggling a 0 dB Mid band deviates by −296 dBFS. Still to do: the M/S side-chain encoder (the side-chain bus now exists; the encoder comes with the detectors, area 06), and the FFT-stage matrixing (area 02)
- [ ] Implement Gain Scale in band design and in the GUI curve (DSP lead and GUI engineer, Phase 2)
- [ ] Implement the Auto Gain worker, lock-free publish and ramp (DSP lead, Phase 2)
- [x] Implement the Level/Pan/Invert node with balance law and ramps (DSP lead, Phase 1). The M/S balance and the Pan Mode crossfade were added with area 04 (2026-11-16). SIMD is not used yet
- [ ] Implement per-band analog and digital response caches on the shared grid (DSP lead, Phase 2). Progress: done for the Natural Phase kernel bins (area 04 R12 task, `engine::CorrectionCache`). Still to do: the Auto Gain log grid (area 04 R14), when Auto Gain is built
- [ ] Prototype the combined 2×2 Natural Phase correction and compare it with per-stage correction (DSP lead, Phase 0). Progress (2026-11-16): the combined correction runs in the engine (`core/engine/include/amq/engine/natural_phase.hpp`): per bin C = T · H⁻¹ with H = Dec · diag(M/S digital) · Enc · diag(L/R digital) and T the same with the analog targets, four 512-tap kernels (LL, LR, RL, RR) in four partitioned convolvers; without Mid or Side bands it reduces to the per-channel ratio and the cross kernels are zero. Measured through the engine at 48 kHz: L/R bands within 0.0004 dB and 0.003 degrees of the analog target delayed by 320; Left, Mid, Side and Stereo bands together within 2.7e-5 (complex, relative) of the 2×2 target. Still to do: the comparison with per-stage correction
- [x] Implement correction regularisation and the Nyquist phase taper (DSP lead, Phase 2). Done on 2026-11-16 (`core/engine/src/natural_phase.cpp`, chosen by measurement: `amq_engine_tests -tc="Natural Phase regularisation evaluation*" --no-skip`). Diagnosis first: matched designs put the digital zero exactly on the analog one, so the ratio is smooth through a zero; the failures came from bins that land on a zero, where both responses are rounding noise (about −288 dB) and the 0/0 is garbage (a 6 kHz notch on bin 64 at 48 kHz: 4.97 dB off, 10.2 dB with no window). The design has three parts. (1) Removable-singularity limit: where a band's digital response is below −120 dB on a bin, the correction is the mean of the correction just either side (±δ, δ from 1e-4 widening until both sides are resolvable): the limit of the ratio, not a neighbour bin's value. (2) Capped ratio: each digital response is floored relative to its own target and every entry capped at +24 dB, for zeros that do not coincide. (3) Nyquist phase taper: the digital response is real at Nyquist and the target is not, so above 0.97 Nyquist the unwrapped phase is scaled to zero with a raised cosine (magnitude kept); 0.97 costs nothing below 0.95 Nyquist (0.95 costs 0.46 degrees, 0.90 costs 60) and keeps the top region within 0.31 dB with Hann (1.2 dB without a taper). Result with no window (decision D3): 28 bands, notches included, within 0.009 dB and 0.07 degrees to 0.95 Nyquist at 44.1, 48 and 96 kHz
- [ ] Document Brickwall behaviour in Natural Phase for the manual (Product designer, Phase 3)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| M/S round trip | Random stereo noise through encode/decode, no bands | ≤ −140 dBFS null |
| Mid-only band | +6 dB Mid Bell on L-only and L=R signals | Only (L+R) content changes, within 0.05 dB |
| Side-only band | +6 dB Side Bell on a mono signal (L=R) | Output equals input within −140 dBFS |
| M/S insert transparency | Toggle a 0 dB M band while playing | No discontinuity > −120 dBFS |
| Gain Scale | Scale 50 % and 200 % on a +12 dB Bell | Peak 6 dB and clamped 30 dB respectively, ±0.05 dB |
| Pan law | Pan full right on a stereo signal | Left silent; right unity; centre unity |
| Invert | Invert on | Output = −input within float precision |
| Auto Gain off | Auto Gain off with bands active | Make-up gain exactly 1 |
| Natural Phase accuracy | Bell, shelf and cut responses up to 0.95·Nyquist | Magnitude ≤ 0.05 dB and phase ≤ 2° vs analog prototype |
| Natural Phase brickwall | Brickwall cut in Natural Phase | Equals Zero Latency response, delayed by the reported latency |
| Linear Phase stage | Symmetric impulse response | Kernel symmetric; phase linear within 2° |

## Open questions

- Is a 0–200 % Gain Scale range right, or do we need negative values to invert the curve? Product designer to decide.
- Does Auto Gain apply to the solo audition signal?
