# 16 EQ Match fitting and apply

Am-Q fits the smoothed, level-aligned difference curve with a greedy matching-pursuit fit of up to 24 bands. Each band is refined by line search. The result is applied as normal bands through the parameter setter, with separate Amount and Bands sliders, as one undo step.

## Requirements

1. R1. Pressing Match starts a fit on a worker thread. The GUI keeps rendering at full frame rate during the fit.
2. R2. The fit target is the difference curve from tab 20, already smoothed and level-aligned, on Am-Q's fixed log-frequency grid.
3. R3. The fit error is the mean-squared dB error between target and the summed band response, over 20 Hz to 20 kHz. Points above the usable limit (the lower of 20 kHz and a margin below Nyquist) get zero weight.
4. R4. Band responses used in the fit are the exact magnitude responses of Am-Q's own filter designs at the session sample rate. These are the same coefficients the audio path uses (tab 01).
5. R5. Each fit round tries Low Cut and High Cut at every slope Am-Q offers, Low Shelf and High Shelf, and a Bell at each of the largest residual peaks.
6. R6. Each round keeps the single candidate that most reduces the error, then subtracts that band's response from the residual.
7. R7. Every candidate is refined by line search over frequency, gain and Q before it is scored.
8. R8. Fitting stops when 24 bands are fitted, when no candidate reduces the error, or when the relative improvement falls below a tunable tolerance.
9. R9. Fitted values always stay inside Am-Q's ranges: frequency 10 Hz to 30 kHz, gain ±30 dB, Q 0.025 to 40. Shelves and cuts are limited to a non-resonant Q range.
10. R10. The fit returns bands ordered by their error contribution, strongest first. The Bands slider applies a prefix of this list.
11. R11. An Amount slider (0 to 100 %) scales every fitted gain. At 100 % the applied curve equals the fit.
12. R12. Apply writes bands through set(id, plain, source=EQ Match). It writes shape, frequency, gain, Q and slope, and records the whole apply as one undo step.
13. R13. Moving the Amount or Bands slider updates the applied bands live. The final value merges into the same undo step as the apply.
14. R14. If fewer free bands exist than requested, Am-Q applies as many as fit, strongest first. It shows a clear message with the count that was dropped.
15. R15. Match is enabled only when both captures meet the minimum-capture rule of tab 17, or the reference comes from a file. The GUI shows a clear ready or not-ready state.
16. R16. The fit shows progress and can be cancelled. Cancelling leaves the band state unchanged.
17. R17. The fit is deterministic: the same target and settings always give the same bands.
18. R18. The fit never produces NaN, Inf or out-of-range values for any input curve.

## Am-Q design

**Target and grid.** The engine receives the dB difference curve on a fixed log grid (planning value: 1/48 octave, 20 Hz to 20 kHz, about 480 points). The grid does not depend on GUI size. Above the usable frequency limit, points are weighted zero rather than extended flat.

**Fitting method.** This is greedy matching pursuit (forward stagewise fitting) with least squares in dB. Each round evaluates every candidate family; there are no entry thresholds. The error reduction decides, which is simple and easy to test.

**Candidates.**

- Cuts: one candidate per Am-Q cut slope (tab 15) and per edge. The start frequency is where the edge deficit begins.
- Shelves: low and high. The start gain is the mean residual over the outer edge octave.
- Bells: at the top K residual peaks. Peaks are scored by residual area over a local window, so broad deviations beat narrow spikes. K is tunable (planning value 12).

**Refinement.** For each candidate, run a few sweeps of coordinate descent. Each sweep uses a golden-section line search over log-frequency, then gain, then log-Q. Every value is clamped to the R9 ranges before evaluation. A bracket only moves if the error drops.

**Ordering and Amount.** Bands are stored in the order they were accepted, which is strongest first. The Amount slider multiplies each gain in dB. For cuts it blends the applied slope towards the gentlest available slope, or bypasses the cut below a set Amount (see Open questions).

**Apply path.** A single transaction calls the parameter setter (tab 09) for every changed field with source = EQ Match, so the undo system records one step. Bands that a previous match created are tagged. A re-apply replaces those bands rather than adding more.

**Threading.** The fit runs on the analysis worker. Every 10 ms or so it posts progress and checks a cancel flag. Results are handed to the GUI thread through a lock-free mailbox. No audio-thread work is involved.

## Implementation tasks

