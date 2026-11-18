# 18 EQ Match averaging

Am-Q averages EQ Match captures by power (Welch method) on a fixed log grid. Silent frames are skipped, and the user chooses between the whole capture and the last N seconds. Averaging can forget old audio and does not depend on GUI size.

## Requirements

1. R1. Each capture accumulates mean power per log-grid point: the sum of per-frame power divided by the number of counted frames.
2. R2. A frame is counted only if its broadband level is at or above −90 dBFS. Skipped frames add nothing.
3. R3. The averaging window is selectable: whole capture (default) or last N seconds (N from 1 to 60 s).
4. R4. In last-N-seconds mode, audio older than N seconds stops affecting the result within one block length (planning value 0.5 s).
5. R5. Accumulation is numerically stable for captures of at least 1 hour (no drift above 0.01 dB).
6. R6. Each log-grid point averages the power of all FFT bins it covers. Points narrower than one bin are interpolated from the power spectrum.
7. R7. Captures expose counted frames, counted seconds and skipped frames for the GUI.
8. R8. Captures run only while recording is on. Pausing keeps the accumulators unchanged.
9. R9. Match averaging, grid and smoothing are independent of editor size and display resolution.
10. R10. The live display path (ballistics, peak-hold) and the match path share the FFT frames but keep separate state.

## Am-Q design

**Power averaging.** For each counted frame, Am-Q computes the windowed power spectrum, merges channels by mean power (tab 17), and maps it onto the log grid. Each grid point averages the power of the bins it spans; points narrower than a bin use interpolation. Per-point sums are kept in double precision. Power averaging is the standard Welch estimate and gives a level-consistent spectrum.

**Gate.** A frame's level is the mean power over 20 Hz to 20 kHz, in dBFS. Frames below −90 dBFS are skipped and counted as skipped. Gating on broadband level means a single hum line cannot keep silence in the average.

**Window modes.**

- Whole capture: one running sum and count per grid point.
- Last N seconds: a ring of block sums (planning value 0.5 s per block) plus a running total. When a block expires, it is subtracted. Rounding drift is cleared by re-summing the ring each time it wraps.

**Grid.** A fixed log grid (planning value 1/48 octave from 10 Hz to the capture's Nyquist) is shared by the fitter (tab 16) and the difference curve (tab 20). It is not tied to pixels. The match therefore never changes with window size (R9).

**Smoothing.** Am-Q does not smooth each capture. Fractional-octave smoothing is applied once, to the dB difference curve (tab 20). The live display may use its own smoothing (tab 12).

## Implementation tasks

- [x] Specify the power accumulator, gate, window modes and log-grid mapping (DSP lead, Phase 0). Done on 2026-11-16 in the `match_capture.hpp` header: double sums of mean power per grid point, the -90 dBFS broadband gate, whole capture or the last N seconds from 0.5 s blocks, and bin-coverage averaging with interpolation for points narrower than a bin
- [x] Prototype magnitude versus power averaging and peak versus broadband gating on test material; record the results (DSP lead, Phase 0). Done 2026-11-18 (`tests/analysis/test_match.cpp`, report "EQ Match averaging and gating prototype": frames made in the spectral domain, each bin the model power times an exponential draw, as windowed noise gives; the EQ curve per 1/3 octave against the known truth). Averaging: with level-dependent material (loud, brighter sections alternating with quiet, darker ones; the reference has +6 dB above 4 kHz in its loud sections only), power averaging recovers the long-term power ratio (+5.92 dB against +5.97 at 16 kHz with the quiet sections 8 dB down) while magnitude averaging is pulled toward the quiet sections (+5.56); with the quiet sections 24 dB down both follow the loud ones (+5.95, +5.88). Magnitude averaging has no variance advantage: 3 s captures spread 0.72 against 0.79 dB at 25 Hz, 0.33 against 0.34 dB at 635 Hz, 0.06 both at 16 kHz. Gating: silence carrying a 50 Hz hum line passes a peak gate whenever the line is above -90 dB (203 to 234 of 234 frames), diluting the capture by up to 3 dB, which shifts level alignment and the counted seconds; the broadband gate (mean bin power over 20 Hz-20 kHz) rejects such frames up to a hum of -70 dB in its bin (none counted), partly at -60 dB (46 of 234) and mostly not at -50 dB (191). Decision: power averaging and the broadband gate, as built. A hum line louder than about -60 dB in its bin defeats both gates; a gate that discounts the few loudest bins would handle it, if real captures show it matters
- [x] Implement the log-grid power mapper with bin-coverage weights and sub-bin interpolation (DSP lead, Phase 3). Done on 2026-11-16 (`MatchCapture`)
- [x] Implement whole-capture and last-N-seconds accumulators with drift control (DSP lead, Phase 3). Done on 2026-11-16: whole capture in double sums (an hour of frames drifts under 0.01 dB, tested), last N seconds (1 to 60 s) summed exactly from a ring of 0.5 s blocks, so old audio stops counting within one block and no running total can drift
- [x] Expose capture statistics (counted, skipped, seconds) to the GUI through snapshots (Plugin and host engineer, Phase 3). Done on 2026-11-16: counted seconds and skipped frames per capture in the `MatchSnapshot`
- [x] Add the averaging-window selector and capture statistics display (GUI engineer, Phase 3). Done on 2026-11-16 in the EQ Match panel (Whole / Last N s, seconds per capture)
- [x] Build averaging tests: stationary noise, gated silence, level steps, long captures (Build and QA engineer, Phase 3). Done on 2026-11-16 (`tests/analysis/test_match.cpp`)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Power mean | Feed two equal-length segments of white noise at −20 and −40 dBFS. | Captured level equals the power mean of the two segments within 0.1 dB |
| Silence gate | Interleave 2 s of noise with 10 s of digital silence and 10 s at −100 dBFS. | Spectrum equals the noise-only capture within 0.1 dB; skipped count matches |
| Hum does not defeat gate | Feed silence plus a −80 dBFS 50 Hz tone that leaves broadband level below −90 dBFS. | Frames skipped |
| Last N seconds | Capture 30 s of pink noise, then 10 s of brown noise with N = 5 s. | Result matches a brown-noise-only capture within 0.2 dB RMS |
| Long capture | Capture 2 h of stationary noise at maximum resolution. | Drift under 0.01 dB versus a double-precision reference |
| Grid mapping | Capture a sine at each grid frequency. | Energy appears at the correct grid point; total power preserved within 0.1 dB |
| GUI size independence | Repeat a capture and fit at three editor sizes. | Bit-identical difference curve and fitted bands |
| Pause | Pause recording for 10 s of loud audio. | Accumulators unchanged |

## Open questions

- Is 0.5 s the right block length for last-N-seconds mode? Should N be continuous or stepped?
- Should Am-Q show a per-point confidence band (from frame-to-frame variance) on the difference curve?
- Should the gate be relative (for example 60 dB below the capture's loudest frame) as well as absolute?
