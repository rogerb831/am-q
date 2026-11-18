# Am-Q decision log

Engineering decisions that close an open question in this plan. Newest first. Each entry
states the decision, who made it, the evidence and what it changes.

## D4 · Editor design approved as built; names frozen (2026-11-18)

**Decision (product owner, 2026-11-18):** "Approve all as built" for the editor spec (`docs/design/editor-spec*.md`), and "freeze them" for the names.

**What it covers:**
- **The editor spec as built:** the region map and reflow, the interaction and wheel tables (drag threshold 3 px, Shift fine 0.1×), the key map with the band panel keys, the band menu and the graph menu (Select All, Paste, Delete Disabled Bands, Delete All Bands), the Output menu labels and ranges (Pan Mode "L/R" / "M/S", Gain Scale 0–200 %, Solo Gain ±20 dB), the EQ Match panel, the preset menu with its search, and every PROPOSED item P-1 to P-32.
- **Names for v1:** shapes Bell, Low Shelf, Low Cut, High Shelf, High Cut, Notch, Band Pass, Tilt Shelf, Flat Tilt, All Pass; modes Zero Latency, Natural Phase, Linear Phase; the product name Am-Q.

**Deferred (product owner, same day):** band Split (stays a disabled menu entry) and the Output Level fade law (stays PROVISIONAL: bottom 10 % of the control, square law).

**What it changes:** the designer tasks in areas 04, 11 (ranges), 12, 13, 14 (menus), 15, 16, 17 and 20 are closed. Code comments that marked these choices PROVISIONAL for the designer now cite the spec. DSP and engineering constants (EQ Match cut bypass, undo depth, spectral and dynamics constants) keep their own PROVISIONAL marks.

## Proposed · Versioning (2026-11-18, PROPOSED: not yet decided by the product owner)

**Mechanism (built):** `project(AmQ VERSION x.y.z)` in `CMakeLists.txt` is the only place the version is set. At every build, `cmake/gen_version.cmake` writes `generated/amq_version.h`. The CLAP descriptor reports `AMQ_VERSION_FULL`, VST3 reports the same (clap-wrapper reads the descriptor), and both Windows DLLs carry it in a version resource. `AMQ_VERSION_FULL` is `x.y.z` on a commit tagged `vx.y.z`, otherwise `x.y.z+<short hash>`, plus `.dirty` with uncommitted changes. CI fails a `v*` tag build whose tag is not `v` + the project version, and names the Windows package after the version.

**Proposed meaning of the numbers:** SemVer, where the public interface is the user's saved work (projects, presets, automation):
- **Major:** anything that can break or change an existing project or preset: a parameter removed or renumbered, a stored value's meaning changed, or a change that makes a saved session sound different on reload. The plugin id never changes.
- **Minor:** additive changes: new parameters on new ids, new features, shapes or formats. Old sessions load and sound the same.
- **Patch:** fixes that leave saved sessions sounding the same (crashes, UI fixes, bit-identical performance work).
- **Before 1.0:** `0.minor` releases may break the rules above; 1.0 is the first release that keeps them.
- **Open call:** a DSP bug fix that audibly changes existing sessions. Proposed: major, or at least minor with a release note.

The state chunk's own versions (`AMQS`, `AMQE`; parameter table, State chunk) are independent of the product version: they describe the format, and loading stays compatible across product versions.

## D3 · Natural Phase kernel window: none (rectangular) (2026-11-16)

