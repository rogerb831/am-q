# 05 Natural Phase kernel and Auto Gain weights

Am-Q's Natural Phase kernel is a 512-tap windowed FIR built from a half-spectrum correction and run in a small uniformly partitioned convolver, with fixed latency = 256 + partition. Auto Gain uses our own band-limited loudness weighting, validated against ITU-R BS.1770 rather than copied.

## Requirements

1. **R1 Natural Phase kernel.** The kernel is 512 taps at every sample rate. It is built from a 257-bin half-spectrum correction, inverse-transformed, centred and windowed.
2. **R2 Natural Phase latency.** Latency is 256 + partition samples, fixed for all settings. With our default 64-sample partition this is 320 samples. Reported exactly.
3. **R3 Natural Phase convolver.** A uniformly partitioned convolver runs the kernel, with no allocation on the audio thread.
4. **R4 Linear Phase kernel.** Kernel length is the resolution length × the power-of-two rate factor. The partition scales by the same factor. Latency is FIR/2 + partition (Tab 02).
5. **R5 Specs per mode.** Natural Phase specs exist only in Natural Phase. Linear specs exist in Linear Phase or while a spectral band is active.
6. **R6 No pre-warp.** Kernel bins are the FFT bin centres, ω\_k = 2πk/N, with targets evaluated at true frequencies.
7. **R7 Window.** The window is chosen by measurement. The candidates are no window (rectangular, added by decision D3), Hann, Blackman–Harris and Kaiser. It must meet the Natural Phase accuracy targets in Tab 03.
8. **R8 Off-thread build.** Kernels build off the audio thread and swap at block boundaries with a crossfade.
9. **R9 Auto Gain formula.** Make-up gain is the inverse of a weighted mean of the combined band response over the shared log grid. It is clamped to ±30 dB.
10. **R10 Auto Gain weighting.** The weighting is our own band-limited curve. It ignores sub-bass and extreme top and is flat through the main band. Weights are normalised to sum to 1.
11. **R11 Auto Gain accuracy.** Across a test set of EQ curves and programme material, integrated loudness after Auto Gain changes by ≤ ±1 LU (BS.1770).
12. **R12 Multichannel Auto Gain.** Gain combines per channel as a weighted geometric mean with speaker weights (Tab 03 R9).
13. **R13 Initialisation.** The weight table is built at plugin start-up, not on the audio thread.

## Am-Q design

**Kernel construction.**

1. Build the 257-bin correction (Tab 03).
2. Inverse FFT to 512 real taps.
3. Circularly shift so the peak sits at tap 256.
4. Apply the chosen window.

Latency comes from the analytic delay of 256 taps plus the convolver partition, not from a hard-coded number.

**Convolution.** Uniformly partitioned frequency-domain convolution (Gardner 1995; Wefers 2015) with a 64-sample partition. If CPU allows, a direct-form head could remove the partition delay. That is a later optimisation only, and it would change latency.

**Auto Gain weighting.** Start from K-weighting combined with a pink-noise programme spectrum. Band-limit it to 20 Hz – 20 kHz and compute the weighted mean in the power domain. Gain = 1/sqrt(Σ w·|H|²). Tune against BS.1770 measurements on a programme corpus.

**Design commitments.**

- A power-domain, loudness-validated weighting.
- Window chosen by measurement.
- Latency derived analytically and tested.

## Implementation tasks

