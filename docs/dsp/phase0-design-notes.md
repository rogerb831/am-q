# Phase 0 DSP design notes

Status: Phase 0 sandbox, updated 2026-11-14. Spec: `docs/plan/` area 01 (R9, R10, R12, R13,
R14, R20) and area 04 (R9 analog target).

Full harness output:
- [phase0-accuracy-report.md](phase0-accuracy-report.md): the current default (conditional correction section) against the revised R12, with comparisons.

## What exists

| Piece | File | Notes |
| --- | --- | --- |
| Conventions | `core/dsp/include/amq/dsp/conventions.hpp` | 10^(dB/20); ±30 dB clamp; one-pole coefficient from a 90 % step time, `c = -expm1(ln(0.1)/(t*fs))`; log2-Hz encoding and its 0..1 host mapping (10 Hz–30 kHz). |
| Analog prototypes | `analog_prototype.{hpp,cpp}` | Ten shapes, built as zero/pole/gain and grouped into 2nd-order s-sections (1st-order where the order is odd). s is normalised to f0. `evaluate(Ω)` returns complex H(jΩ). |
| Digital design | `digital_design.{hpp,cpp}`, `joint_fit.cpp` | Three-point matched (Vicanek), least-squares matched, **joint minimax refinement (default)** and bilinear transform with pre-warp. All Pass always uses the bilinear design, pre-warped at f0. |
| Update policy | `update_policy.hpp` | R15/R17 values behind an interface, **PROVISIONAL (counsel review pending)**. Nothing uses it yet. |
| Harness | `tools/accuracy_harness/` | Sweeps shapes × slopes × frequencies × gains × Q × sample rates and reports errors. Also has a `--case` per-section dump and a `--bench` design-cost benchmark. |

### Shape construction (all from public filter theory)

- **Low/High Cut:** Butterworth high-pass/low-pass of order N = slope/6, −3 dB at f0.
- **Low/High Shelf:** Butterworth shelf of order N. Poles sit on radius G^(−1/2N) and zeros on G^(+1/2N), so the gain at f0 is half the total in dB (after Orfanidis 2005).
- **Tilt Shelf:** a high shelf of total gain g, rescaled so it is −g/2 below f0, +g/2 above, and 0 dB at f0.
- **Bell / Notch / Band Pass:** the low-pass to band-pass mapping s → Q(s + 1/s), applied to a Butterworth low shelf, high-pass or low-pass of order floor(N/2). A 12 dB/oct Bell is then exactly the RBJ analog peaking prototype; the unit test checks this to 1e-9 relative.
- **Flat Tilt (PROVISIONAL):** a constant dB/oct tilt from 20 Hz to 20 kHz, built from first-order pole/zero pairs spaced half an octave apart. The gain parameter is the total tilt across that range. The pivot (0 dB) is at f0, and the gain at the 1 kHz reference is limited to ±30 dB.
- **All Pass:** Butterworth poles with mirrored zeros. Its magnitude is exactly 1.
- **Brickwall:** not implemented. It is a Phase 2 design, so the harness skips it.

### Q convention (R14): PROVISIONAL placeholder

- **Bell, Notch, Band Pass:** the displayed Q is the bandwidth Q of the band-pass mapping.
- **Cuts, shelves, Tilt Shelf, All Pass:** the displayed Q multiplies the Q of the dominant (highest-Q) Butterworth pole pair, so Q = 1 is maximally flat. 6 dB/oct shapes ignore Q.
- **Flat Tilt:** ignores Q and slope.

At Q 10–40 this turns a shelf into a section with a +20 to +46 dB resonance. Those prototypes are behind every remaining large miss (see below). The DSP lead and the Product designer still need to define this mapping (Phase 0 task).

### Analog → digital

1. **Three-point matched (`MatchedThreePoint`)**, after Vicanek 2016.
   - Each section's poles map exactly with z = exp(sT).
   - The numerator's |N|² is written as B0·φ0 + B1·φ1 + B2·φ2, with φ1 = sin²(ω/2), φ0 = 1 − φ1 and φ2 = 4φ0φ1.
   - B0, B1 and B2 are solved so the magnitude is exact at DC, at Nyquist and at the section's natural frequency. The coefficients are then recovered as the minimum-phase factor.
2. **Least-squares matched (`MatchedLeastSquares`)**: the same φ basis.
   - B0 is fixed so DC is exact. B1 and B2 are fitted by Lawson-weighted least squares up to Nyquist.
   - Notch and DC zeros stay exact. Poles above 0.95π are pulled down to 0.95π.
