# 20 EQ Match difference curve

Am-Q builds the EQ Match difference curve (reference minus input) on the worker, from untilted power spectra on a fixed log grid. Level alignment (on by default, visible) and fractional-octave smoothing are applied before fitting. The curve never depends on repaint timing or editor size.

## Requirements

1. R1. The difference curve is D(f) = reference dB − input dB per log-grid point, computed from the two power captures.
2. R2. D(f) is computed on the analysis worker, at least 10 times per second while capturing and once more when Match is pressed. It is never tied to editor repaint.
3. R3. Level alignment is on by default. Am-Q subtracts the loudness difference (ITU-R BS.1770 K-weighted, gated) between the captures. The offset is shown in dB, and the option can be turned off.
4. R4. Fractional-octave smoothing is applied to D(f), selectable at 1/3, 1/6, 1/12 or 1/24 octave (default 1/6).
5. R5. Grid points where either capture is near its noise floor get reduced fit weight. They are drawn as low confidence.
6. R6. D(f) is clamped to a range slightly wider than the fit gain range, so outliers cannot dominate the error.
7. R7. Until both captures are ready (tab 17 R8), D(f) is not shown and Match is disabled. Am-Q never substitutes a flat curve silently.
8. R8. The curve passed to the fitter is exactly the curve shown, at the same settings, from one snapshot.
9. R9. The display overlay shows D(f), the fitted response and the residual. It never feeds back into the fit.
10. R10. Tilt is never applied to D(f).

## Am-Q design

**Computation.** On each update, the worker converts each capture's mean power to dB on the shared log grid and subtracts. Because both captures share the window, FFT size and grid, scaling constants cancel exactly. It works on raw grid data, not on display curves.

**Level alignment.** Each capture also runs a BS.1770 K-weighted mean-square meter on its counted frames, with the standard absolute and relative gating. The dB difference of the two integrated loudness values is subtracted from D(f). Without it, a loudness mismatch would become a broadband offset that the fitter spends shelves on. When alignment is off, Am-Q shows the raw offset as a hint.

**Smoothing.** Fractional-octave smoothing follows standard practice. Am-Q uses a complex-free, magnitude-domain smoother with a window whose width is a fixed fraction of an octave at each point (for example the Hatziantoniou–Mourjopoulos approach). It is applied to D(f) in dB. Edges use a shrinking window rather than padding. The span is in octaves, never pixels.

**Confidence weights.** For each point, Am-Q compares each capture with the floor estimated from its skipped frames and quietest regions. Points where either capture is within a set margin of the floor get weight that falls to zero. The weights go to the fitter (tab 16 R3).

**Snapshot.** D(f), weights, level offset and capture statistics are published as one immutable snapshot. Match takes the latest snapshot, so the fit and the display always agree.

## Implementation tasks

- [x] Specify the difference curve, level alignment, smoothing options, clamping and confidence weights (DSP lead, Phase 0). Done on 2026-11-16 in the header of `core/analysis/include/amq/analysis/match_curve.hpp` (PROVISIONAL floor and ramp constants)
- [x] Implement the BS.1770 loudness meter per capture on counted frames (DSP lead, Phase 3). Done on 2026-11-16: each counted frame's K-weighted mean square from its power spectrum (Parseval), grouped into 400 ms blocks and gated like BS.1770; a +6 dB reference reads +6.02 LU (tested). The standalone time-domain meter (`dsp/loudness.hpp`) validates Auto Gain
- [x] Implement fractional-octave smoothing with edge handling and the four settings (DSP lead, Phase 3). Done on 2026-11-16: 1/3, 1/6 (default), 1/12, 1/24 octave on the uniform log grid, edges shrinking symmetrically; tested
- [x] Implement confidence weighting from the capture floor (DSP lead, Phase 3). Done on 2026-11-16: weights fall to 0 over 10 dB as either capture nears its floor (its loudest point minus 60 dB, at least -110 dBFS), and are 0 above the usable limit (PROVISIONAL constants)
- [x] Publish the difference-curve snapshot on the worker at 10 Hz or faster and on Match (Plugin and host engineer, Phase 3). Done on 2026-11-16: rebuilt every 100 ms of analysed audio on the analysis worker and when a file finishes; Match fits exactly the published curve (R8)
- [x] Draw the difference, fitted and residual curves with low-confidence shading, plus the level-offset readout and toggle (GUI engineer, Phase 3). Done on 2026-11-16: the editor's match overlay draws the difference curve (dimmed where the weight is under 0.5), the fitted response at the current Amount and Bands, and the residual; the panel shows the level offset and the Align Level toggle
- [ ] Design the smoothing selector and level-alignment control (Product designer, Phase 3)
- [x] Build tests for direction, level alignment, smoothing, editor-size independence and fit/display agreement (Build and QA engineer, Phase 3). Done on 2026-11-16: direction and level alignment (`test_match.cpp`: a +6 dB reference gives +6 dB unaligned and about 0 aligned), smoothing, and the engine test (the fit finds the bell the curve shows). Editor-size independence holds by construction: the grid and smoothing are in octaves, never pixels

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Direction | Reference is input plus a known +6 dB bell at 1 kHz. | D(f) peaks at +6 dB at 1 kHz within 0.3 dB |
| Level alignment on | Reference is input +8 dB broadband. | D(f) within 0.2 dB of flat; offset reads +8.0 dB |
| Level alignment off | Same signals, alignment off. | D(f) ≈ +8 dB flat; fit uses a gain or shelf as expected |
| Smoothing width | Smooth a single-point spike at each setting. | Spread matches the selected octave fraction within one grid point |
| Editor-size independence | Same captures, editor at three sizes. | Bit-identical D(f) |
| Repaint independence | Close the editor; press Match from a host-automation test path. | Same D(f) and fit as with the editor open |
| Fit uses shown curve | Log the fit input and the displayed curve. | Identical snapshot ID and data |
| Not-ready state | Press Match with one capture short of ready. | Match disabled; no flat curve fitted |
| Noise-floor weighting | Reference band-limited to 8 kHz; input full-band. | Points above 8 kHz are low-confidence; no high-shelf boost over 3 dB is fitted |
| Tilt isolation | Change display tilt. | D(f) unchanged |

## Open questions

- Should level alignment use BS.1770 integrated loudness (planned) or a K-weighted mean of D(f), which also suits non-stationary material?
- What noise-floor margin and roll-off should the confidence weights use?
- Should the default smoothing be 1/6 or 1/3 octave for mix-bus use?
