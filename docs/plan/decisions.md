# Am-Q decision log

Engineering decisions that close an open question in this plan. Newest first. Each entry
states the decision, who made it, the evidence and what it changes.

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
