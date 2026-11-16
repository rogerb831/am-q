# Clean-room rules for Am-Q code

Am-Q is built from public DSP literature and from Am-Q's own behaviour-level plan.
It is never built from another product's code, data or reverse-engineering results.

## Rule for everyone who writes Am-Q code (people and AI sessions)

1. The only specification is the sanitized plan in [`docs/plan/`](docs/plan/README.md)
   plus the public references it cites (papers, textbooks, permissively licensed code).
2. Do **not** open, list, search or read any material outside this repository about another
   product: its files, reverse-engineering notes, analysis results, disassembly, decompiled
   code or evidence bundles.
3. Do not use reverse-engineering tooling (for example REA or Frida) in this repository's work.
4. If anything in the spec looks like another product's internals (addresses, decompiled
   code, internal function descriptions, copied constant tables), stop and tell the
   project lead and counsel. Do not build from it.
5. Every DSP component cites its public source in code comments or in its design note.
6. Every third-party dependency is listed with its licence in [`THIRD_PARTY.md`](THIRD_PARTY.md).

This is engineering guidance, not legal advice. Counsel owns the clean-room review.

## Exposure log

Record here any time a code author saw material outside the sanitized spec.

| Date | Who | What was seen | Action |
| --- | --- | --- | --- |
| 2026-11-13 | AI coding session that wrote the Phase 0 DSP sandbox (initial scaffold) | Before the spec was sanitized, this session read the earlier version of the plan's tab "01 Core DSP and host path" in the shared Claude Doc. That version contained a "Source and scope" paragraph naming the analysed product and the findings report, and a "Coverage" table summarising static-analysis and runtime findings about that product (statements about its internal structure, not addresses or code). The session also glanced at the main tab's extract. | The session stopped and reported. Its local extracts were deleted. It then built only from `docs/plan/` and public references. Counsel should weigh this exposure when reviewing the code. |

## Design system provenance

- **Original provenance note (kept for history):** the token notes for the Am-Q design
  system (`docs/design/spike-reference/tokens.json`, `bundle.css`, `editor-main.dc.html`)
  said its colours were sampled from a reference screenshot of a dark pro-audio EQ. The
  plan's clean-room table lists another product's colours and layout as not allowed, so
  the AI coding session raised this before using the tokens.
- **2026-11-13:** the project owner confirmed the Am-Q design system is cleared for use
  ("its clear to use"). The confirmation was relayed to this coding session by the
  coordinating session. The spike window uses the local sanitized files only; the online
  design canvas is not opened from this repository's work.
- The plan still requires Legal to review the UI design before Phase 2 starts
  (`docs/plan/README.md`, "Clean-room rules", item 3).
- **2026-11-16 (design canvas hand-over):**
  - **What was supplied.** The analyst side supplied the full Am-Q design canvas as
    `docs/design/reference/`:
    - five 1280×760 boards: Main, Processing, Analyzer, Interactions and Match;
    - `canvas.json`;
    - `ds/am-q/tokens.json`;
    - `ds/am-q/components/bundle.css`.
  - **Latency figures removed.** The Linear Phase latency figures were taken out before the
    hand-over and replaced with placeholders ("[latency]", "[from area 02 latency
    function]"). The Natural Phase figure on the boards (320 samples) is the plan's own
    value (area 05 R2).
  - **Cleared.** The project owner cleared the design for use. The coordinating session relayed
    this to the AI coding session.
  - **Replaces the spike reference.** These files replace `docs/design/spike-reference/` as
    the design source. The spike copy stays for history.
  - **Spec written from them.** The AI coding session wrote `docs/design/editor-spec*.md` from
    these files, `docs/plan/` and code only. Every latency figure in the spec comes from
    `core/dsp/include/amq/dsp/latency.hpp`, never from the boards.
  - **What the files contain.** Only layout, tokens and Am-Q's own labels. No addresses,
    decompiled code or other-product internals were seen.
  - **Provenance still under review.** `tokens.json` still says its provenance is "under
    counsel review". The Legal UI review above still applies.

## Provisional items pending counsel review

Area 01's **R15** (read parameter streams every 4 samples, redesign coefficients at most
every 32 samples), **R17** (100 ms section-count crossfade) and **R18** (TPT/ZDF SVF kernel
details) are pending counsel review. Any code touching them must:

- keep the values and the schedule behind an interface so they can change
  (see `core/dsp/include/amq/dsp/update_policy.hpp`);
- mark them `PROVISIONAL (counsel review pending)` in code comments.

The Phase 0 sandbox implements no kernel and no scheduler yet; it only defines the
`UpdatePolicy` interface with the provisional defaults.
