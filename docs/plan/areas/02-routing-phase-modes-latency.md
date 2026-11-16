# 02 Routing, phase modes, latency

Am-Q runs bands serially per channel in an L/R stage and an M/S stage, each with a zero-latency IIR group and an FFT group. The reported latency depends only on processing mode, resolution, sample rate and whether a spectral band is active, and it is always exact.

## Requirements

1. **R1 Wrapper.** The process wrapper contains no EQ maths. It splits blocks, runs the core, and applies bypass and the latency-matched dry path.
2. **R2 Bypass.** Global bypass is a smoothed wet/dry crossfade. The dry signal is delayed by exactly the reported latency.
3. **R3 Safe silence.** When the processor is not prepared, or the sample rate is unsupported, the output is silence. The plugin never outputs stale buffers.
4. **R4 Band order.** On each channel, bands run in index order. Each band joins either the IIR group or the FFT group of its placement stage.
5. **R5 Placement rules.**
   - Stereo bands run on every channel.
   - Left and Mid bands run on the first channel of a pair.
   - Right and Side bands run on the second.
   - On a mono channel only Stereo bands run, and the GUI shows other placements as inactive.
6. **R6 Smooth enable.** Enabling or disabling a band on a channel crossfades over a short, click-free time. The initial time is 10 ms, tuned by listening.
7. **R7 Surround model.** Channels carry a speaker descriptor (azimuth, side, centre, LFE, height). Bands carry a speaker group (all, non-LFE, height, front, LFE, centre, azimuth pairs, rear centre). Only stereo and mono are active in v1.
8. **R8 FFT-path rule.**
   - In Linear Phase, every band uses the FFT path.
   - In Zero Latency and Natural Phase, a band uses it only if it is a spectral dynamic band with non-zero range on a shape that has gain.
   - Resolution never moves a band between paths.
9. **R9 Latency formula.**
   - Linear Phase: FIR/2 + partition.
   - Natural Phase: the fixed Natural Phase kernel latency (Tab 05).
   - Zero Latency: 0, unless a spectral band is active; then it is the spectral FFT latency.
10. **R10 Latency independent of placement.** Using L/R, M/S or both never changes the latency.
11. **R11 Rate scaling.** Linear Phase FIR lengths are 4096, 8192, 16384, 32768 and 131072 taps for Low to Maximum at 44.1/48 kHz. They double at each power-of-two rate step, so frequency resolution stays roughly constant.
12. **R12 Exact report.** The latency reported to the host equals the measured latency for every mode × resolution × rate × spectral combination.
13. **R13 Latency notification.** A latency change is sent through VST3 restartComponent(kLatencyChanged) or the CLAP latency extension. Output fades out and back in around the change.
14. **R14 Solo and audition.**
    - Soloing a band plays only that band's region.
    - Bell and Notch audition through a band-pass at the band frequency whose width follows Q.
    - Solo Gain (±20 dB, default 0) applies only while a band is soloed.
    - A boosted Bell's solo level includes its boost.
15. **R15 Design modes.**
    - User bands use the magnitude-matched design.
    - All Pass uses bilinear with pre-warp.
    - Internal helper filters (side-chain, audition) use bilinear with a bandwidth pre-warp.
16. **R16 Design tolerance.** Audio and GUI curves use the same design routine, so the drawn curve matches the audio within 0.05 dB.
17. **R17 Analyzer tap.** The processor sends pre-EQ and post-EQ audio to the analyzer through a lock-free FIFO (detail: Tab 19).
18. **R18 No oversampling in v1.** v1 has no oversampling or extra-latency character mode. The latency model leaves room for one later.

## Am-Q design

**Stages.** Each channel pair has an L/R stage, then encode, then an M/S stage, then decode (Tab 03). Each stage has an IIR group and an FFT group. The FFT work of both stages runs inside one shared STFT frame. Spectra are matrixed to M/S in the frequency domain, processed, and matrixed back. One forward and one inverse transform per channel keeps latency independent of placement.

**Natural Phase.** A single 2×2 correction kernel after all IIR stages gives Natural Phase one fixed latency (Tab 03, Tab 05).

**Routing lists.** Lists are rebuilt off the audio thread when enable, placement, shape or mode changes. They are swapped atomically at a block boundary. The per-band enable gain ramps during the swap.

