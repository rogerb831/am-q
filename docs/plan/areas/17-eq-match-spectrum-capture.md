# 17 EQ Match spectrum capture

Am-Q captures the input and reference spectra in the shared analyzer engine on a worker thread, from a lock-free copy of the audio. Captures are kept separate from the live display, merged across channels by power, and only become matchable once enough non-silent audio has been heard.

## Requirements

1. R1. EQ Match keeps two captures: input and reference. Each is fed from a lock-free copy of the audio and processed only on the analysis worker.
2. R2. Reference sources in v1 are sidechain and audio file. Another Am-Q instance is a v2 source. The input source is the plugin input before the EQ.
3. R3. Captures use an STFT with 75 % overlap and a Kaiser or Blackman-Harris window. FFT size follows tab 19 (match captures use the engine's highest resolution).
4. R4. Only full frames enter a capture. Frames padded with zeros during start-up are excluded.
5. R5. Multichannel audio is merged by mean power per bin across channels, not by per-bin maximum.
6. R6. If the worker falls behind, the audio thread never blocks. Oldest audio is dropped, the drop is counted and the capture stays valid.
7. R7. Start, Stop and Reset controls exist for each capture. Reset clears the accumulators and the counted-frame total.
8. R8. A capture is “ready” once it holds at least a minimum duration of counted (non-silent) audio. Planning value: 2 s. An audio-file reference is ready once its analysis completes.
9. R9. An audio-file reference is analysed on the worker with the same window, FFT size, gate and averaging as live captures. Its progress is shown.
10. R10. Captures carry the sample rate and FFT size they used. A sample-rate change resets live captures and shows a notice.
11. R11. Tilt is a display-only setting. EQ Match captures and the difference curve are computed from untilted spectra.
12. R12. The fit range is 20 Hz to 20 kHz, limited by the usable band below Nyquist of the capture's sample rate.
13. R13. A capture never stores audio samples, only accumulated spectra and counters.

## Am-Q design

**Audio tap.** The audio thread writes each block into a single-producer, single-consumer ring per tap (input, sidechain). This is the same tap the live analyzer uses (tab 19). The worker reads, frames and windows the audio. On overflow, the worker skips ahead and increments a dropped-samples counter. The capture is kept and the gap is recorded.

**Framing and window.** Hop is N/4 (75 % overlap). The window is periodic Blackman-Harris (4-term) or Kaiser, as the DSP lead decides in prototypes. Spectra are scaled for power using the window's energy (sum of squared window values), as in Welch's method. Only frames whose ring segment is fully filled are used.

**Channel merge.** Per bin, power is averaged across channels. Max-merging would bias a stereo mix with uncorrelated sides upwards and makes the match depend on panning. Power averaging gives a level that is consistent for mono and stereo material.

**Sources.** Sidechain uses the host sidechain bus. Audio files are decoded on the worker with a public decoder, resampled to the session rate and analysed offline. The instance-to-instance source (v2) shares accumulated spectra through Am-Q's instance registry, never audio. Using the input itself as a reference, and saved spectrum files, are open questions.

**Storage and hand-off.** Each capture owns a power accumulator on the engine's log grid (tab 18). The difference curve (tab 20) is built from the two captures on the worker. It is published to the GUI through a lock-free snapshot. The GUI only reads snapshots; it never writes match data.

## Implementation tasks

- [x] Specify capture sources, readiness rule, reset rules and sample-rate handling (DSP lead, Phase 0). Done on 2026-11-16 as the header of `core/analysis/include/amq/analysis/match_capture.hpp` and `core/engine/include/amq/engine/eq_match.hpp`: input from the plugin input, reference from the side chain or a WAV file; ready at 2 s of counted audio, or when a file's analysis completes; Reset clears accumulators and counters; a new rate (or FFT size) at activation resets both captures
- [x] Implement the lock-free input and sidechain taps with overflow counting (Plugin and host engineer, Phase 1). `amq/analysis/audio_tap.hpp`: an SPSC ring of 0.5 s (rounded up to a power of two). The audio thread always writes and never waits. The reader skips ahead when a ring behind and counts dropped frames, and a seqlock-style claim check drops any frames overwritten during a copy. Every read reports a gap, so the framer restarts and uses full frames only (R4). Tested: no allocation on write, exact drop counts, and 1.6 M frames read under a racing producer with no torn frame. The engine taps pre-EQ input, post-EQ output and the side chain
- [x] Implement the capture framer: full frames only, 75 % overlap, power-scaled window, power merge across channels (DSP lead, Phase 3). Done on 2026-11-16: the captures take the analyzer's full frames at its highest size (75 % overlap, periodic Blackman-Harris, channels merged by mean power); a file is framed the same way (`MatchCapture::analyseSignal`). Tested: a tone lands on its grid point; a hard-left tone reads 3 dB below a centred one (mean-power merge)
- [x] Implement the audio-file reference: decode, resample, analyse on the worker with progress (Plugin and host engineer, Phase 3). Done on 2026-11-16: Am-Q's own WAV reader (PCM 16/24/32-bit, float 32/64, extensible; files over 20 minutes refused), a windowed-sinc resampler to the session rate (a 3 kHz tone stays within 0.05 dB from 44.1 to 48 kHz), and analysis on a file thread with progress and cancel; the editor opens files with the native dialog (Windows; macOS with the macOS build). Other formats are later work
- [x] Implement Start, Stop, Reset and the ready state per capture (Plugin and host engineer, Phase 3). Done on 2026-11-16: Record/Stop and Reset per capture through `EqMatchStage`, ready state and counted seconds in the snapshot; tested in the engine
- [x] Build the capture controls, source selector, readiness indicator and dropped-audio warning (GUI engineer, Phase 3). Done on 2026-11-16 in the EQ Match panel: Record/Stop and Reset per capture, Side Chain / File source with Load File, readiness and seconds, file progress and errors, and a dropped-audio warning from the analyzer's skipped-frame counts while a capture records (the capture stays valid, R6). Wording PROVISIONAL
- [ ] Design the source selector and capture workflow (Product designer, Phase 3)
- [x] Build capture tests: sine, noise, stereo panning, overflow and sample-rate change (Build and QA engineer, Phase 3). Done on 2026-11-16 (`tests/analysis/test_match.cpp`, `tests/engine/test_eq_match_engine.cpp`): tone, noise, mean-power panning, gating, windows, an hour without drift, reset, the engine path, and overflow (two seconds with the worker stalled: frames dropped and counted, the capture keeps counting and stays valid); a rate change resets at activation (prepare)
- [ ] Review the audio-file decoder licences (Counsel, Phase 3)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Audio thread safety | Run captures under a real-time checker (no locks, allocation or blocking on the audio thread). | Zero violations over 10 minutes |
| Overflow | Stall the worker for 2 s while audio runs. | Audio unaffected; drop counter non-zero; capture still valid after recovery |
| Start-up frames | Start capture on a steady sine and inspect the first frames. | No partially filled frame contributes |
| Channel merge | Feed the same pink noise hard-left, centre and hard-right. | Captured spectra agree within 0.1 dB at every grid point |
| Readiness | Feed 1 s of audio, then 1 s of silence, then 1 s of audio. | Ready only after the second second of audio |
| File reference | Analyse a 3-minute file; capture the same file live through the sidechain. | Spectra agree within 0.5 dB RMS over 20 Hz to 20 kHz |
| Sample-rate change | Change host rate during a capture. | Live captures reset; notice shown; no crash |
| Tilt independence | Capture with tilt at 0 and at 4.5 dB/oct. | Difference curve identical |
| GUI isolation | Resize and close the editor while capturing. | Capture continues and results do not change |

## Open questions

- Should Am-Q offer a “snapshot input as reference” source?
- Should a captured reference be savable and loadable as a spectrum file? If so, what versioned format?
- Should the input tap be before the EQ (planned) or selectable, so users can match with existing bands active?
- What is the minimum capture duration for the release, and should it scale with the smoothing setting?
