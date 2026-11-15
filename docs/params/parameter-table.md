# Am-Q parameter table spec

Status: written 2026-11-14 with the first implementation (`core/params`). This is plan area
09's "parameter-table spec" task. It covers the encodings, ranges, defaults, host ids,
versioning and the state chunk. Items marked **PROVISIONAL** are not fixed by the plan; their
owner confirms them before release. Changing one is a one-line edit in
`core/params/src/param_specs.cpp` plus this table.

## Rules

- **One table.** It has 24 band blocks of 23 fields in a fixed order, then 21 globals: 573 parameters. Order: `BandField` and `GlobalParam` in `param_spec.hpp`.
- **String ids** are `bandNN.field` (NN = 01–24), `global.*`, `analyzer.*` and `display.*`. They are frozen at release.
- **Host ids.** The host id is FNV-1a 32 of the string id, masked to 31 bits, because VST3 reserves ids ≥ 2³¹. CLAP uses it as `clap_id`, and clap-wrapper exposes the same number as the VST3 `ParamID`.
  - The ids are frozen by `tests/params/host_ids.golden.txt`, which an independent implementation generated. After release, lines may be appended but never edited.
  - The table refuses to build if two ids collide.
- **Host ids versus fixed indices.** The plan README says "fixed host indices"; area 21 says "stable hashes". Hashes are fixed per id and don't depend on table layout, so they satisfy both.
  - Host list order (the CLAP param index) is table order. CLAP and VST3 key automation and state by id, not by position.
  - So the README's 8 spare fields per band and 32 spare globals need no reserved slots: a new field gets a new string id and a new hash, and no existing id moves.
- **Reserved ids** are never exposed and never reused: `global.character`, `global.midiReceive`, and `bandNN.speakers` (area 21 R3).
- **Plain values** are stored in perceptual units, and host automation is linear in them (area 09 R3).
  - Output Level is the only exception. It is stored in dB, but its host value is a position from 0 to 1 on the area 11 curve (see below). Its CLAP min/max are 0 and 1.
- **Types** are Continuous, Toggle (0/1) and Choice (index). Toggles and choices are stepped, and choices are flagged as enums for CLAP. Readable labels come from `value_to_text` (area 21 R15).
- **The setter** (`ParameterTable::set`):
  - **Clamp.** Values are clamped to range, toggles and choices are rounded to a whole index, and NaN becomes the default.
  - **Ignore tiny changes.** A continuous change below 1e-9 of the range is ignored; toggles and choices compare exactly.
  - **Store.** The value goes into an atomic slot, the source is recorded, and the parameter is marked dirty. It is lock-free and safe on any thread.
  - **Notify on the main thread.** Listeners are called only from `dispatchNotifications()` on the main thread: one coalesced call per changed parameter, with the latest value and source.
- **Sources** are Gui, Host, Preset, EqMatch and Undo. Undo records only Gui and EqMatch (area 09).

## Band fields (per band, ids `bandNN.<id>`)

| # | id | Type | Range (plain) | Default | Unit / display |
| --- | --- | --- | --- | --- | --- |
| 0 | used | Toggle | 0–1 | Off | |
| 1 | enabled | Toggle | 0–1 | On | |
| 2 | freq | Continuous | log2(10)–log2(30000) | log2(1000) | Hz / kHz |
| 3 | gain | Continuous | −30 – +30 | 0 | dB |
| 4 | q | Continuous | n 0–1 | 0.5 (Q 1) | Q = 0.025·1600ⁿ |
| 5 | shape | Choice | 10 shapes, stored as `dsp::Shape` (0 Bell … 9 All Pass) | Bell | shape name |
| 6 | slope | Choice | 10 slopes (6 … 96 dB/oct, Brickwall) | 12 dB/oct **PROVISIONAL** (Product designer) | slope name |
| 7 | placement | Choice | Stereo, Left, Right, Mid, Side | Stereo | |
| 8 | dynRange | Continuous | −30 – +30 | 0 | dB; sign selects cut or boost |
| 9 | dynEnabled | Toggle | | Off | |
| 10 | dynAuto | Toggle | | On | |
| 11 | threshold | Continuous | −90 – 0 (area 01 R8) | −20 **PROVISIONAL** (DSP lead) | dBFS |
| 12 | attack | Continuous | 0–100 | 50 | knob |
| 13 | release | Continuous | 0–100 | 50 | knob |
| 14 | scExternal | Toggle | | Off | |
| 15 | scMode | Choice | Band, Free | Band | |
| 16 | scLow | Continuous | log2(10)–log2(20000) (area 01 R8) | 10 Hz (fully open, area 21 R8) | Hz |
| 17 | scHigh | Continuous | log2(10)–log2(20000) | 20 kHz (fully open) | Hz |
| 18 | scListen | Toggle | | Off | |
| 19 | spectral | Toggle | | Off | |
| 20 | spectralDensity | Continuous | 0–100 | 50 | knob |
| 21 | spectralTilt | Toggle | | Off (area 07 R6: a per-band toggle) | |
| 22 | solo | Toggle | | Off | |