3. **Joint minimax refinement (`MatchedRefined`, the default)** (`joint_fit.cpp`).
   - **What is fitted:** all sections together. Levenberg–Marquardt minimises the dB error against the analog prototype on a fit grid below 0.9·Nyquist, and Lawson re-weighting turns the least-squares solution into a near-minimax one. This is the "minimum p-error" magnitude design of cascaded biquads (Deczky 1972) applied to an analog target.
   - **Parameters:** b and a for each section.
     - The analog section's structural zeros stay exact: notch zeros at their exact frequency on the unit circle, and zeros at DC.
     - Stability is enforced through the stability triangle (steps that leave it are rejected).
     - Numerators are reflected to minimum phase afterwards.
   - **Fit grid:**
     - log-spaced points over the band;
     - a window around f0, with a step that shrinks with the highest section Q;
     - very fine windows around notch zeros;
     - loose (±3 dB dead-zone) points between 0.9·Nyquist and Nyquist, so the response there stays sensible.

     Levels below −60 dB only have to stay below −60 dB.
   - **Starting points:** least-squares matched, then three-point matched, then pre-warped bilinear, stopping at the first that meets the target.
   - **Picking the winner:** candidates are compared on an independent, denser check grid, so a narrow spike between fit points cannot win.
   - **When it runs:** only if the starting design misses 0.07 dB.
   - **Optional correction sections** (`RefineOptions::extraSections`): when the order-N result still misses, it is refined again with identity biquads appended. The N+2 design is therefore never worse than the order-N one.
4. **Bilinear with pre-warp (`BilinearPrewarp`)**: the fallback. All Pass always uses it, pre-warped at f0, which makes its phase exact at f0.

## R12 results

**Sweep**
- 10 shapes, 9 slopes (Brickwall excluded).
- 36 centre frequencies (10 Hz–30 kHz, 1/3 octave).
- Gains of ±1, ±6, ±15 and ±30 dB.
- Q of 0.1, 0.707, 1, 3, 10 and 40.
- **7 sample rates:** 44.1, 48, 88.2, 96, 176.4, 192 and 384 kHz.
- 508,536 cases in total, 505,512 of them evaluated.

**Metric**
- The error is max |dB(digital) − dB(analog)| below 0.9·Nyquist, with both magnitudes floored at −60 dB.
- Verification uses the harness grid:
  - 384 log points;
  - 1/48 octave within ±2 octaves of f0;
  - a 1/(16·Q_max)-octave window;
  - 1/800 octave around notch zeros.
- Each full sweep takes about 6 minutes on this machine with all threads.

### Before and after (All Pass excluded; it passes everywhere, max phase error 2.4e-5°)

All 7 rates:

| Design | Pass ≤ 0.1 dB | Max error |
| --- | ---: | ---: |
| Before: least-squares matched per section (previous default) | 80.2 % (incl. All Pass) | 75.5 dB |
| Three-point matched (Vicanek baseline) | 73.3 % (incl. All Pass) | 82.8 dB |
| **After, order N (default):** joint refinement | **97.19 %** | 28.2 dB |
| **After, order N + 2:** joint refinement + 1 correction biquad | **99.994 %** (31 misses) | 28.2 dB |

Per shape, pass rate and worst-case error. These rows use the 4 original rates (44.1/48/96/192 kHz), so they compare like for like with the first run, which reported 77.9 % excluding All Pass.

| Shape | Before | After, order N | After, N + 2 |
| --- | --- | --- | --- |
| Bell | 84.6 % / 49.38 dB | 97.7 % / 0.38 dB | 100.0 % / 0.12 dB |
| Low Shelf | 77.8 % / 50.02 dB | 97.9 % / 15.68 dB | 100.0 % / 15.68 dB |
| High Shelf | 77.8 % / 67.77 dB | 97.8 % / 28.19 dB | 100.0 % / 28.19 dB |
| Tilt Shelf | 77.8 % / 60.55 dB | 97.8 % / 27.62 dB | 100.0 % / 27.62 dB |
| Low Cut | 86.1 % / 14.79 dB | 95.7 % / 1.97 dB | 100.0 % / 0.10 dB |
| High Cut | 51.4 % / 9.24 dB | 88.5 % / 0.27 dB | 100.0 % / 0.24 dB |
| Notch | 88.5 % / 16.88 dB | 96.3 % / 0.46 dB | 100.0 % / 0.05 dB |
| Band Pass | 32.2 % / 27.41 dB | 75.0 % / 0.73 dB | 100.0 % / 0.20 dB |
| Flat Tilt | 87.5 % / 0.13 dB | 100.0 % / 0.03 dB | 100.0 % / 0.03 dB |
| **All (4 rates)** | 77.9 % | 96.8 % | 99.99 % |