**Latency.** A pure function computes latency from mode, resolution, sample rate and the spectral flag. The function lives in core/dsp and is unit-tested in a table. The wrapper caches the value and notifies the host only when it changes. The bypass dry delay line reads the same value.

**Solo.** Solo replaces the output with a band-pass audition of the band region. We tune the width-from-Q law by ear, with no copied constants. A dedicated decode path on the monitor bus handles M/S-placed bands, so solo is heard in L/R.

**Design commitments.**

- Latency does not depend on stage count.
- Resolution does not decide whether a spectral band takes the FFT path.
- There is no oversampling mode.

## Implementation tasks

- [ ] Write the routing builder: per-channel IIR and FFT lists per stage, with placement and speaker-group tests (DSP lead, Phase 1). Progress: the IIR routing is done. The L/R stage, then the M/S stage, each in band index order; placement masks; mono runs Stereo bands only. Still to do: the FFT groups (Phase 2) and the speaker groups (inactive in v1)
- [ ] Add the per-band, per-channel enable ramp and atomic list swap (DSP lead, Phase 1). Progress: a per-band 10 ms enable crossfade with placement masks; a fully-off band is skipped and bit-transparent. Still to do: the FFT-group list swap, which comes with Phase 2 modes
- [x] Define the speaker descriptor and speaker-group enums; stereo and mono only in v1 (DSP lead, Phase 1). Done on 2026-11-16 (`core/dsp/include/amq/dsp/speakers.hpp`): descriptor (azimuth, LFE, height), the eight groups of R7, mono (front centre) and stereo (±30°) layouts, and channel masks. Tested on both layouts and on surround descriptors (`tests/test_speakers_design_modes.cpp`)
- [x] Implement the combined L/R + M/S STFT processing for FFT bands (DSP lead, Phase 2) Done on 2026-11-16 (`core/engine/spectral.{hpp,cpp}`): one STFT per channel of the processed signal; L/R spectral gains per bin, then an exact M/S encode, M/S gains, decode, then one inverse per channel, so the latency does not depend on placement. Linear Phase runs its bands as one FIR instead (area 05)
- [x] Implement the latency function and its table-driven unit test (DSP lead, Phase 1). `amq/dsp/latency.hpp`: a constexpr `latencySamples(mode, resolution, sampleRate, spectralActive)` with no placement input (R10), and the shared power-of-two `rateFactor()` (the analyzer now uses it too). The test covers all 240 rows: 3 modes × 5 resolutions × 8 rates from 44.1 to 384 kHz × spectral on/off. It also measures impulse delays through the real convolver: the identity Natural Phase kernel at 320 samples for several host block sizes, and a centred delta kernel of full Linear Phase length at every resolution, from 44.1 to 384 kHz (High and below at the two top rate steps). Each matches the function to the sample. The Linear Phase partition is 64 × the rate factor (PROVISIONAL; area 05 R4 scales it with the FIR). `Engine::latencySamples()` reports the latency of what actually runs, and the plugin passes it to the host at activate. That stays 0 until the Phase 2 modes land
- [x] Wire latency reporting and change notification for VST3 and CLAP, with fades (Plugin and host engineer, Phase 2). Done on 2026-11-16. The engine latches the processing mode in `prepare()` and reports its latency (Natural Phase 320, Zero Latency 0; Linear Phase runs as Zero Latency until Phase 2). When `global.mode` asks for another mode the output fades out over 10 ms (PROVISIONAL); once silent, the plugin calls `request_restart` once (from flush or state load, where no audio runs, at once). The next activation reports the latency, calls `latency.changed()`, and fades back in. clap-wrapper turns the request into `restartComponent(kIoChanged | kLatencyChanged)`. Undoing the change before the restart fades back in; a host that never restarts gets the old mode back after 1 s of silence (PROVISIONAL). Tested in the engine (fade shapes within 1e-6) and in the CLAP and VST3 host tests (restart, latency 320, impulse 320 samples late)
- [x] Implement the latency-matched dry path shared by bypass and solo (Plugin and host engineer, Phase 2). Done on 2026-11-16: bypass crossfades to the dry path, and the solo audition filters it, so both line up with the reported latency (tested in Natural Phase: the audition is the Zero Latency one 320 samples later, within 1e-7)
- [ ] Implement solo audition filters with Solo Gain, and tune the width law by listening (DSP lead and Product designer, Phase 2). Progress: implemented on 2026-11-16 (`core/dsp/include/amq/dsp/audition.hpp`, `core/engine`). Bell, Notch, Band Pass and All Pass audition through a band-pass at the band frequency; Low Shelf and Low Cut through a low-pass, High Shelf and High Cut through a high-pass (Butterworth, -3 dB at the band frequency); Tilt Shelf and Flat Tilt play unfiltered. The band-pass follows R15: bilinear with the centre pre-warped (0 dB peak exactly at the band frequency) and the bandwidth pre-warped (the -3 dB edges keep the analog octave width of the audition Q, within 1e-9). Width law: audition Q = band Q, clamped to 0.3 to 8 (all PROVISIONAL). The level is Solo Gain plus the boost of a boosted Bell or shelf, after Gain Scale (R14). The lowest soloed band in use is heard (PROVISIONAL: one at a time). Solo crossfades over 20 ms (PROVISIONAL); moving solo to another band fades out, switches, and fades in. The audition skips the Level / Pan / Invert node and decodes M/S placements itself (area 04 R7): Mid on both sides, Side as +S / -S; Left and Right play on their own side. Bypass wins over solo. Tested: the acceptance row below (Bell and Notch at 120 Hz, 1 kHz and 9 kHz, Q 0.5 to 16: peak within 1 %), Solo Gain bit-transparent while nothing is soloed, the boost rule, the decode, the crossfades and Natural Phase alignment. Still to do: tune the width law and the fade time by listening
- [x] Add the three design-mode selections to the filter design API (DSP lead, Phase 1). Done on 2026-11-16 (`core/dsp/include/amq/dsp/helper_filters.hpp`): `DesignMode` (MagnitudeMatched, BilinearCentre, BilinearBandwidth) and `designModeFor(shape, role)`: user bands matched, All Pass bilinear at the centre, helper filters bilinear with the centre and bandwidth pre-warped. The helper band-pass, low-pass and high-pass live there; the solo audition uses them and the side-chain filters will. Tested: routing for every shape and role, -3 dB exactly at the cutoff, 0 dB band-pass peak at the centre
- [x] Add a safe-silence state for unprepared or invalid configurations (Plugin and host engineer, Phase 1)
- [ ] Build an automated latency measurement harness that runs in several hosts (Build and QA engineer, Phase 2)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Latency exact | Impulse through every mode × resolution × 44.1–384 kHz × spectral on/off | Measured equals reported, to the sample |
| Latency independent of placement | Toggle L/R-only, M/S-only and mixed bands in each mode | Reported latency unchanged |
| Latency change notification | Change mode in Reaper, Cubase, Bitwig and Live | Host re-aligns; no crash; fade heard, not a click |
| Bypass null | Bypass on with latency active | Output vs delayed input ≤ −140 dBFS |
| Placement routing | L-only, R-only, M-only and S-only band boosts on test signals | Only the matching channel or component changes, measured to 0.05 dB |
| Mono layout | Load on a mono track with L/R/M/S bands | Only Stereo bands affect audio |
| Enable crossfade | Toggle a +12 dB band during a sine | No discontinuity; residual < −90 dBFS vs ideal fade |
| Solo audition | Solo a Bell and a Notch at several Q values | Band-pass centred within 1 % of the band frequency; Solo Gain applied only while soloed |
| Design-mode routing | Inspect the All Pass and helper-filter designs | All Pass phase within 2°; helper bandwidth error ≤ 1 % |
| Curve vs audio | Compare the GUI curve with measured audio for 1000 random bands | ≤ 0.05 dB difference |

## Open questions

- Should spectral latency in Zero Latency follow the Resolution setting, or use one fixed spectral size?
- With a spectral band active in Natural Phase, R8 puts the band on the FFT path, whose latency (2112 samples or more) is far above the fixed 320. The latency function currently reports the larger value (PROVISIONAL), so the IIR and kernel path would be delayed to line up. Confirm this, or amend R9, when area 07 settles the spectral latency.
- Do we keep Solo Gain as a separate global parameter, or fold audition level into Output Level? For now it is separate, and the audition skips Output Level (and Pan and Invert).
- Should the latency change fade length be fixed or host-dependent? The engine uses a fixed 10 ms (PROVISIONAL), and waits up to 1 s of silence for the host's restart before playing the old mode again (PROVISIONAL).