- [x] Write the EQ Match fitting spec: grid, error metric, candidate families, stop rule, ranges and ordering (DSP lead, Phase 0). Done on 2026-11-16 as the header of `core/engine/include/amq/engine/eq_match_fit.hpp`: the 1/48-octave grid (480 points, 20 Hz to 20 kHz), the weighted mean squared dB error, the candidate families (cuts at every slope, shelves, K = 12 peak bells by area), golden-section coordinate descent (3 sweeps), the stop rule (24 bands, no improvement, relative tolerance 1e-3), the ranges (non-resonant Q 0.3 to 1.0 for shelves and cuts) and the ordering (strongest first). PROVISIONAL constants
- [ ] Prototype the greedy fitter offline against synthetic curves and real mix pairs; tune K, tolerance and line-search sweeps (DSP lead, Phase 0). Progress (2026-11-16): the fitter is implemented and its synthetic-curve suite passes (a three-band curve is fitted to 0.008 dB² mean squared error in 0.7 s; an edge deficit becomes a cut). Still to do: real mix pairs and tuning K, the tolerance and the sweeps on them
- [x] Expose a magnitude-response evaluator for every band shape at any sample rate, shared with the audio path (DSP lead, Phase 1). `core/engine/include/amq/engine/band_response.hpp`: the engine, the design worker and the evaluator share one parameter-to-spec conversion and one design call per tier (fast or refined). Tested against the running engine: within 4e-7 dB of measured steady-state gain. Note for the fitter: a refined design costs about 6 ms per band
- [x] Implement the fitter as a pure, deterministic library with no GUI or host dependencies (DSP lead, Phase 3). Done on 2026-11-16 (`core/engine/eq_match_fit.{hpp,cpp}`): no GUI or host dependencies; deterministic (tested by re-running); the search uses the analog prototypes and the kept candidate is scored and subtracted with the exact digital response of the audio path's design (R4)
- [x] Add range clamping and the non-resonant Q limit for shelves and cuts (DSP lead, Phase 3). Done on 2026-11-16: every candidate is clamped to 10 Hz - 30 kHz, ±30 dB, Q 0.025 - 40, and shelves and cuts to Q 0.3 - 1.0 (PROVISIONAL); the fuzz test checks every fitted band
- [x] Run the fit on the analysis worker with progress, cancel and a lock-free result hand-off (Plugin and host engineer, Phase 3). Done on 2026-11-16 (`EqMatchStage`): Match copies the latest snapshot's curve and fits on its own thread (not the analysis worker, so captures keep running), with progress and cancel checked between candidates; the result waits in a mailbox the GUI takes once. Cancelling leaves the bands unchanged (R16)
- [x] Implement the apply transaction through set(id, plain, source=EQ Match) as one undo step, with match-band tagging and replace-on-reapply (Plugin and host engineer, Phase 3). Done on 2026-11-16 (`ui/eq_match_apply`): every write carries source EQ Match; the apply is one undo step and Amount / Bands slider moves merge into it (a later move reopens the step while it is still the latest, so other edits never merge); bands an earlier match wrote are tagged and reused, so a re-apply replaces them; too few free bands apply the strongest with "N fitted bands not applied: no free bands." (R14). Tested
- [x] Build the Match button, ready state, progress display, Amount slider, Bands slider and free-band message (GUI engineer, Phase 3). Done on 2026-11-16 in the editor's EQ Match panel: Match (enabled only with a valid curve, R15) turning into Cancel with a progress readout, Amount and Bands value boxes that apply live, the ready state of each capture, and the message line. Wording PROVISIONAL
- [ ] Define the wording and states of the EQ Match panel (Product designer, Phase 3)
- [x] Build the synthetic-curve regression suite and the fuzz harness for the fitter (Build and QA engineer, Phase 3). Done on 2026-11-16 (`tests/engine/test_eq_match_fit.cpp`): curves made of known bands are recovered (bell and shelf found within 0.15 octave and 0.75 dB; a cut for an edge deficit), random and hostile curves (huge values, NaN, negative weights) give finite, in-range, deterministic results, zero-weight regions are ignored, and cancel returns no bands

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Synthetic match accuracy | Generate 200 random Am-Q EQ settings (1 to 12 bands, all shapes). Use the response as the target and fit. | RMS error ≤ 0.5 dB over 20 Hz to 20 kHz for every case |
| Real-pair sanity | Fit 20 curated reference/input pairs. A listener and the analyzer compare the result. | Residual RMS lower than the unfitted target in every case; no audible resonance artefacts reported |
| Parameter ranges | Fit extreme targets (±60 dB steps, spikes, ramps). | All bands inside 10 Hz to 30 kHz, ±30 dB, Q 0.025 to 40; shelves and cuts inside the non-resonant range |
| Fuzz | Feed 100 000 random curves, including NaN, Inf, constant and empty inputs. | No crash, no NaN or Inf in output; invalid input gives a clear error |
| Determinism | Run the same fit 10 times on different threads. | Bit-identical band lists |
| GUI responsiveness | Fit 24 bands at the largest grid while profiling the GUI thread. | No GUI frame over 20 ms; progress updates at least every 100 ms |
| Fit time | Time a 24-band fit on the minimum-spec machine. | Under 2 s |
| Amount slider | Apply at 0, 50 and 100 %. | 0 % is flat; 50 % halves every bell and shelf gain in dB; 100 % equals the fit |
| Bands slider | Set N from 1 to 24. | Exactly the first N fitted bands are active, strongest first |
| Undo | Apply, move the sliders, then undo once. | Band state returns exactly to the pre-apply state |
| No free bands | Fill 20 bands, then apply a 10-band fit. | 4 bands written, strongest first; message reports 6 dropped |
| Cancel | Cancel mid-fit. | Band state unchanged; no undo entry created |

## Open questions

- How should Amount act on cuts: blend slope, reduce frequency excursion, or switch the cut off below a threshold?
- Should a final joint refinement of all bands (for example Levenberg-Marquardt in dB) run after the greedy pass as an optional “polish”?
- Should the fit weight frequencies by perceptual importance (for example ERB density) instead of uniformly per octave?
- Should matching be allowed to use the Dynamic or Spectral band modes, or only static shapes? Planning answer: static only for v1.