"100.0 %" is rounded; the exact miss counts are below.

### Miss clusters in the original design, and the cause of each

From `before_cases.csv`, 62,136 misses:

| Cluster | Misses | Max | Cause | Fixed by |
| --- | ---: | ---: | --- | --- |
| A. Low-pass-type sections near Nyquist (High Cut, Band Pass) | 8,665 | 12.0 dB | With exactly mapped poles, one biquad cannot follow an analog roll-off that keeps falling past Nyquist. The per-section fit then trades passband error against stopband error. | Joint fit: the poles move too. Where one biquad is all there is (12–18 dB/oct), only an extra section fixes it. |
| B. High Q, f0 ≥ 0.25·Nyq | 12,613 | 67.8 dB | High-Q poles near Nyquist; a per-section 3-point or least-squares fit cannot place the resonance. | Joint fit from several starts. |
| C. Very wide bands (Q 0.1) | 7,864 | 12.9 dB | The band-pass mapping pushes poles to about 10·f0, above Nyquist, where they get aliased or clamped. | Joint fit (the poles are re-placed). |
| D. f0 ≥ Nyquist | 5,685 | 51.9 dB | The prototype's resonance lies above Nyquist; only its skirt is in band. | Mostly fixed by the joint fit; residue below. |
| E. Flat Tilt | 136 | 0.13 dB | First-order sections near Nyquist. | Joint fit. |
| F. High Q (≥ 10), low f0 | 16,573 | 8.9 dB | Per-section errors of 0.03–0.06 dB add up over 8 sections. | Joint fit (errors cancel across sections). |
| G. Other (mostly 6–18 dB/oct Bells and shelves at 0.1–0.5·Nyq) | 10,600 | 16.9 dB | Few sections, so little freedom per section. | Joint fit; the rest needs N + 2. |

**Order-N residue after the joint fit.** 13,839 misses, almost all at 6–18 dB/oct. These designs have 1–2 biquads, and an order-N digital filter has no spare freedom for the analog skirt between about 0.5·Nyq and 0.9·Nyq. For f0 < 0.9·Nyq the worst order-N error is 2.38 dB, or 1.01 dB with Q ≤ 10. The pass rate at looser tolerances shows what relaxing R12 alone would take at order N:

| Tolerance | 0.1 dB | 0.25 dB | 0.5 dB | 1 dB | 2 dB |
| --- | ---: | ---: | ---: | ---: | ---: |
| Order N | 97.19 % | 99.18 % | 99.89 % | 99.96 % | 99.993 % |
| Order N + 2 | 99.994 % | 99.996 % | 99.997 % | 99.998 % | 99.998 % |

### Cases that still miss with N + 2 (31 of 505,512)

| Region | Q | Misses | Max error |
| --- | --- | ---: | ---: |
| f0 0.5–0.9·Nyq | 10 | 4 | 0.119 dB |
| f0 ≥ 0.9·Nyq | 3 | 2 | 1.03 dB |
| f0 ≥ 0.9·Nyq | 10 | 9 | 15.7 dB |
| f0 ≥ 0.9·Nyq | 40 | 16 | 28.2 dB |

All 31 are at 44.1 or 48 kHz. The large ones are Low/High/Tilt Shelf at 18–36 dB/oct, ±30 dB and Q 10–40, with f0 = 20.5–25.8 kHz.
- **The prototypes themselves are extreme.** The +30 dB, Q 40 high shelf at 25.8 kHz has a −28 dB analog notch at 14.5 kHz, a direct product of the provisional Q convention.
- **This is not proven to be a hard limit.** The local optimiser failed from three starting points. A global or multi-start search might do better, but I found no published closed-form method that targets these.
- **For f0 ≤ 0.9·Nyquist only 4 cases miss, at 0.119 dB.** Optimiser tuning would most likely close these.

### Design cost (single thread, 48 kHz, 4,320 designs across the grid)

| Design | p50 | p90 | p99 | max |
| --- | ---: | ---: | ---: | ---: |
| Three-point matched | 0.9 µs | 3.7 µs | 8.4 µs | 21 µs |
| Least-squares matched | 123 µs | 526 µs | 1.4 ms | 3.3 ms |
| Joint refinement (order N) | 1.1 ms | 17 ms | 35 ms | 59 ms |
| Joint refinement (N + 2) | 1.1 ms | 20 ms | 40 ms | 76 ms |

