# 08 Small gaps

This plan closes three small gaps. The auto-threshold envelope has an instant attack and a release of about 10 s. Density reaches the spectral engine through a parameter binding, which Am-Q makes explicit in its table. A per-parameter "shape category" hint has no known caller, so we defer it.

## Requirements

1. R1. The auto-threshold reference envelope has an instant (peak) attack and a slow release. The default starts at about 10 s, is tuned by listening, and is documented.
2. R2. The auto-threshold envelope's times do not change with the Attack or Release knobs.
3. R3. Every DSP value derived from a parameter, Density included, updates through the parameter table. There is no hidden second write path. Updates land within one block.
4. R4. Every parameter write clamps to range. Continuous DSP parameters then ramp to the new value (Tab 09 R13).
5. R5. The parameter table stores per-shape metadata: family, gain-capable, slope-capable. GUI, EQ Match and any future host hint use this one source.
6. R6. No host-facing filter-category hint ships in v1.

## Am-Q design

- **Reference envelope.** One-pole peak follower: instant rise and slow fall. It lives in the auto-threshold block of the dynamics core (Tab 07), separate from the Tab 06 smoothers.
- **Density path.**
  - Density is an ordinary band field in the flat table.
  - The audio thread reads its slot once per block and compares it with the last value.
  - On change, it schedules the grid rebuild (Tab 07).
  - This is one explicit, testable read.
- **Parameter write.** set(id, plain, source) clamps, stores and notifies. Ramps are created on the audio side when a slot change is seen (Tab 09).
- **Shape metadata.** Each Shape enum value carries a static descriptor in core/params. A future category hint for control surfaces or hosts would map from it. Neither VST3 nor CLAP has a standard filter-type hint, so v1 has none.

## Implementation tasks

- [ ] Implement the instant-attack, slow-release reference envelope with a tunable release (DSP lead, Phase 2)
- [ ] Add a unit test that Attack/Release changes leave reference-envelope state untouched (Build and QA engineer, Phase 2)
- [ ] Implement change detection on the Density slot that triggers the grid rebuild (DSP lead, Phase 2)
- [ ] Add static shape descriptors to core/params and use them in the GUI and EQ Match (Plugin and host engineer, Phase 1). Progress: the descriptors are in core/params, and a test checks they match core/dsp. Still to do: using them in the GUI and EQ Match
- [ ] Record the deferred category hint in the post-v1 backlog (Product designer, Phase 4)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| Reference attack | +20 dB side-chain step, Auto on | Reference envelope reaches 90 % within one sample |
| Reference release | −20 dB step after 5 s of signal | Fall time within ±5 % of the documented release |
| Independence | Sweep Attack/Release during steady input, Auto on | Reference envelope deviates by less than 0.01 dB |
| Density update | Automate Density at a known sample offset | Grid rebuild starts in the block containing that offset |
| Clamping | set() with values outside range from each source | Stored value equals the clamped value |
| Shape metadata | Unit test over all shapes | GUI gain-enable, slope-enable and EQ Match eligibility match the descriptors |

## Open questions

- Should a category hint be offered later through a CLAP extension or control-surface protocol? Revisit after v1.