**Decision (product owner, 2026-11-16):** the Natural Phase kernel uses no window (rectangular).
Area 05 R7 gains "no window" as a candidate. Chosen from: no window (recommended), Kaiser β=2
(the best of R7's original candidates), or keeping Hann.

**Evidence:** `amq_engine_tests -tc="Natural Phase window evaluation*" --no-skip`, with the
area 03 regularisation, over 28 bands (the 16 accuracy bands and 12 stress cases: notches on and
off bins, deep cuts, bands near Nyquist, an all-pass, a +18 dB Q16 bell at 20.5 kHz and a +12 dB
Q10 bell at 40 Hz) at 44.1, 48 and 96 kHz, against the analog target to 0.95 Nyquist.
- No window: worst 0.0088 dB and 0.072 degrees; above 0.95 Nyquist within 0.024 dB.
- Kaiser β=2: 0.054 dB; Hann 0.18 dB; Blackman-Harris 0.35 dB. Every window misses only on the
  20.5 kHz Q16 bell at 44.1 kHz, where tapering smears a sharp correction; without that band all
  candidates pass.
- Once the regularisation removes the 0/0 bins, the correction is smooth: frequency sampling
  with no window is exact at the bins and the most accurate between them.

**What it changes:** `NaturalPhaseKernelBuilder::kDefaultWindow` is `Rectangular`. Area 05's
window task is closed. The other windows stay available for later evaluations.

## D1 · Framework: CLAP-native core + clap-wrapper + Visage (2026-11-14)

**Decision (product owner, 2026-11-14):** Am-Q is a CLAP-native plugin. VST3 is built from
the same code by clap-wrapper. The editor uses Visage (GPU UI). JUCE 8 is not used.
Screen-reader accessibility does not have to ship in v1.

Owner's words, relayed by the coordinating session: "No, go CLAP + Visage", in answer to
"Must screen-reader accessibility ship in v1?".

**Evidence:** the framework spike in `spikes/clap-native/` (README there).
- Sample-accurate automation and latency-change notification pass in both CLAP and VST3, measured in our own test hosts.
- clap-validator, the Steinberg VST3 validator and pluginval (strictness 10) are clean.
- Dirty-region redraw and display-rate drag work in our own host.
- Hosts: loads in Studio One (scan) and Fender Studio (automation works, user report). Blue Cat's Axiom crashed on load, then worked after a fix; editor drag and the Q wheel work there (user report).
- Licences are all permissive: CLAP, clap-helpers, clap-wrapper, VST3 SDK 3.8 and Visage are MIT. See `THIRD_PARTY.md`.
- Visage has no accessibility layer (no UIA, MSAA or NSAccessibility support).

**What it changes:**
- **Formats.** CLAP is the native API. VST3 comes from clap-wrapper. clap-wrapper can also build AUv2, so the AU question stays open but needs no new framework.
- **Accessibility.** In v1: keyboard navigation (area 15 R3), HiDPI scaling, and an accessible-control model in the UI code (area 15 R12). After v1: the screen-reader layer (UIA on Windows, NSAccessibility on macOS), built on that model.
- **Keyboard.** Visage provides key events, keyboard focus and handled/unhandled results, and it forwards unhandled keys to the host window (on Win32 by posting to the parent window; on macOS through the normal NSResponder chain). It has no general Tab focus traversal, so Am-Q implements its own focus order. Keyboard navigation therefore stays in v1 and needs no accessibility layer.
- **Threading.** Every callback a VST3 host can reach through clap-wrapper must be safe on any thread. VST3's threading is looser than CLAP's, and the wrapper passes it through. Example: Blue Cat's Axiom calls `getLatencySamples()` off the main thread.
- **Host checks.** clap-helpers' `Terminate` misbehaviour handler never ships, because it aborts the host on a contract violation (the Axiom crash). Strict checks are a development build option only.
- **Effort.** The plan's estimate for this route was about 2 extra months over JUCE: our own editor widgets, focus handling and accessible-control model, and a less mature UI library (Visage v1.0, one tagged release).
- **Risks added:** Visage maturity; no Visage accessibility; macOS untested in the spike; the editor not yet checked in a DAW on a local display.

## D2 · FFT backend: PFFFT (2026-11-15)

Owner's words: "counsel is go", then PFFFT chosen from three permissive candidates (PFFFT, PocketFFT, KissFFT).

**What:** PFFFT v1.3.0 (`179a48d`, https://github.com/marton78/pffft), BSD-style licence (FFTPACKv5 / UCAR plus the PFFFT authors). Only its two single-precision core files are compiled (`cmake/AmqFftDeps.cmake`); the benchmark-only copies of other FFT libraries in its repository are not.

**Why:** SIMD real FFTs (SSE, NEON), fast at the 128 to 8192 sizes the analyzer and the Natural Phase convolver use, and widely used in audio.

**What it changes:**
- **Interface.** It sits behind Am-Q's own `amq/dsp/fft.hpp` (area 01 R22) and can be replaced without touching callers. `fft_pffft.cpp` is the only file in core/dsp that includes it.
- **Sizes.** Real transforms need n = 32 · 2^a · 3^b · 5^c with SIMD. All planned sizes (128 for the 64-sample partition, 512 for the Natural Phase kernel, the analyzer's powers of two) qualify.
- **Licence obligations.** Keep the copyright notices and the disclaimer in source and binary distributions (THIRD_PARTY.md; the product's licence notice must reproduce them).

