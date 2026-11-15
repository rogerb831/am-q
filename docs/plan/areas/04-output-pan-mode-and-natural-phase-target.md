# 04 Output Pan Mode and Natural Phase target

Am-Q's Output Pan Mode switches the output balance between L/R and M/S by wrapping the Level/Pan node in an encode/decode pair. The Natural Phase target is the exact analog s-domain response of each band's prototype, evaluated at the kernel's bin frequencies with no pre-warp.

## Requirements

1. **R1 Pan Mode parameter.** Output Pan Mode has two values: L/R (default) and M/S.
2. **R2 Output graph.** The order is: input, mode encoder, Level/Pan/Invert gain node, mode decoder, output. In L/R mode both converters pass through.
3. **R3 M/S balance.** In M/S mode the balance law acts on M and S. Pan right lowers Mid (wider). Pan left lowers Side (narrower, reaching mono at full left). The GUI labels the ends “Mid” and “Side”.
4. **R4 Mode switch smoothing.** Switching Pan Mode with a non-centre pan crossfades over a short, click-free time (initial 20 ms).
5. **R5 Level and Invert in both modes.** Level and Invert give the same result in both modes, because they act equally on both components.
6. **R6 Pan has its own mode state.** The Pan parameter's display text follows the mode (L/R percent or M/S percent).
7. **R7 Monitor decode separate.** The solo/monitor path has its own decoder and does not share the Pan Mode converters (Tab 02).
8. **R8 Pan only in stereo.** Pan and Pan Mode are inactive on mono layouts.
9. **R9 Natural Phase target.** Each band's target is its analog transfer function H(jΩ). This is the product of its second-order s-domain sections, evaluated at the true bin frequency, with no bilinear warp or oversampling.
10. **R10 Digital response.** Each band's digital response comes from the exact coefficients the audio filter uses, from the same design call.
11. **R11 Mode-specific objects.** Target and digital response objects exist only in Natural Phase. Linear Phase uses a zero-phase magnitude object per band instead.
12. **R12 Caching.** Per-band responses are cached per bin and invalidated only when that band changes. A one-band edit recomputes only that band.
13. **R13 Kernel freshness.** After a parameter change, a new Natural Phase kernel takes effect within 20 ms at 48 kHz. During continuous automation, rebuilds are rate-limited and each swap crossfades.
14. **R14 Auto Gain grid.** Auto Gain evaluates the combined response on a 512-point log grid from 20 Hz to 20 kHz.

## Am-Q design

**Pan Mode.** Pan Mode reuses the M/S converter type from Tab 03, configured at the output. Changing mode runs old and new output graphs in parallel for 20 ms and crossfades. This avoids a jump when pan is off-centre.

**Analog response.** Each band's prototype exposes evaluate(Ω), which returns the complex product of its sections. Frequencies are normalised to the band's centre frequency, using our own documented normalisation. The digital response object evaluates the designed sections on the unit circle.

**Caching.** Both response objects cache values per grid bin, with a version stamp per band. The kernel builder multiplies only cached bins and rebuilds only when a stamp changes.

**Auto Gain grid.** The grid is defined in core/dsp and shared with the GUI, so the curve, Auto Gain and tests agree.

**Design commitments.**

- Explicit mode-switch crossfade.
- Documented pan direction in M/S.
- Bounded kernel latency under automation.

## Implementation tasks

- [ ] Add the Output Pan Mode parameter and per-mode pan display text (Plugin and host engineer, Phase 2)
- [ ] Build the output graph with mode converters and the 20 ms mode-switch crossfade (DSP lead, Phase 2)
- [x] Implement analog prototype evaluation and digital section evaluation (DSP lead, Phase 1). Analog: `AnalogPrototype::evaluate` (≤ 1e-9 against an independent reference, Phase 0 test). Digital: `dsp::svfResponse` / `svfCascadeResponseHz` evaluate the exact SVF sections the audio runs (5e-11 against the source biquads), and `engine::bandMagnitudeDb` designs through the engine's own calls (R10)
- [ ] Implement per-band response caches with version stamps (DSP lead, Phase 2)
- [ ] Implement a rate-limited kernel rebuild scheduler with crossfaded swaps (DSP lead, Phase 2)
- [x] Define the shared 512-point log grid for Auto Gain and the GUI curve (DSP lead, Phase 1). `core/dsp/include/amq/dsp/log_grid.hpp`: 20 Hz to 20 kHz inclusive, one shared instance
- [ ] Design Pan Mode button labels and pan-end labels (Product designer and GUI engineer, Phase 2)

## Acceptance tests

| Test | Method | Pass criterion |
| --- | --- | --- |
| L/R pan | Pan full right in L/R mode | Left silent; right unity |
| M/S pan right | Pan full right in M/S mode on L=R input | Output silent; pure side content passes at unity |
| M/S pan left | Pan full left in M/S mode on wide input | Output is mono (L = R) |
| Mode switch | Toggle Pan Mode at pan 50 % during noise | No discontinuity > −90 dBFS; crossfade 20 ms ±5 % |
| Analog target | Compare evaluate(Ω) with an independent reference implementation | ≤ 1e-9 relative error |
| Cache invalidation | Edit one band among 24 | Only that band's cache recomputed |
| Kernel freshness | Step a band gain in Natural Phase | New response in place within 20 ms |
| Grid sharing | Compare Auto Gain grid, GUI grid and test grid | Identical |

## Open questions

- Should M/S pan right mean “wider”, or follow another user-tested convention? Product designer to confirm by user tests.
- Should Pan Mode be automatable, or a non-automatable setting?