## Globals

| id | Type | Range | Default | Notes |
| --- | --- | --- | --- | --- |
| global.mode | Choice | Zero Latency, Natural Phase, Linear Phase | Zero Latency | |
| global.resolution | Choice | Low, Medium, High, Very High, Maximum | Medium **PROVISIONAL** (DSP lead) | five steps (area 02 R11) |
| global.gainScale | Continuous | 0–200 % | 100 % | area 03 R6 |
| global.outputLevel | Continuous | silence floor (−200 dB = −∞) – +36 dB | 0 dB | host value is the curve position; fade law **PROVISIONAL** (Product designer) |
| global.outputPan | Continuous | −100 (L or M) – +100 (R or S) **PROVISIONAL** (Product designer) | 0 (C) | |
| global.panMode | Choice | L/R, M/S | L/R | automatable for now (area 04 open question) |
| global.invert | Toggle | | Off | |
| global.bypass | Toggle | | Off | CLAP bypass flag; VST3 bypass via clap-wrapper |
| global.autoGain | Toggle | | Off | |
| global.soloGain | Continuous | −20 – +20 dB | 0 dB | area 02 R14 |
| analyzer.showPre / showPost / showExternal | Toggle | | On / On / Off | not automatable |
| analyzer.range | Choice | 60, 90, 120 dB **PROVISIONAL** | 90 dB | not automatable |
| analyzer.resolution | Choice | Low, Medium, High, Maximum (FFT 2048–16384) **PROVISIONAL** labels | Medium | not automatable |
| analyzer.speed | Choice | Very Slow … Very Fast **PROVISIONAL** | Medium | not automatable |
| analyzer.tilt | Continuous | 0–6 dB/oct **PROVISIONAL** range | 4.5 dB/oct | not automatable |
| analyzer.freeze / collisions / grab | Toggle | | Off / Off / On | not automatable |
| display.range | Choice | 3, 6, 12, 30 dB **PROVISIONAL** | 12 dB | not automatable |

The analyzer and display settings are table parameters because area 21 R2 lists them as globals, and area 12 R15 wants them saved with the state. They are not automatable. Whether they belong in the host list at all, or should be editor state saved in the chunk, is an open question for the Plugin and host engineer.

## Output Level curve (area 11 R7)

- **Position 0.1 to 1** (0.1 = `kOutputFadeStart`, **PROVISIONAL**): linear in dB from −36 to +36 dB.
- **Position 0 to 0.1:** amplitude = (gain at −36 dB) · (position / 0.1)². This is continuous and monotonic, and reaches silence at 0.
- **0 dB** sits at position 0.55.
- **Silence** is stored as −200 dB and displayed as "-inf dB".

## State chunk (CLAP state; VST3 via clap-wrapper)

The chunk is little-endian:

1. `u32 magic` = "AMQS";
2. `u32 version` = 2;
3. `u32 count`;
4. then `count` × (`u32 host id`, `f64 plain value`).

Version 1 (the Phase 1 skeleton) had no pairs and still loads, as all defaults.

On load:
- unknown ids are ignored;
- missing ids take their defaults;
- values go through the setter with source Preset;
- the host gets `params.rescan(VALUES)` on the main thread, deferred when the load arrives on another thread.

**PROVISIONAL:** values are applied one at a time. Applying them all at once before the next block (area 09 R15, area 21 R14) is the batch-load task. The JSON preset format comes later.

## Not in this table yet

- Gesture helper and GUI attachments (areas 09, 10).
- Audio-side ramps and block splitting (areas 01, 09).
- Batch load.
- The per-shape displayed-Q to design-Q convention (area 09 R12, documented in the Phase 0 DSP notes).
