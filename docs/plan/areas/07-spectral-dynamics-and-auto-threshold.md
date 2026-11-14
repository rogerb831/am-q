# 07 Spectral dynamics and auto-threshold

Am-Q's spectral dynamics runs an STFT side chain over log-spaced analysis bands whose width is set by Density. Each band has its own gain and frequency-scaled times. The automatic threshold sits |range| dB below a slow, instant-attack reference envelope, capped at 30 dB.

## Requirements

1. R1. Spectral dynamics works over the band's response span, from its lower to its upper edge. With no band limits it defaults to 20 Hz–20 kHz.
2. R2. Density (0–100, default 50) sets analysis resolution. Reference behaviour runs from about 1/4 octave at 0 to about 1/16 octave at 100, with finer bands at high frequencies.
3. R3. The analysis band count is bounded, with a fixed minimum and maximum. At maximum Density and full span, total CPU stays within the 3 % budget.
4. R4. Per-analysis-band attack and release come from the band's Attack and Release knobs. They are longer at low frequency; the ratio is tuned by listening.
5. R5. Changes to Density or band edges rebuild the grid without allocating on the audio thread. They do not click: residual stays below −90 dBFS.
6. R6. Spectral Tilt is a per-band toggle. It tilts the per-band threshold across frequency by a fixed dB/octave amount, which we tune by listening.
7. R7. The automatic threshold equals the side-chain level relative to a slow reference envelope, offset by −|range| dB. The offset is capped at 30 dB.
8. R8. Auto engages only through Dyn Auto. There is no hidden mode tied to the Threshold value.
9. R9. The auto-threshold envelope has an instant attack and a release of several seconds. After reset, transport start or enabling Auto, it settles within 0.5 s.
10. R10. On silence or very low input the auto-threshold envelope holds a floor. No gain change and no NaN result.
11. R11. Soft-knee width grows with band width and with |range|, by a documented Am-Q mapping.
12. R12. The spectral path's latency is reported to the host and compensated against the dry path (Tab 02).

## Am-Q design

- **STFT side chain.** A windowed STFT with overlap runs on the side chain. FFT size follows the phase-mode resolution (Tab 02).
- **Analysis bands.**
  - Bands are log-spaced across the span. Width is our own function of Density and centre frequency, narrower at high frequencies.
  - Each band has overlapping weights, triangular or raised-cosine. We choose by listening.
  - Band level is the maximum of the weighted bin magnitudes. Power-sum pooling is an alternative we test in Phase 0.
- **Per-band gain.** The same gain computer and dual-release smoother as Tab 06 run per analysis band, at frame rate. Times are scaled linearly in log frequency between a low and a high anchor.
- **Apply.** Band gains are interpolated to bins with the same overlapping weights, then applied on the FFT path (decided). Tilt adds a per-band dB offset proportional to octave distance from the band centre.
- **Grid rebuild.** Buffers are preallocated for the maximum band count. A new grid is built off the hot path and swapped at a frame boundary. Gains crossfade over one frame.
- **Auto-threshold reference.**
  - A peak envelope of |side chain| with instant attack and a release of several seconds. We start from about 10 s and tune by listening.
  - During a warm-up of about 0.5 s a fast envelope runs in parallel. Its state is then copied into the slow one.
  - The envelope has a level floor so that silence gives no gain change.
  - One reference design serves both non-spectral and spectral bands.
- **Knee.** Width is a base value plus terms in bandwidth (octaves) and |range|. The DSP lead tunes the coefficients.
- **Design commitments.**
  - Threshold at its maximum is just a threshold; it never switches Auto on.
  - Tilt stays a toggle in v1, but its value is stored as a float so a continuous tilt can be added later.

## Implementation tasks

- [ ] Specify the Density-to-width mapping, band-count bounds and frequency-time scaling; prototype in analysis notebooks (DSP lead, Phase 0)
- [ ] Prototype max versus power-sum pooling and pick by listening (DSP lead, Product designer, Phase 0)
- [ ] Implement the preallocated log-band grid with bin weights and a frame-boundary swap (DSP lead, Phase 2)
- [ ] Implement per-band gain computer and smoothers at frame rate, reusing the Tab 06 smoother (DSP lead, Phase 2)
- [ ] Implement bin-gain interpolation and apply on the FFT path; report latency (DSP lead, Plugin and host engineer, Phase 2)
- [ ] Implement the auto-threshold reference envelope with warm-up and floor, shared by both dynamics paths (DSP lead, Phase 2)
- [ ] Implement the knee mapping from bandwidth and range (DSP lead, Phase 2)
- [ ] Implement the Spectral Tilt toggle and tune its slope (DSP lead, Product designer, Phase 3)
- [ ] Add CPU benchmarks at maximum Density, full span and 24 spectral bands (Build and QA engineer, Phase 2)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Density resolution | Log band count and widths at Density 0, 50 and 100 over a 2-octave band at 1 kHz | Widths match spec within ±5 %; count within bounds |
| Frequency-scaled times | Step tones at 100 Hz and 10 kHz inside one spectral band | Time ratio matches spec within ±5 % |
| Selectivity | Two tones an octave apart, one over threshold, at Density 100 | The other tone changes by less than 0.5 dB |
| Grid rebuild | Sweep Density and band frequency during pink noise | No allocation on the audio thread (instrumented); residual below −90 dBFS |
| Auto threshold | Pink noise at −30 then −10 dBFS, Auto on, range −6 dB | Gain reduction settles relative to the new level within 0.5 s of the warm-up rule |
| Auto offset cap | Range scaled past 30 dB | Offset ≤ 30 dB |
| Silence | Digital silence, then −120 dBFS noise, Auto on | No gain change; no NaN or Inf |
| Threshold at top | Threshold at maximum with Dyn Auto off | Manual threshold applies (no Auto) |
| Latency | Null test with the spectral path on | Host-reported latency matches the measured value to the sample |
| CPU | 24 spectral bands, Density 100, 48 kHz, 64-sample blocks | Within the overall 3 % budget, or a budget set in Phase 0 for spectral mode |

## Open questions

- Is a separate CPU budget needed for all-spectral sessions? Decide in Phase 0 from prototype numbers.
- Should Tilt act on the threshold or on the detector spectrum? Decide by listening tests.
- Max or power-sum pooling? Decided by listening.
- What release time for the auto-threshold reference? Start near 10 s and tune.