The refined design cannot run inside R15's 32-sample redesign interval (0.67 ms at 48 kHz). It must run off the audio thread (R19 allows this). Under automation the audio path would use a fast design and crossfade to the refined design when the band settles. Another option is a precomputed, interpolated coefficient table per shape and slope. Either one is a Phase 1 design decision, and the R15 values are themselves still PROVISIONAL.

## Revised R12 (adopted 2026-11-14) and the conditional correction section

The project owner approved both proposals. Area 01 R12 now reads:
- **f0 ≤ 0.9·Nyquist:** ≤ 0.1 dB below 0.9·Nyquist.
- **Floor:** wherever the analog response is below −60 dB, the digital response only has to stay below −60 dB.
- **f0 > 0.9·Nyquist:** 1.1 dB for Q ≤ 3; **TBD** above Q 3 until R14 fixes the Q mapping.
- **All Pass:** keeps its exact-phase requirement.
- **Correction section:** a band may use one correction biquad, but only when order N misses.

The tolerance lives in one function, `r12ToleranceDb()` (`digital_design.hpp`), which the designer and the harness share.

### How the extra section is decided (design time)

`designRefined()` (the default behind `designDigital()`) works in three steps:
1. **Order N first.** It designs the band at the analog order: least-squares matched, then the joint refinement from up to three starts.
2. **Measure the miss.** It measures the result on the independent dense check grid (`denseCheckMaxErrorDb`).
3. **Add a section only if needed.** If that error exceeds 0.9 × the band's R12 target (`r12DesignTargetDb`: the R12 tolerance, with 1.1 dB assumed for the TBD region), it refines again with one identity biquad appended. It keeps the result only if the dense-check error improves.

The decision is a pure function of the band's settings and the sample rate, made while designing. It is not measured at runtime. So it slots unchanged into Phase 1's off-thread redesign.

A section-count change is then just another structure change, and it uses the R17 crossfade (PROVISIONAL). `RefineOptions::extraSections = 0` gives order N only. `extraOnlyWhenNeeded = false` always tries the extra section.

### Results against the revised R12 (full sweep, 7 rates, 505,512 cases)

| Shape | Revised R12 pass | Misses | TBD cases: max error | Uses correction section |
| --- | ---: | ---: | --- | ---: |
| Bell | 99.999 % | 1 | 720: 0.11 dB | 2.25 % |
| Low Shelf | 99.999 % | 1 | 720: 15.7 dB | 1.86 % |
| High Shelf | 99.999 % | 1 | 720: 28.2 dB | 1.86 % |
| Tilt Shelf | 99.999 % | 1 | 720: 27.6 dB | 1.86 % |
| Low Cut | 100 % | 0 | 90: 0.77 dB | 3.48 % |
| High Cut | 100 % | 0 | 90: 0.24 dB | 12.1 % |
| Notch | 100 % | 0 | 90: 0.05 dB | 3.37 % |
| Band Pass | 100 % | 0 | 90: 0.73 dB | 25.2 % |
| Flat Tilt | 100 % | 0 | 0 | 0 % |
| All Pass | 100 % (phase ≤ 2.4e-5°) | 0 | 90: 0 | 0 % |
| **All** | **99.999 %** | **4 of 502,182** | 3,330: 28.2 dB | **2.88 %** |

**Before → after under the revised R12 (TBD excluded):**

| Design | Pass |
| --- | ---: |
| Per-section least-squares (the first default; first sweep, 4 rates) | 80.4 % |
| Order N with joint refinement (7 rates) | 97.3 % |
| Default (conditional correction section) | 99.999 % |

**The 4 remaining misses** are all at 48 kHz, Q 10, with f0 at 0.68–0.89·Nyquist, and errors of 0.106–0.119 dB:
- Bell 96 dB/oct at 20.48 kHz, −30 dB.
- Low, High and Tilt Shelf, 30 dB/oct at 16.25 kHz, ±30 dB.

In the Bell case the dense check missed what the harness grid found (0.119 dB), so the extra section was never tried. Each of the three shelf cases already uses the extra section and still misses by about 0.01 dB. These should close with a finer dense-check window or more optimiser starts (open question 6).

**TBD region (f0 > 0.9·Nyquist, Q > 3):** 3,330 cases, reported separately. Large errors appear only at Q 10–40 on shelves, and they come from the provisional Q convention.

### Share of the parameter space that uses the extra section

2.88 % of evaluated bands (14,543 of 505,512) carry the extra section. Over the whole grid that adds 0.87 % more sections than order N alone. The share is concentrated where order N has little freedom:

