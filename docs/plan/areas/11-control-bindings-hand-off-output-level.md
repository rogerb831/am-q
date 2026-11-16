# 11 Control bindings, hand-off, Output Level

Am-Q binds every control by parameter id and keeps styling separate. Analyzer, meter and gain-reduction data reach the editor through a lock-free snapshot queue that merges peaks between pulls. Output Level is linear in dB from −36 to +36 dB, with a smooth fade to silence below −36.

## Requirements

1. R1. Each control gets a theme style and a parameter id. Menus read range, labels and order from the parameter table.
2. R2. The band field set covers every reference band field, plus 8 spares per band, for 24 bands. Fields: Used, Enabled, Frequency, Gain, Q, Shape, Slope, Placement, Speakers (reserved), Dyn Range, Dyn Enabled, Dyn Auto, Threshold, Attack, Release, SC External, SC Mode, SC Low, SC High, SC Listen, Spectral, Spectral Density, Spectral Tilt, Solo.
3. R3. The audio thread publishes analyzer frames without locks. The editor pulls them at about 30 Hz. Each frame carries an increasing stamp.
4. R4. If the editor misses frames, spectrum and meter peaks are max-merged into the next snapshot. No transient is lost from the display.
5. R5. Each frame carries every band's current dynamic gain change in dB. Each band's Dyn Range is available to the GUI for scaling (Tab 14).
6. R6. Each meter source publishes peak, RMS and peak-hold per channel.
7. R7. Output Level runs from silence (−∞) through −36 dB to +36 dB, with a default of 0 dB. It is linear in dB from −36 to +36. Below −36 it fades continuously and monotonically to silence.
8. R8. dB-to-linear and linear-to-dB helpers clamp to a fixed floor for zero or denormal input. The inverse of the knob mapping is exact within float precision.
9. R9. Gain Scale, Output Pan and any solo-audition gain have documented ranges and defaults in the table (Tab 03).

## Am-Q design

- **Controls.** The ui/ attachment class (Tab 10) binds a control to its id. Styles are theme tokens. Menus are built from the table's labels and its menu-order map.
- **Field set.** The decided table already covers R2. Spare fields leave room for later flags.
- **Snapshot queue.**
  - analysis/ keeps one single-producer, single-consumer triple buffer per data type.
  - The audio side accumulates peaks since the last publish.
  - The editor reads the newest frame.
- **Gain-change feed.** Each band publishes its smoothed signed gain change, plus the peak since the last publish. Spectral bands publish one summary value per band (Tab 14).
- **Output Level mapping.**
  - A bottom fade segment uses our own smooth power curve that meets −36 dB exactly.
  - Above it, the mapping is linear in dB.
  - The product designer sets the segment split.

## Implementation tasks

- [ ] Implement menus that read labels and order from the table (GUI engineer, Phase 1)
- [ ] Confirm the band field list and spare allocation in the table spec (Plugin and host engineer, Phase 0)
- [x] Implement the lock-free snapshot queue with peak accumulation and stamps (DSP lead, Phase 1). `core/analysis`: one SPSC triple buffer per data type, stamps from 1, peaks of unread frames max-merged into the next one. Publishing at 60 Hz (PROVISIONAL); RMS 300 ms and hold 1.5 s are PROVISIONAL
- [x] Publish per-band gain change and meter data from the processor (DSP lead, Phase 2). The engine meters input, output and side chain (peak, RMS, hold per channel) and, since 2026-11-16, publishes each band's signed dynamic gain change (negative for a cut) in the gain-change snapshot
- [x] Implement editor pulling at about 30 Hz with stamp checks (GUI engineer, Phase 2). Done early: `ui::SnapshotPoller` on a 33 ms Visage timer; frames with an old stamp are skipped, and a redraw is reported only for a visible change (0.1 dB)
- [ ] Specify the Output Level segments and fade curve (Product designer, DSP lead, Phase 0)
- [x] Implement the Output Level forward and inverse mappings and the dB helpers (Plugin and host engineer, Phase 1). The fade start (0.1) and the fade law are PROVISIONAL, for the Product designer
- [ ] Document ranges for Gain Scale, Output Pan and solo gain (Product designer, Phase 0)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Menu labels | Compare every indexed menu with the table | Exact match in order and text |
| Lock-freedom | Instrument the audio-thread publish path | No locks, allocations or system calls |
| Peak merge | Inject a one-sample click while editor pulls are paused for 500 ms | Next snapshot shows the peak within 0.1 dB |
| Stamps | Log stamps over a 10-minute run | Strictly increasing; no torn frames |
| Gain-change feed | Band with known gain reduction on a steady tone | Published value within 0.1 dB of measured |
| Level mapping | Check centre, fade boundary, 0 and 1; round-trip 10,000 points | 0 dB, −36 dB, −∞ and +36 dB respectively; monotonic; round-trip error < 1e-6 |
| Fade continuity | Slow sweep through the fade boundary on a sine | No step > 0.1 dB; no click above −90 dBFS |
| dB floor | Feed zero, denormal and negative input | Returns the floor; no NaN or Inf |

## Open questions

- Do we need a solo-audition gain parameter?
- Should 0 dB sit exactly at the knob centre, given the fade segment?
- Is a per-analysis-band gain display worth its cost (Tab 14)?
