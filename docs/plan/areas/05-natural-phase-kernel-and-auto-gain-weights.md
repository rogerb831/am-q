# 05 Natural Phase kernel and Auto Gain weights

Am-Q's Natural Phase kernel is a 512-tap windowed FIR built from a half-spectrum correction and run in a small uniformly partitioned convolver, with fixed latency = 256 + partition. Auto Gain uses our own band-limited loudness weighting, validated against ITU-R BS.1770 rather than copied.

## Requirements

1. **R1 Natural Phase kernel.** The kernel is 512 taps at every sample rate. It is built from a 257-bin half-spectrum correction, inverse-transformed, centred and windowed.
2. **R2 Natural Phase latency.** Latency is 256 + partition samples, fixed for all settings. With our default 64-sample partition this is 320 samples. Reported exactly.
3. **R3 Natural Phase convolver.** A uniformly partitioned convolver runs the kernel, with no allocation on the audio thread.
4. **R4 Linear Phase kernel.** Kernel length is the resolution length × the power-of-two rate factor. The partition scales by the same factor. Latency is FIR/2 + partition (Tab 02).
5. **R5 Specs per mode.** Natural Phase specs exist only in Natural Phase. Linear specs exist in Linear Phase or while a spectral band is active.
6. **R6 No pre-warp.** Kernel bins are the FFT bin centres, ω\_k = 2πk/N, with targets evaluated at true frequencies.
7. **R7 Window.** The window is chosen by measurement. The candidates are Hann, Blackman–Harris and Kaiser. It must meet the Natural Phase accuracy targets in Tab 03.
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
- [ ] Evaluate window candidates against the Natural Phase accuracy targets (DSP lead, Phase 0). Progress: `amq_engine_tests -tc="Natural Phase window evaluation*" --no-skip` builds the plain ratio correction (no regularisation yet) for 16 bands at 44.1, 48 and 96 kHz. Every window meets 0.05 dB and 2 degrees to 0.95 Nyquist for bells, shelves, tilt and cuts up to 96 dB/oct (Hann worst 0.011 dB, 0.019 degrees; the IIR alone is off by up to 25 degrees, 179 for steep cuts). The exception is a notch (15 kHz, Q4): 0.43 dB at 48 kHz and 1.27 dB, 2.2 degrees at 96 kHz with Hann (Blackman-Harris 0.90 dB, 1.5 degrees), from the spike in the ratio next to the zero. Still to do: re-run once the area 03 regularisation exists, then fix the window
- [x] Implement the uniformly partitioned convolver with a 64-sample partition (DSP lead, Phase 1). Uniformly partitioned overlap-save with a frequency-domain delay line (Gardner 1995, Wefers 2015). Latency is exactly one partition: a centred 512-tap kernel puts an impulse at sample 320 (R2). No allocation in process (R3). A 512-tap kernel costs about 0.06 % of one core per channel at 48 kHz. The crossfaded kernel swap (R8) comes with the kernel builder
- [ ] Implement per-mode spec creation and rate-scaled Linear Phase specs (DSP lead, Phase 2)
- [ ] Design the Auto Gain weighting, then build the programme corpus and BS.1770 validation script (DSP lead and Build and QA engineer, Phase 2)
- [ ] Build the weight table at start-up and share it with the GUI (DSP lead, Phase 2)
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

- Should the Natural Phase kernel length scale with sample rate, to keep correction resolution near Nyquist at high rates? Evidence: the notch error grows from 0.43 dB at 48 kHz to 1.27 dB at 96 kHz, because the bins are 187.5 Hz apart there.
- The all-flat null through the float convolver is −127.6 dBFS when the kernel comes from `build()`, short of the −140 dBFS baseline. Partly settled (2026-11-16): with every band flat the engine loads `NaturalPhaseKernelBuilder::identity()`, and an impulse through the engine then comes out exactly (deviation 0) 320 samples late. Programme material through the float convolver has not been measured against −140 dBFS yet.
- Is a 64-sample partition the right CPU/latency trade-off, or should we use 32?
- Should the Auto Gain weighting treat the lowest band differently for cut filters, to avoid big boosts on high-pass curves?