| Slope | Bands using the section |
| --- | ---: |
| 6 dB/oct | 6,270 |
| 12 dB/oct | 4,746 |
| 18 dB/oct | 2,718 |
| 24 dB/oct and steeper | 809 |

By shape: 25 % of Band Pass and 12 % of High Cut bands use it. By centre frequency, 62 % of these bands have f0 < 0.25·Nyquist. That is not a high-frequency effect: it is the 1–2-biquad shapes failing to follow the analog skirt towards Nyquist. Every sample rate is affected, 44.1 kHz through 384 kHz.

### CPU cost

**Audio path.** One extra biquad (SVF) section per affected band.
- **Worst case:** a 1-section band doubles to 2 sections (×2 for that band).
- **Typical:** +0.87 % sections across the grid. For a 24-band session, +1 section per affected band.
- **Rough figure:** at about 2–3 ns per section per sample (double precision, scalar, x86-64), one extra section costs roughly 0.01 % of a core at 48 kHz.

**Design (off-thread), single thread, 48 kHz, 4,320 designs across the grid:**

| Design | p50 | p90 | p99 | max |
| --- | ---: | ---: | ---: | ---: |
| Three-point matched | 0.6 µs | 1.9 µs | 2.1 µs | 12.8 µs |
| Least-squares matched | 78 µs | 396 µs | 424 µs | 555 µs |
| Default, order N + conditional section | 1.0 ms | 16.5 ms | 32 ms | 66 ms |
| Order N only | 1.0 ms | 15.2 ms | 33 ms | 62 ms |

The conditional section adds little to the design cost: max 66 ms against 62 ms. Even so, the refined design is far too slow for R15's 32-sample redesign interval, so it has to run off the audio thread. Under automation the audio path uses a fast design and crossfades to the refined one; precomputed tables are the other option (open question 2).

## Open DSP questions

1. **R12 TBD region:** the tolerance for f0 > 0.9·Nyquist with Q > 3, once R14 settles the Q mapping. The design uses 1.1 dB as its trigger there in the meantime.
2. **Design cost under automation** (R15): a fast path with a settle-time refined design, or precomputed tables?
3. **Q convention (R14):** should shelves and cuts accept Q 10–40 at all, and with what mapping? This drives every remaining large miss.
4. **Odd-order Bell / Notch / Band Pass:** with the band-pass mapping, 18 and 30 dB/oct come out the same as 12 and 24.
5. **Slope semantics** for Flat Tilt (currently ignored), and whether Tilt Shelf's slope means shelf order.
6. **The 4 remaining revised-R12 misses (0.106–0.119 dB, 48 kHz, Q 10):** a finer dense-check window or more optimiser starts is likely to close them.
7. **Brickwall:** a Phase 2 design, not covered here.

## References

| Reference | Used for | Verified? |
| --- | --- | --- |
| M. Vicanek, "Matched Second Order Digital Filters", 14 Feb 2016, https://www.vicanek.de/articles/BiquadFits.pdf | Exact pole mapping and the φ-basis 3-point magnitude match | Partly. The title, date and URL were confirmed by web search. Only search excerpts were read, not the full PDF; the φ-basis algebra was re-derived here independently. |
| S. J. Orfanidis, "Digital Parametric Equalizer Design With Prescribed Nyquist-Frequency Gain", J. Audio Eng. Soc. 45(6), 444–455, June 1997 | Background on avoiding cramping | Citation confirmed by web search. **The plan dates this title 2005; the correct year is 1997.** Not read. |
| S. J. Orfanidis, "High-Order Digital Parametric Equalizer Design", J. Audio Eng. Soc. 53(11), 1026–1046, Nov 2005 | High-order Butterworth shelf and the band-pass mapping | Citation confirmed by web search. Not read; the construction used is standard Butterworth/frequency-transformation theory. |
| A. G. Deczky, "Synthesis of recursive digital filters using the minimum p-error criterion", IEEE Trans. Audio Electroacoust. AU-20(4), 1972 | Magnitude-only optimisation of cascaded biquads (joint refinement) | From memory; not looked up in this session. |
| K. Levenberg (1944), D. W. Marquardt (1963); C. L. Lawson (1961) | Damped Gauss–Newton solver; IRLS towards minimax | Standard numerical methods; not looked up. |
| R. Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter coefficients" | Analog peaking prototype used as an independent test reference | From memory; not looked up. |
| V. Zavalishin, *The Art of VA Filter Design*; A. Simper, Cytomic SVF notes | R18 kernel (Phase 1, PROVISIONAL) | Not used yet; not verified. |
