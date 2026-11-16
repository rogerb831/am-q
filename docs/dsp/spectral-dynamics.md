# Spectral dynamics spec

Status: PROVISIONAL constants, 2026-11-16. Code: `core/dsp/include/amq/dsp/spectral_grid.hpp`, `core/engine/spectral.{hpp,cpp}`. Plan: area 07 (spectral dynamics and auto threshold), area 02 R8–R12 (the FFT path and its latency), area 08 R3 (Density). The timing core is shared with area 06 ([dynamics-timing.md](dynamics-timing.md)).

## Which bands, which path

- **Spectral band:** a gain band (Bell, shelves, tilts) with Dynamic and Spectral on and a non-zero range, in Zero Latency or Natural Phase.
- **Static shape:** stays an IIR band. Only the dynamics run on the FFT path.
- **Latency:** while any spectral band is in use, the reported latency is the FFT-path latency, FIR/2 + partition at the current resolution (area 02 R9, `dsp/latency.hpp`).
- **Switching:** turning the spectral path on or off fades out and asks the host to restart, like a mode change (area 02 R13).
  - Until the restart, a spectral band runs the IIR dynamic blend (area 06).
  - In Linear Phase it always does (PROVISIONAL).

## STFT

- **Frame N:** 1024, 2048, 4096, 8192 or 16384 for Low to Maximum, times the rate factor. Hop N/4.
- **Windows:** sqrt-Hann (periodic) for analysis and synthesis. Their product, a periodic Hann, sums to 2 at this overlap, so unity gains reconstruct the input exactly, N samples late.
- **Padding:** a delay pads the processed path so the total equals the reported latency.
  - Zero Latency: pad = FIR/2 + partition − N.
  - Natural Phase: the same, less 320.
- **Measured:** an impulse comes out at the reported latency (within 1e-4) at Low, Medium and Very High, in both modes.

## Side chain and levels

- **Source:** the plugin input (or the external side-chain bus with SC External), framed the same way.
- **Placement:** encoded per band, as in area 06:
  - Stereo: linked, the louder of L and R per bin;
  - Left, Right: that channel;
  - Mid, Side: (L ± R)/2.
- **Level of an analysis band:** the largest weighted bin magnitude in it (max pooling; power-sum is the area 07 alternative). It is scaled to the peak amplitude of a sine, in dBFS, so manual thresholds mean the same as for non-spectral bands.

## Analysis grid (Density)

- **Width at 1 kHz:** Density 0 gives 1/4 octave; Density 100 gives 1/16 octave; between them the width is log-interpolated.
- **Frequency dependence:** a band at f is `(f / 1 kHz)^-0.1` times that width, so bands are finer at high frequencies (area 07 R2).
- **Minimum width:** two FFT bins.
- **Span** (R1): centres run across the band's span.
  - Free mode: SC Low to SC High.
  - Bell, Notch, Band Pass, All Pass: f0 · 2^(±b), with b the octave width of the Q.
  - Low shelf / low cut: 20 Hz to 2 f0.
  - High shelf / high cut: f0/2 to 20 kHz.
  - Tilts: 20 Hz to 20 kHz.
  - The span is limited to 20 Hz – 20 kHz and below 0.45 fs.
- **Count:** 3 to 96 bands (R3); a span that would need more is widened uniformly.
- **Weights:** triangular in log frequency between neighbouring centres, summing to 1 at every bin. Interpolating a constant gain back to bins is therefore exact.
- **Rebuilds:** Density or a span change rebuilds the grid at a frame boundary, without allocating (area 08 R3). Each new analysis band starts from the state of the nearest old one, so gains continue (R5).

## Per analysis band, at frame rate

- **Threshold:** the manual Threshold, or Auto: the area 07 reference envelope per analysis band, offset by |range| and capped at 30 dB.
- **Spectral Tilt** (R6): moves each analysis band's threshold by −3 dB per octave from the band centre.
- **Gain computer:** the area 06 soft knee and range cap. The knee width is set by the analysis band's width.
- **Smoothing:** the area 06 dual release, running at the frame rate (fs / hop).
  - Attack: `attackSeconds(knob, centre)`.
  - Release: `releaseSeconds(knob) × (1 kHz / centre)^0.25`, limited to ×0.5 … ×2, so low bands are slower (R4).
  - Dyn Auto uses the knob-50 timing.

## Apply

Per bin, the gain reduction is interpolated with the same weights and divided by |range|. That fraction scales the band's relative response `|H(gain + range) / H(gain)|` in dB, from the analog prototypes. The dynamics therefore keep the band's shape: at the band centre a fully reduced band changes by exactly the range.

- Gains of several spectral bands add in dB per component.
- L and R components are applied first, then M and S through an exact encode/decode.

## Measured (tests/engine/test_spectral.cpp)

| Check | Result |
| --- | --- |
| Reconstruction | Exact at the reported latency |
| Range at the band centre | −12 dB within 0.6 dB (24 dB over threshold) |
| Selectivity (two tones an octave apart, Density 100) | The tone under threshold changes by less than 0.5 dB |
| Mid band on a pure Side signal | Unchanged |
| Silence and −120 dBFS noise with Auto | No gain change, finite output |
| Density grid widths | Within 5 % of the mapping |
| Density and frequency sweeps during noise | Rebuild many times; output stays finite and bounded |

**CPU** (`tests/engine/test_cpu.cpp`, Medium, 48 kHz):
- One spectral band at Density 100 adds about 1.4 % of a core.
- All 24 bands spectral at Density 100 take 11 %. Whether all-spectral sessions get their own CPU budget is an area 07 open question.