- [x] Implement the kernel builder: half-spectrum, inverse FFT, shift, window (DSP lead, Phase 1). `amq/dsp/natural_phase_kernel.hpp`: 257 bins at w_k = 2 pi k / 512 (R6), DC and Nyquist made real, the 256-tap delay applied as (-1)^k before the inverse FFT, then a window centred on tap 256 (Rectangular, Hann, Blackman-Harris or Kaiser; Hann is the PROVISIONAL default). Measured latency with the convolver is 320 samples (R2). The crossfaded swap (R8) is in the convolver: `submitKernel()` from the worker, picked up at a partition boundary, 256-sample linear crossfade (PROVISIONAL), no allocation; the rate-limited scheduler is area 04
- [x] Evaluate window candidates against the Natural Phase accuracy targets (DSP lead, Phase 0). Done on 2026-11-16: no window (decision D3). With the area 03 regularisation, over 28 bands at 44.1, 48 and 96 kHz: no window worst 0.0088 dB and 0.072 degrees; Kaiser β=2 0.054 dB; Hann 0.18 dB; Blackman-Harris 0.35 dB, all of them missing only on a +18 dB Q16 bell at 20.5 kHz at 44.1 kHz. Report: `amq_engine_tests -tc="Natural Phase window evaluation*" --no-skip`
- [x] Implement the uniformly partitioned convolver with a 64-sample partition (DSP lead, Phase 1). Uniformly partitioned overlap-save with a frequency-domain delay line (Gardner 1995, Wefers 2015). Latency is exactly one partition: a centred 512-tap kernel puts an impulse at sample 320 (R2). No allocation in process (R3). A 512-tap kernel costs about 0.06 % of one core per channel at 48 kHz. The crossfaded kernel swap (R8) comes with the kernel builder
- [x] Implement per-mode spec creation and rate-scaled Linear Phase specs (DSP lead, Phase 2). Done on 2026-11-16 (`core/engine/linear_phase.{hpp,cpp}`, `core/dsp/nonuniform_convolver.{hpp,cpp}`). Natural Phase objects exist only in Natural Phase and Linear Phase kernels only in Linear Phase (the engine prepares the stage of the latched mode). The Linear Phase kernel multiplies every static band's exact analog magnitude at the FFT bin frequencies, per channel, combined 2x2 for M/S like area 03; zero phase delayed by N/2, a Tukey taper on the outer 12.5 % per side (PROVISIONAL). N is the resolution's FIR length times the rate factor; the head partition is 64 times the rate factor (R4). Long kernels run in a non-uniformly partitioned convolver (partitions 64, 256, 1024, then 4096 for the rest, times the rate factor; exact against direct convolution). Kernel changes go to a standby set of convolvers that is always fed, and the output crossfades to it over 20 ms (PROVISIONAL) once all its segments have switched (R8). Tested: an impulse comes out exactly FIR/2 + partition late at every resolution at 48 kHz, at 44.1 kHz and at 96 and 192 kHz; the impulse response is symmetric; magnitude within 0.05 dB of the analog prototypes from 200 Hz to 20 kHz (three bands, High); a Mid band filters only Mid; a gain change swaps in smoothly; no allocation in process. CPU: 2.3 % of one core at Medium, 48 kHz. Dynamic bands in Linear Phase run their dynamic part as a minimum-phase blend at the relative range on top of the FIR (PROVISIONAL until the FFT-path dynamics)
- [ ] Design the Auto Gain weighting, then build the programme corpus and BS.1770 validation script (DSP lead and Build and QA engineer, Phase 2). Progress (2026-11-16): weighting designed (pink share per log-grid point times the BS.1770 K-weighting power, 20 Hz to 20 kHz, normalised; `dsp/auto_gain.hpp`) and a BS.1770-4 meter written (`dsp/loudness.hpp`: K-weighting for any rate, matching the standard's 48 kHz coefficients to 1e-5; gating; a 1 kHz full-scale tone reads -3.01 LUFS). Validation (`tests/engine/test_auto_gain.cpp`): 50 random EQ curves (1 to 5 bells, shelves and tilts, ±12 dB) on pink noise move integrated loudness by at most 0.97 LU after Auto Gain; on pure tones and white or brown noise by up to 9 LU, reported only (a response-only make-up gain cannot know where a signal's power is). Still to do: the real programme corpus (20 items) and rerunning the ±1 LU check on it
- [x] Build the weight table at start-up and share it with the GUI (DSP lead, Phase 2). Done on 2026-11-16: `dsp::autoGainWeights()` builds the table once; the Auto Gain stage builds it in its constructor (plugin start-up), never on the audio thread; the GUI includes the same header
- [ ] Review the Auto Gain weighting for clean-room independence (Counsel, Phase 2)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Natural Phase latency | Impulse in Natural Phase at all rates and resolutions | Measured equals reported (320 at default partition) |
| Linear Phase latency | Impulse at every resolution and rate | Measured = FIR/2 + partition, equals reported |
| Kernel symmetry baseline | All bands flat in Natural Phase | Output = input delayed, ≤ −140 dBFS null |
| Natural Phase accuracy | Battery of bands up to 0.95·Nyquist | ≤ 0.05 dB and ≤ 2° vs analog |
| No pre-warp | Correction phase at a high bell centre | Matches analog within 2° |
| Auto Gain loudness | 50 EQ curves × 20 programme items | ΔLUFS ≤ ±1 LU |
| Auto Gain clamp | Extreme cuts and boosts | Make-up within ±30 dB |
| Auto Gain flat | All bands flat | Make-up exactly 0 dB |
| Real-time safety | Mode and band changes during playback | No allocation or lock on the audio thread |

## Open questions

- Should the Natural Phase kernel length scale with sample rate, to keep correction resolution near Nyquist at high rates? The original evidence (a notch error growing from 0.43 dB at 48 kHz to 1.27 dB at 96 kHz) was the 0/0 bin, fixed by the area 03 regularisation: at 96 kHz every measured band is now within 0.009 dB. No current evidence for longer kernels; revisit with the high-rate stress cases (192 kHz and up).
- The all-flat null through the float convolver is −127.6 dBFS when the kernel comes from `build()`, short of the −140 dBFS baseline. Partly settled (2026-11-16): with every band flat the engine loads `NaturalPhaseKernelBuilder::identity()`, and an impulse through the engine then comes out exactly (deviation 0) 320 samples late. Programme material through the float convolver has not been measured against −140 dBFS yet.
- Is a 64-sample partition the right CPU/latency trade-off, or should we use 32?
- Should the Auto Gain weighting treat the lowest band differently for cut filters, to avoid big boosts on high-pass curves?
