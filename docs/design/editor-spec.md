# Am-Q editor specification

This spec is written from the Am-Q design canvas in [`reference/`](reference/). It gives the GUI engineer enough detail to rebuild every board without opening the HTML.

- **Status:** draft, 2026-11-16. Area 12's task "Define the editor region map and the visual hierarchy" points here.
- **Boards:** seven boards, 1280×760 each, listed in `reference/canvas.json` in this order (revised to match this spec on 2026-11-17):
  - **Main:** the editor, with the graph and the band panel.
  - **Match:** EQ Match.
  - **Processing:** the processing menu and the output menu.
  - **Analyzer:** analyzer and interface settings.
  - **Interactions:** multi-select, the band menu, the shape menu, the slope picker and the band list.
  - **States:** solo, the inline value editor, the side-chain filter popover and the one-band menu.
  - **Hints:** hint mode, and the band panel's compact and strip modes.
- **Tokens:** `reference/ds/am-q/tokens.json` and `reference/ds/am-q/components/bundle.css`. These files replace `docs/design/spike-reference/` as the design source. The spike copy stays where it is for history.

The spec is split into three files:

| File | Sections |
| --- | --- |
| this file | 0 Ground rules and tokens · 1 Region map and reflow · 6 Modifier-to-action table · 7 Motion and timing · Conflicts with the plan · PROPOSED items |
| [editor-spec-panels.md](editor-spec-panels.md) | 2 Component inventory, region by region · 3 States |
| [editor-spec-graph.md](editor-spec-graph.md) | 4 Overlays and z-order · 5 The graph |

## How to read this spec

- **The plan wins.** `docs/plan/` wins over the boards on any conflict. Each conflict is listed under [Conflicts with the plan](#conflicts-with-the-plan), with the resolution this spec uses.
- **PROPOSED.** Anything the boards do not show is designed here in Am-Q's tokens and marked **PROPOSED**. Every PROPOSED item is listed at the end for the Product designer to confirm.
- **Demo state is not default state.** The boards show demo states, for example Auto Gain on, a frozen analyzer, or the averaging window at Last 10 s. Defaults always come from `docs/params/parameter-table.md`.
- **Latency figures come from code.** Every latency figure comes from `amq::dsp::latencySamples` (`core/dsp/include/amq/dsp/latency.hpp`, area 02 R9–R12, area 05 R2/R4), evaluated at the session rate. The boards' "[latency]" placeholders are never used.
- **Names.** "Token" names refer to `tokens.json`. Where this spec writes a hex value, it is the token's value, given for convenience.
- **Pixels.** Sizes are logical pixels at 100 % interface scale.

---

## 0 Ground rules and tokens

### 0.1 Token summary

The values below are copied from `tokens.json`. That file is the authority.

**Colours**

| Token | Value | Use (short) |
| --- | --- | --- |
| surface-000 | #0d0d10 | Meter troughs, segmented-control tracks, inset fields |
| surface-100 | #17161a | Window and graph ground |
| surface-200 | #201f24 | Bars, floating panels and menus |
| surface-300 | #2b2a30 | Raised controls: selects, chips, knob faces; menu item hover |
| surface-400 | #37363d | Hover of a surface-300 control; open select; segment on |
| line-grid | #26252b | Graph grid |
| line-hairline | #3d3c43 | Panel edges, dividers, knob track |
| line-zero | #8f8d96 | 0 dB line |
| ink | #eceaf0 | Primary text and icons |
| ink-muted | #a5a3ad | Secondary text, captions, shortcut hints |
| ink-axis | #8c8a93 | Frequency and analyzer axis labels (on surface-100 only) |
| ink-disabled | #5e5c66 | Disabled or bypassed text (never for must-read information) |
| accent | #ffb81c | Summed curve, knob arcs, gain scale, active readouts, focus ring |
| accent-hover | #ffc94d | Hover or drag of an accent element |
| accent-ink | #1a1305 | Text on an accent fill |
| band-1 … band-8 | #c4ee3c, #ff4d4d, #3aa0ff, #d84cf2, #7d62ff, #2fe0b0, #ff8a3d, #ff63ae | Band colours; band n uses band-((n−1) mod 8 + 1) |
| analyzer-pre | #3a3940 | Pre-EQ spectrum fill |
| analyzer-post | #7c7a84 | Post-EQ spectrum line; EQ Match input line |
| analyzer-sc | #5b6e8a | Side-chain spectrum line; EQ Match reference line |
| meter-ok / meter-hot / signal-clip | #3faf3a / #d9d33a / #ff3b30 | Meter below −18 dBFS / −18 to −3 dBFS and peak tick / above −3 dBFS, clip LEDs, collision hatch |
| signal-solo | #2fe0b0 | Solo and listen indicator dot, always paired with a text label |

**Type.** The family is Inter, with fallbacks "Segoe UI", system-ui, sans-serif. All numerals are tabular (`font-variant-numeric: tabular-nums`).

| Style | Size / line | Weight | Extra | Use |
| --- | --- | --- | --- | --- |
| wordmark | 30/32 | 800 | −0.02em, accent | The Am-Q wordmark only |
| preset | 15/20 | 600 | | Preset name |
| label | 13/16 | 500 | | Bar items, menu items, select text, panel rows |
| knob-label | 12/14 | 600 | 0.06em, uppercase | Knob captions and section captions ("SIDE CHAIN", "PROCESSING") |
| caption | 12/16 | 400 | | Secondary notes, "(edited)", explanations |
| readout | 13/16 | 600 | tabular | Live values, chips, knob values |
| scale | 12/14 | 500 | | Gain scale (accent), frequency and analyzer axes (ink-axis) |
| micro | 10/12 | 600 | 0.04em | Meter scale, band index badges ("#3"); minimum size |

**Spacing**

| Token | Value |
| --- | --- |
| space-1 | 4 |
| space-2 | 8 |
| space-3 | 12 |
| space-4 | 16 |
| space-6 | 24 |
| space-8 | 32 |

**Radius**

| Token | Value |
| --- | --- |
| radius-sm | 4 |
| radius-md | 8 |
| radius-lg | 16 |
| radius-full | pill |

The boards also use 6 px on menu items and segments. That value is not a token; see conflict C-17.

**Sizes**

| Token | Value |
| --- | --- |
| size-handle | 18 (22 selected) |
| size-knob-sm | 64 |
| size-knob-lg | 88 |
| size-meter | 6 |
| size-bar | 44 |

Dynamics knobs are 48 px. That size is not a token; see C-17.

**Opacity**

| Token | Value | Use |
| --- | --- | --- |
| opacity-band-fill | 0.38 | Band fill; the selected band uses 0.55 |
| opacity-band-idle | 0.18 | Fill of the other bands while one is dragged |
| opacity-bypassed | 0.35 | A bypassed band |

**Shadows**

| Token | Value |
| --- | --- |
| shadow-panel | `0 16px 40px rgba(0,0,0,.55), 0 0 0 1px line-hairline` |
| shadow-handle | `0 0 0 2px surface-100, 0 2px 6px rgba(0,0,0,.6)` |
| shadow-knob | `inset 0 1px 0 rgba(255,255,255,.06), 0 4px 10px rgba(0,0,0,.5)` |

The boards draw the knob shadow as a drop-shadow filter with the same outer part.

**Focus.** Every focusable control shows a 2 px `focus` (= accent) outline, offset 2 px, while it has keyboard focus. Pointer focus does not show it (`:focus-visible` behaviour).

### 0.2 Shared primitives

These primitives are referenced throughout the spec.

| Primitive | Geometry and style |
| --- | --- |
| **Icon button** | 28×28, radius-md, transparent. Icon 16×16 in a 16-unit viewBox, stroke ink 1.5, round caps and joins, no fill. |
| **Chip** (toggle button) | Height 28 (32 where noted), padding 0 10, radius-md, surface-300, readout type in ink. On: accent fill, accent-ink text. |
| **Select** (opens a menu or popover) | Height 32 (28 in bars and compact rows), padding 0 12, gap 8, radius-md, surface-300, label type in ink. Optional 16 px leading glyph. Trailing 12 px chevron (`M3 4.5 L6 7.5 L9 4.5`, stroke ink-muted) pushed to the right. |
| **Segmented control** | Track surface-000, radius-md, padding 2, gap 2. Equal-width segments, height 28, radius 6, knob-label weight at 12/14 600 (no uppercase, no tracking), ink-muted. On segment: surface-400 fill, ink text. |
| **Readout chip** | Inline flex, gap 8, padding 3 8, radius-sm, surface-300, readout type in ink. Optional 8 px dot. Accent variant (`is-accent`): surface-200 fill, 1 px line-hairline ring, accent text. |
| **Floating panel** | surface-200, radius-lg (menus: radius-md, see below), shadow-panel. |
| **Menu** | surface-200, radius-lg (from `.amq-panel`), shadow-panel, padding 6. Items are 30 tall, radius 6, padding 0 10, gap 10, label type. Separator: 1 px line-hairline, margin 4 6. |
| **Range slider** (Gain Scale, EQ Match Amount and Bands) | See [panels §2.9](editor-spec-panels.md#29-range-slider-proposed-drawing). |

### 0.3 Wordmark and icon geometry

The icons are drawn in a 16-unit viewBox, stroke-only, 1.5 wide. The wordmark's Q glyph uses a 64-unit viewBox.

| Name | Path data |
| --- | --- |
| Wordmark | Text "Am-" in wordmark type, then the Q glyph at 1em, baseline offset −0.2em. The glyph is a circle at (28,28) r 17.5, stroke 11, and a tail `M37 37 C39.5 39.5 40.5 42.5 42.5 44.5`, stroke 11, round cap. A filled dot at (56,56) r 7. All in accent. |
| Undo | `M5 3 L2 6 L5 9` and `M2 6 H10 A4 4 0 0 1 10 14 H7` |
| Redo | `M11 3 L14 6 L11 9` and `M14 6 H6 A4 4 0 0 0 6 14 H9` |
| Copy A to B | `M3 8 H12` and `M9 5 L12 8 L9 11` |
| Previous / next preset | `M10 3 L5 8 L10 13` / `M6 3 L11 8 L6 13` |
| EQ Match (button glyph) | `M1 10 C4 10 4 5 7 5 C10 5 10 11 15 8` (ink) and `M1 12 C4 12 5 8 8 8 C11 8 12 10 15 10` (ink-muted, dash 2 2) |
| Settings (gear) | Circle (8,8) r 2.2 and `M8 1.5 V3.5 M8 12.5 V14.5 M1.5 8 H3.5 M12.5 8 H14.5 M3.4 3.4 L4.8 4.8 M11.2 11.2 L12.6 12.6 M3.4 12.6 L4.8 11.2 M11.2 4.8 L12.6 3.4` |
| Full screen | `M2 6 V2 H6 M10 2 H14 V6 M14 10 V14 H10 M6 14 H2 V10` |
| Help | Circle (8,8) r 6, `M6.5 6.2 A1.6 1.6 0 1 1 8.6 7.7 C8 8 8 8.6 8 9.2` and `M8 11.5 V11.6` |
| Power (band Enabled, plugin Bypass) | `M8 2 V8` and `M4.5 4.5 A5 5 0 1 0 11.5 4.5` |
| Headphones (Solo, SC Listen) | `M3 10 V8 A5 5 0 0 1 13 8 V10`, plus rect (2,10) 3×4 rx 1 and rect (11,10) 3×4 rx 1 |
| Delete (×) | `M4 4 L12 12 M12 4 L4 12` |
| Freeze (snowflake) | `M8 2 V14 M2.8 5 L13.2 11 M2.8 11 L13.2 5` |
| Invert polarity | Circle (8,8) r 4.5 and `M12.5 3.5 L3.5 12.5` |
| Placement (two rings) | Circles (6,8) r 4 and (10,8) r 4 |
| Check | `M3 8.5 L6.5 12 L13 4.5` (accent) |
| Invert Gain | `M2 8 C5 8 5 3 8 3 M8 13 C11 13 11 8 14 8` |
| Make Dynamic | `M8 3 V13 M5 6 L8 3 L11 6` |
| Make Spectral | `M2 12 V9 M5 12 V6 M8 12 V8 M11 12 V4 M14 12 V7` |
| Slope | `M2 4 L14 12` |
| Copy | rect (5,5) 8×9 rx 1 and `M3 11 V3 H10` |
| Paste | rect (4,3) 8×11 rx 1 |
| Split | Circles (4,12) r 2 and (12,12) r 2, plus `M5.5 10.5 L12 2 M10.5 10.5 L4 2` |

**Shape glyphs.** The same glyph appears in the band panel's Shape select and in the Shape menu.

| Shape | Path |
| --- | --- |
| Bell | `M1 12 C5 12 6 4 8 4 C10 4 11 12 15 12` |
| Low Shelf | `M1 5 L4 5 C7 5 7 11 10 11 L15 11` |
| Low Cut | `M1 14 C4 14 5 4 9 4 L15 4` |
| High Shelf | `M1 11 L6 11 C9 11 9 5 12 5 L15 5` |
| High Cut | `M1 4 L7 4 C11 4 12 14 15 14` |
| Notch | `M1 6 L6 6 C7 6 7.5 14 8 14 C8.5 14 9 6 10 6 L15 6` |
| Band Pass | `M1 14 C5 14 6 4 8 4 C10 4 11 14 15 14` |
| Tilt Shelf | `M1 11 L4 11 C7 11 9 5 12 5 L15 5`, plus `M8 2 V14` dashed 1.5 2 |
| Flat Tilt | `M1 12 L15 4` |
| All Pass | `M1 8 L15 8` and circle (8,8) r 2 |

---

## 1 Region map and reflow

### 1.1 Default layout

The boards are drawn at 1280×760. The plan's default editor size is 1180×672 (**PROVISIONAL**, `ui::Editor::kDefaultWidth/Height`, area 12). See conflict C-1. The map below is the board's frame. Every region is anchored, so the layout is the same rule at any size. Coordinates are x, y, width, height with the origin at the window's top-left.

| Region | At 1280×760 | Anchor rule |
| --- | --- | --- |
| **Top bar** | 0, 0, 1280, 44 | Full width, top, height size-bar. surface-200. Above the graph in z. |
| **Graph region** | 0, 44, 1280, 672 | Fills the space between the bars: height = H − 88. |
| – Plot area (curves, grid, nodes) | 0, 68, 1180, 624 (graph-local 0, 24) | Left 0, top inset 24, bottom inset 24, right gutter 100. The plot runs to the window's left edge. There is no left gutter. |
| – Right gutter | 1180, 68, 100, 624 | Gain scale labels right-aligned at plot right + 30. Analyzer dB labels right-aligned at plot right + 66. Meters at plot right + 78 and + 86, 6 wide each, 2 apart, ending 8 from the window edge. |
| – Frequency axis labels | baseline y = plot bottom + 16 (board y 708) | Centred on each frequency gridline, inside the 24 px bottom inset. |
| – Selected-band readout | node + (18, −44) | See [graph §5.6](editor-spec-graph.md#56-readouts-on-the-graph). |
| – Analyzer or Match legend | 24, 68 (graph-local 24, 24) | Top-left of the graph region, inset 24. Shown on demand (§4). |
| – Band panel | centred; bottom = graph bottom − 32 | Horizontally centred in the graph region, 32 (space-8) above the graph's bottom edge. Width depends on the band's state (panels §2.3). Floats over the plot. |
| **Status bar** | 0, 716, 1280, 44 | Full width, bottom, height size-bar. surface-200. |
| Popovers opened from the status bar | bottom = graph bottom − 8 | Processing, Output and Analyzer and Interface panels, anchored per §4. |

The default view has no left sidebar. The band list is a popover.

**Visual hierarchy.** From most to least prominent:

1. The summed curve (accent, 2 px).
2. The selected band: a 22 px handle with an ink ring, its fill at 0.55 and the readout chip.
3. The other bands: 18 px handles, fill 0.38.
4. The band panel (surface-200, floating, shadow-panel).
5. The bars (surface-200, flat).
6. The analyzer (pre fill and post line, both low-contrast greys).
7. The grid (line-grid).

The accent colour is reserved for the summed curve, active values and "on" toggles. Nothing decorative uses it.

### 1.2 Reflow between 640×360 and 3840×2160

The size limits are logical pixels at 100 % interface scale (area 12: 640×360 to 3840×2160, **PROVISIONAL**). The free aspect ratio is kept. Bars, panels, type and controls never scale with the window. Only the plot area grows. The boards show one size only, so the reflow rules below are all **PROPOSED** (P-1 to P-6).

**Top bar.** The three clusters are left, centre and right. The centre cluster is centred on the window. If it would overlap a side cluster (less than 16 px clear), it centres in the free space between the two side clusters instead. As the width shrinks, items collapse in this order (P-1):

| Width | Change |
| --- | --- |
| < 1100 | The EQ Match button shows its glyph only, with the tooltip "EQ Match". |
| < 960 | The A/B chips and Copy A to B move into an overflow icon button (`…`, three 1.5-unit dots), which opens a menu with "A", "B" and "Copy A to B". |
| < 800 | Full screen and Help move into the same overflow menu. The preset button's minimum width drops from 220 to 140, and the name ellipsizes. |
| < 700 | The previous and next preset buttons are hidden. The preset button still opens the preset menu. |

**Status bar.** The left group is anchored left and the right group right, with at least 16 between them. Items collapse in this order (P-2). Each collapsed item stays reachable from the Output or Processing menu, which already contain it:

| Width | Change |
| --- | --- |
| < 1180 | "Gain Scale 100 %" and the separator before "Output" are hidden. Gain Scale stays in the Output menu. |
| < 1020 | The "Output" caption and Invert polarity are hidden. Invert is in the Output menu. |
| < 900 | The Auto Gain chip is hidden. It is in the Output menu. The Processing select drops its latency text, which stays in the menu. |
| < 760 | The Analyzer select drops its "Analyzer" caption, showing only the summary ("Pre + Post"). |

**Plot.** The plot width is W − 100 and its height is H − 88 − 48.

*Frequency gridlines and labels* (P-3):

- Major gridlines and labels sit at 20, 50, 100, 200, 500, 1k, 2k, 5k, 10k and 20k Hz.
- If two neighbouring labels would sit less than 8 px apart, drop the 50, 500 and 5k labels first (the gridlines stay), then 20, 200 and 2k.
- When a decade is 600 px or wider (wide windows, or zoomed in), add minor gridlines at 30, 40, 60, 70, 80 and 90 × 10ⁿ. They use line-grid at 50 % opacity, with no labels.

*Gain gridlines* (P-4):

- The grid step is display range ÷ 6, drawn at every step from −range to +range.
- Gain labels go on every second gridline, plus 0.
- If one gridline step is shorter than 12 px, draw every second gridline only.
- If a label step is shorter than 20 px, label only ±range and 0.

*Analyzer labels* (P-4):

- Labels go every 10 dB. If that step is shorter than 20 px, they go every 20 dB.

**Band panel** (P-5):

- **Wrapping.** If the panel's one-row width (panels §2.3: 632, 1080 or 1140) is more than graph width − 48, the dynamics group and the SIDE CHAIN column wrap onto a second row below the main row. They keep their left divider, now drawn as a top divider (1 px line-hairline, 16 above the row). The panel width becomes the wider of the two rows.
- **Compact mode** applies when the graph region is shorter than 520 or narrower than 680:
  - knobs become 64 (Gain), 48 (Freq and Q) and 40 (dynamics);
  - selects and chips become 28 tall;
  - panel padding becomes 12 16;
  - the bottom offset becomes 16.
- **Strip mode** applies when the graph region is shorter than 400. The panel collapses to a 44 tall strip with:
  - the band dot;
  - "Band #N";
  - the shape name;
  - the readout `freq · gain · Q`;
  - an expand chevron (up).

  Expanding shows the compact panel over the plot. It collapses again when the selection changes or on Escape.

**Popovers** (P-6). Every popover keeps its board width, clamped to window width − 32. Its height is clamped to graph height − 16, and its content scrolls inside the panel's padding. A popover anchored to a bar control is placed per §4 and then shifted to stay at least 16 from the window edges.

**At 3840×2160.** Only the plot grows. The band panel stays its token size, centred. The minor gridlines appear, and analyzer and curve sampling follow the plot width (area 12 R12 accuracy holds per pixel).

### 1.3 Interface scale (100–300 %)

The Analyzer board's INTERFACE panel offers Scale at 100, 150, 200 and 300 %, captioned "Drag the window corner for any size in between." The plan requires clean scaling from 100 to 300 % (area 12 R16), but it has no user scale setting: the editor follows per-monitor DPI and declines `gui.set_scale` (area 12 progress note). See conflict C-2. The proposal (P-7):

- **Content zoom.** Interface scale z is a user zoom applied on top of the OS DPI factor. Every px value in this spec is multiplied by z, and the layout space is window size ÷ z.
- **Choosing a preset.** Choosing a Scale preset asks the host to resize the window to the current layout size × z, so the layout does not change. If the host refuses, the layout space shrinks or grows within the reflow rules instead.
- **Window limits.** The limits become 640z × 360z up to 3840 × 2160 (window logical px). At z = 300 % the layout space is therefore at most 1280×720.
- **Storage.** z is editor state saved in the state chunk, not a host parameter. It is not automatable and not part of undo.
- **Rendering.** All drawing is vector at device resolution. Hairlines stay 1 device-independent px × z, and are never thinner than 1 device px.

---

## 6 Modifier-to-action table

Area 13 says this table is an original Am-Q design document. It is built from the Interactions board's footer and band-list hints, then made consistent with area 13 R1–R22 and area 15. "Ctrl" is the logical command modifier (Ctrl on Windows, Cmd on macOS; area 13 R1). The Counsel review (area 13 task) applies to this whole table.

### 6.1 Pointer

| Input | Target | Action | Source |
| --- | --- | --- | --- |
| Double-click | Empty plot | Create a band at that frequency and gain, every field at its default, one undo step | Board hover readout "Double-click to add"; R3, R19 |
| Double-click with a modifier | Empty plot | No variants in v1 | **PROPOSED** P-20 (R4 lets the designer decide) |
| Double-click | Band node | Open the inline value editor (§4.9) | Area 13 design commitment |
| Click | Band node | Select and focus it | R6 |
| Shift-click | Band node | Add to the selection | README; band list hint "Shift adds"; R6 |
| Ctrl-click | Band node | Toggle in the selection | README "Shift/Ctrl-click"; R6 |
| Alt-click | Band node | Toggle Enabled, one undo step | Board "Alt-click: enable"; R7 |
| Alt-click | Range handle | Toggle Dynamics Enabled, one undo step | R7 |
| Click | Empty plot | Clear the selection | R5 |
| Drag | Empty plot | Lasso. Shift keeps the existing selection | R5; board lasso rectangle |
| Drag | Band node | Frequency (x, log) and Gain (y). Gainless shapes: Q (y) | R9 |
| Drag with a multi-selection | Any selected node | Every selected band moves by the same octaves and dB ("drag moves both") | Board; R14 |
| Shift during a drag | Node or range handle | Fine: steps × 0.1 | Board "Shift: fine 0.1×"; R12 |
| Alt-drag | Band node | Vertical drag changes Q | R13 (designer-assigned; matches the code) |
| Ctrl pressed during a drag | Node drag | Lock to the first dominant axis | R14 (optional axis lock; matches the code) |
| Drag | Range handle | Dyn Range only | R10 |
| Drag, band soloed | Soloed node, vertical | Solo Gain, shown on screen | R11 |
| Escape during a drag | | Cancel back to the values at the press | Code; area 15 |
| Right-click | Band node, or anywhere with a selection | Band menu for the selection, or for the clicked band if it is not selected | R8; area 14 |
| Press less than 3 px and release | | A click, not a drag (3 px is **PROVISIONAL**) | R2 |

### 6.2 Wheel

The board's footer reads "Wheel: Q · Ctrl: gain · Alt: range · Shift: slope". R12 says the fine modifier also scales wheel steps, and the board's fine modifier is Shift. So Shift cannot also select Slope. This spec keeps the board's Ctrl and Alt bindings, keeps Shift as fine everywhere, and moves Slope to Ctrl+Alt (conflict C-14, **PROPOSED** P-21):

| Input | Over | Action |
| --- | --- | --- |
| Wheel | A node, or the plot with a selection | Q (0.02 Q-position per notch, **PROVISIONAL**) |
| Ctrl+wheel | Same | Gain (0.5 dB per notch) |
| Alt+wheel | Same, on a dynamic band | Dyn Range (0.5 dB per notch) |
| Ctrl+Alt+wheel | Same | Slope, one whole step per notch, only slopes the shape allows (R15) |
| Add Shift | Any of the above except Slope | Fine: steps × 0.1 |
| Wheel | Empty plot, no selection | Zoom the frequency span about the pointer, within 10 Hz–30 kHz (code; R17) |
| Horizontal wheel or trackpad pan | Plot | Pan the zoomed frequency span (**PROPOSED** P-22; area 13 open question) |

The wheel direction is the same for every shape (R16): wheel up raises the value.

### 6.3 Keys

The key map is the Product designer's (area 15). Every key not listed passes to the host (area 15 R1–R6). Space always passes to the host: area 15's acceptance test has Space toggling the transport. So the band list's "Space enables" hint is replaced (conflict C-15).

| Key | Focus | Action |
| --- | --- | --- |
| Delete, Backspace | Graph or band list | Delete the selected bands (or the focused band), one undo step |
| Escape | Any | Cancel a drag; else close the innermost overlay; else clear the selection |
| ← / → | Graph | Frequency −/+ 1/12 octave (Shift: 1/96) |
| ↑ / ↓ | Graph | Gain +/− 0.5 dB (Shift: 0.1). Gainless shapes: Q |
| ↑ / ↓ | Band list | Move focus to the previous or next band. Shift extends the selection (board hint "↑ ↓ move · Shift adds") |
| Enter | Band list | Toggle Enabled of the focused band (**PROPOSED** P-23, replacing "Space enables") |
| Tab / Shift+Tab | Graph | Focus the next or previous band by frequency |
| Ctrl+Z, Ctrl+Shift+Z or Ctrl+Y | Editor | Undo, redo |
| Ctrl+A | Graph | Select all bands |
| Ctrl+X / C / V | Graph | Cut, copy, paste bands (board menu shows "Ctrl C", "Ctrl V") |
| Enter, Tab, Escape | Inline value editor | Commit; commit and move to the next field; cancel |

Arrow keys and Tab are graph commands only while the graph or band list has keyboard focus. Otherwise they pass to the host.

### 6.4 Hint strings

The status-bar hint text uses these exact strings, with " · " separators (the board's punctuation). The texts reflect this table, not the board's footer, where they differ:

- Multi-selection, left: "**N bands selected** · drag moves all · Shift: fine 0.1×". The board says "drag moves both" for N = 2. Keep "both" for exactly two.
- Right: "Wheel: Q · Ctrl: gain · Alt: range · Ctrl+Alt: slope · Alt-click: enable · Del/⌫: delete".
- Band list footer: "↑ ↓ move · Shift adds · Enter enables · Del removes".

Where the hints appear is **PROPOSED** P-24: in the status bar while Help is toggled on. See §4.13.

---

## 7 Motion and timing

The boards are static. They imply only the states, not the motion between them. Every duration below is **PROPOSED** (P-25), unless the plan sets it.

| Event | Motion |
| --- | --- |
| Hover and press colour changes | 80 ms linear |
| Handle size change on select (18 → 22) and its ring | 100 ms ease-out |
| Band fills changing opacity (0.38 ↔ 0.55, 0.18 while dragging) | 100 ms |
| Menus, submenus and popovers opening | 120 ms: fade from 0, plus a 4 px slide from the anchor side |
| Closing | 80 ms fade |
| Band panel content change (selection change, toggling Dynamic or Spectral) | Instant, no animation. The panel re-centres instantly (its width changes) |
| Readout chip | Follows the node or pointer every frame, no easing |
| Analyzer and meters | Updated at the snapshot rate, about 30 per second (area 12 R10). Ballistics come from the Speed setting (area 19 display follower), not from GUI easing. The meter peak-hold tick holds 1.5 s, then falls at 20 dB/s (**PROPOSED**) |
| Gain-change displays (range arc, live curve) | At the snapshot rate (area 14) |
| EQ Match progress | Updated at least every 100 ms (area 16 acceptance test), drawn without easing |
| Curve redraw during a drag | Within the same frame as the parameter change (area 13 R22 "within one frame") |
| Reduced motion | If the OS asks for reduced motion, all transitions are 0 ms |

No timing affects audio. The processing-mode change fade (10 ms, area 02) is audio-side and has no GUI animation.

---

## Conflicts with the plan

Each conflict lists what the boards show, what `docs/plan` (or the parameter table it owns) says, and what this spec does. "Plan wins" means the spec follows the plan, and the board needs a revision.

| # | Topic | Boards | Plan | This spec |
| --- | --- | --- | --- | --- |
| C-1 | Default editor size | 1280×760 | 1180×672, **PROVISIONAL** pending this region map (area 12; `ui::Editor`) | Resolved 2026-11-17: the project owner chose 1280×720. All regions are anchored, so the layout works at any size |
| C-2 | Interface scale | User Scale 100/150/200/300 % | R16 requires 100–300 % scaling; the editor follows OS DPI and declines `gui.set_scale`; no user zoom setting | **PROPOSED** P-7 adds a user zoom on top of DPI; the designer must confirm the setting exists |
| C-3 | Linear Phase resolution labels | Low, Med, High, V. High, Max (five, default Med) | `global.resolution`: Low, Medium, High, Very High, Maximum (area 02 R11), default Medium **PROVISIONAL** | No conflict in count or default. The segments use the abbreviated labels; the accessible names use the full choice names |
| C-4 | Latency figures | "320 smp" for Natural Phase; "[latency]" and "[from area 02 latency function]" for Linear Phase | `latencySamples()`: Natural Phase 320 at every rate; Linear Phase FIR/2 + partition × rate factor | From code at the session rate: Natural Phase 320. Linear Phase at 44.1/48 kHz: 2,112 · 4,160 · 8,256 · 16,448 · 65,600 smp (Low to Maximum). These double at each rate step (≤ 100 kHz × 2, ≤ 200 kHz × 4, above × 8) |
| C-5 | Spectral latency wording | "Spectral bands add FFT latency in every mode." | They do not add. Zero Latency reports the FFT latency; Natural Phase reports the larger of 320 and the FFT latency (**PROVISIONAL**, area 02 open question); Linear Phase is unchanged | Caption: "While a spectral band is active, latency is at least the FFT latency at this resolution." The menu rows always show the latency `latencySamples` reports now, spectral flag included |
| C-6 | Resolution scope | Captioned "Linear Phase resolution" | `global.resolution` also sizes the spectral FFT path in every mode (latency.hpp) | Caption: "Resolution". Sub-caption: "Linear Phase, and spectral bands in every mode". Enabled in every mode |
| C-7 | Analyzer speed | Slow, Medium, Fast | `analyzer.speed`: Very Slow … Very Fast (five), default Medium | Plan wins: five segments V. Slow, Slow, Medium, Fast, V. Fast |
| C-8 | Analyzer resolution | Segments labelled 2048, 4096, 8192, 16384 | `analyzer.resolution`: Low, Medium, High, Maximum (**PROVISIONAL** labels); the FFT size scales with rate (area 19 R2) | Plan wins: segments Low, Medium, High, Max. Caption shows the actual size, for example "4096-point FFT at 48 kHz" |
| C-9 | Analyzer tilt | A select reading "4.5 dB/oct" | `analyzer.tilt`: continuous 0–6 dB/oct, default 4.5 (area 19 R6) | Plan wins: a draggable readout chip with an inline editor, and a preset menu at 0, 1.5, 3, 4.5 and 6 (**PROPOSED** P-12) |
| C-10 | Analyzer range | Select "90 dB" | `analyzer.range`: 60, 90, 120 dB, default 90 | Agrees |
| C-11 | Display (gain) range | Gain scale fixed at ±18 dB | `display.range`: 3, 6, 12, 30 dB, default 12 | Plan wins: default ±12 dB; ±18 is not a choice. Grid rule §1.2. The selector is **PROPOSED** P-13 |
| C-12 | Gain Scale range | Slider −100 to 200 % | `global.gainScale` 0–200 % (area 03 R6) | Plan wins: 0–200 % |
| C-13 | Solo Gain | A toggle "On", "Level match while a band is soloed" | `global.soloGain`: continuous ±20 dB, default 0, audition level while soloed (area 02 R14) | Plan wins: a draggable dB readout, captioned "Audition level while a band is soloed" |
| C-14 | Wheel modifiers | Ctrl gain, Alt range, Shift slope | R12: the fine modifier also scales wheel steps; Shift is the fine modifier (board) | Ctrl gain, Alt range, Ctrl+Alt slope, Shift fine (P-21). The code today uses Shift gain and Command slope, so it follows this table once confirmed |
| C-15 | Space in the band list | "Space enables" | Area 15: every unlisted key, Space included, goes to the host; acceptance test "Space toggles the transport" | Plan wins: Space passes through. Enter toggles Enabled in the band list (P-23) |
| C-16 | Band menu items | Enabled, Invert Gain, Make Dynamic, Make Spectral; Shape, Slope, Placement; Copy, Paste, Split, Delete | Area 14 R3 also lists a selection group and Cut; split is undefined (area 14 open question) | Cut and Select All added (P-15). Split is shown but disabled until the Product designer defines it |
| C-17 | Non-token values | Radius 6 on menu items and segments; 48 px dynamics knobs; 0.6 opacity on un-applied EQ Match nodes; peak hold drawn in ink-muted | Tokens only (bundle.css says "every value is a token") | Resolved 2026-11-17: the canvas's tokens.json now has `radius-xs` 6, `size-knob-xs` 48 and `opacity-preview` 0.6. The source design system does not have them yet |
| C-18 | Placement choices | The panel shows Stereo, Mid, Side | `placement`: Stereo, Left, Right, Mid, Side | Plan wins: all five in the Placement select and submenu |
| C-19 | Gainless shapes in the panel | A Low Cut band shows a Gain knob reading "0.0 dB" | Gain applies only to Bell, shelves and tilts (`shapeHasGain`, area 15 R8) | Plan wins: the Gain knob is hidden for gainless shapes (panels §2.3) |
| C-20 | Bypassed band in the sum | The Interactions board's summed curve still includes bypassed band 2 | The GUI curve matches the DSP (area 12 R12); a disabled band does not process | The summed curve excludes disabled bands |
| C-21 | Dyn Auto | Threshold shows "Auto"; Attack and Release show "50 %" at full contrast | Area 06: Dyn Auto keeps the stored knob values and greys Threshold, Attack and Release | Plan wins: the three knobs are greyed in Auto. A separate Auto chip turns it on and off (P-9) |
| C-22 | EQ Match captures | One shared "Capturing" toggle and one Reset | Area 17: Start/Stop and Reset per capture; counted seconds and ready state per capture | Plan wins: a record toggle and reset per capture row; seconds shown (P-16) |
| C-23 | EQ Match flow | Apply and Cancel only; sliders before Apply | Area 16: Match (fit) enabled when ready, shows progress, can be cancelled; sliders update the applied bands live after Apply, merging into its undo step | One primary button that steps Match → Stop (while fitting) → Apply → Done (P-17) |
| C-24 | EQ Match averaging window default | "Last 10 s" | Area 18 R3: whole capture is the default | Board is demo state; default "Whole capture" |
| C-25 | A/B compare | A and B chips and Copy A to B in the top bar | Area 12 open question: A/B is not decided | Drawn as on the board, but marked pending a product decision. No state model is specified here |
| C-26 | Full screen and Help | Top-bar buttons | Not in the plan; plugin windows are host-owned | Help: **PROPOSED** P-24. Full screen: needs a decision (CLAP and VST3 offer no portable full-screen request); hidden until decided |
| C-27 | Analyzer peak hold | A "Peak hold" On/Off toggle | No parameter: the display follower always keeps a post peak hold (area 19) | A toggle needs a new non-automatable `analyzer.peakHold` (default On) or editor state. **PROPOSED** P-14 |
| C-28 | `analyzer.grab` | Not shown | A table parameter, default On | Not placed. A question for the designer (P-14) |
| C-29 | Value formats | "+4.5 dB", "Q 0.71", "32 Hz", "+0.0 dB", U+2212 minus | Area 10: one formatter set for host and editor; code prints "+4.50 dB", "0.707", "32.0 Hz" and an ASCII minus | Board formats adopted for display (panels §2.10). The shared formatters change to match, keeping the parser tolerant. A code follow-up |
| C-30 | Gain-reduction indicator | Not drawn | Area 12 R3: a live gain-reduction indicator in the dynamics section | **PROPOSED** P-10 |
| C-31 | Spectral section | Density knob and a "Spectral Tilt" chip | Area 12 R3 and area 07: density and a per-band tilt toggle | Agrees |
| C-32 | Settings entry point | The gear opens "Interface and analyzer settings" (the Analyzer board) | Area 12 R2: analyzer controls are part of the editor | Agrees. The gear and the status bar's Analyzer select open the same panels (§4.6) |

---

## Implementation status (2026-11-17)

The editor in `ui/src/editor.cpp` follows this spec. The region map and reflow live in `ui/editor_layout` and the value formats in `ui/display_format`; both are unit-tested (`tests/ui/test_layout_format.cpp`). `amq_plugin_gui_shot` saves screenshots of the real editor for review, and the real-window GUI test passes.

**Built:**

- The bars, with their collapse rules.
- The graph layers, in the §5.3 order.
- Nodes and range handles, with their states.
- Readouts, the add cursor and the lasso.
- The band panel in every band state, including wrapping, compact and strip modes.
- The band menu with submenus and the slope grid.
- The Shape and Placement menus, and the slope picker.
- The Processing, Output, Analyzer and Interface panels, and the side-chain filter popover.
- The band list and the analyzer legend.
- The inline value editor, for band nodes and knob values.
- Hint mode.
- EQ Match mode: panel, layers, fitted nodes, and the Match → Stop → Apply → Done sequence. The panel wraps into two rows when the window is narrow.
- The spec's wheel bindings (Ctrl, Alt, Ctrl+Alt, Shift fine) and band-menu order.

**Not built yet, or built differently:**

- **Font.** Inter isn't bundled (bundling it is a licence and download decision). The editor uses the spec's first fallback, Segoe UI from the system on Windows, and the embedded Lato elsewhere.
- **Interface scale (P-7).** Done 2026-11-18 through a local Visage patch (`cmake/patches/visage-user-scale.patch`, applied by FetchContent): `visage::Window` carries a user scale that it multiplies into every OS DPI scale it sets, so the zoom survives Visage's own DPI updates. A first attempt that overrode the window's DPI from outside fought Visage's reset on every `WM_SIZE` and ran away; the patch removes that fight. The zoom is saved with the state; the minimum window size grows with it.
- **Peak hold.** Done 2026-11-17: `analyzer.peakHold` (non-automatable, default On) drives the toggle and the layer (C-27).
- **`analyzer.grab`** is not placed (C-28).
- **Presets.** Done 2026-11-18 (`ui/presets`, `PresetBridge` in the plugin). The button shows the current preset's name, with "(edited)" once a parameter changes after a load or save. Previous and next step through the list and wrap. The preset menu (P-28) has Save, Save As… (native save dialog), Revert, and the list with folders as submenus. Presets are `.amqpreset` files (the state chunk) in `%APPDATA%\Am-Q\Presets`; subfolders are categories; the built-in "Init" (all defaults) is always first. The host's state recall restores the preset name. The menu's search field is not built yet.
- **Strip mode.** Fixed 2026-11-18: the expanded strip is the compact panel (it had drawn with the strip's 44-tall frame, its knobs off the top). When the compact panel is taller than the graph (a wrapped dynamic or spectral band at the minimum size), it is tightened to fit, PROVISIONAL until the Product designer's call: it may reach 4 from the graph's top and bottom, padding becomes 8 tall, the wrap divider 8 then 6 from each row, and the dynamics knobs 32. The collapsed strip now shows the `freq · gain · Q` readout, and Escape collapses the expanded strip.
- **A/B and Full screen.** A/B is drawn but inactive, and Full screen is hidden. Both are pending a decision (C-25, C-26).
- **Tooltips.** Done 2026-11-17: a tooltip frame on top shows the hovered control's text after 600 ms; Attack and Release show their effective time.
- **Side-chain range in Band mode.** It displays one octave either side of the band frequency. This is provisional, until the engine exposes the derived range.
- **Collisions.** The hatch uses a provisional −60 dBFS activity floor.
- **Motion.** Done 2026-11-17 for the main transitions: menus and popovers fade in over 120 ms with a 4 px slide and out over 80 ms; handles grow 18 → 22 over 100 ms; band fills ease over 100 ms; Windows' "Show animations" off makes them instant. Hover colour changes are still instant.
- **Default size.** 1280×720, decided by the project owner on 2026-11-17 (C-1). The boards are 1280×760, so the default graph is 40 px shorter than on the boards.
- **Value formats.** Done 2026-11-17: the host-facing formatters in `core/params` use the spec's formats (C-29), and a test pins them to the editor's.

## PROPOSED items for the designer

Each item below is not shown on the boards, or changes what they show. It is designed in Am-Q's tokens and needs the Product designer to confirm.

| # | Item | Where |
| --- | --- | --- |
| P-1 | Top-bar collapse order and overflow menu | §1.2 |
| P-2 | Status-bar collapse order | §1.2 |
| P-3 | Frequency label thinning and minor gridlines | §1.2 |
| P-4 | Gain and analyzer grid step and label rules for every display and analyzer range | §1.2 |
| P-5 | Band panel wrapping, compact mode and strip mode | §1.2 |
| P-6 | Popover clamping and scrolling | §1.2 |
| P-7 | Interface scale as a user zoom, with window limits | §1.3 |
| P-8 | Band panel with no band selected, and with a multi-selection | panels §2.3 |
| P-9 | The Dyn Auto chip and greyed knobs | panels §2.3 |
| P-10 | The gain-reduction indicator (range-knob inner arc and live curve) | panels §2.3, graph §5.4 |
| P-11 | The side-chain filter popover (Band or Free, Low and High) | panels §2.3 |
| P-12 | Analyzer tilt as a readout with presets | panels §2.6 |
| P-13 | The display range selector (on the gain scale) | graph §5.2 |
| P-14 | Peak-hold setting storage; placement of Grab | panels §2.6 |
| P-15 | Band menu: Cut, Select All, mixed-state checks, single-band caption, Slope and Placement submenus | graph §4.1 |
| P-16 | EQ Match per-capture rows: record, reset, seconds, ready state, file progress, dropped-audio warning | panels §2.8 |
| P-17 | EQ Match primary button sequence (Match, Stop, Apply, Done) | panels §2.8 |
| P-18 | EQ Match residual and low-confidence drawing | graph §5.7 |
| P-19 | Solo state: handle badge, other bands dimmed, Solo Gain readout while dragging | panels §3, graph §5.4 |
| P-20 | No double-click variants in v1 | §6.1 |
| P-21 | Wheel: Slope on Ctrl+Alt, Shift fine | §6.2 |
| P-22 | Horizontal wheel pans the zoomed span | §6.2 |
| P-23 | Enter toggles Enabled in the band list | §6.3 |
| P-24 | Help toggles hint mode in the status bar | §4.13 |
| P-25 | All motion timings | §7 |
| P-26 | The inline value editor (band node and knob values) | graph §4.9 |
| P-27 | Pressed, hover-on-handle, focus-on-handle, dragging and disabled styles not drawn | panels §3 |
| P-28 | Preset menu contents | graph §4.10 |
| P-29 | Meter scale, clip LEDs and peak-hold behaviour | graph §5.5 |
| P-30 | Clip state of a band (band-2 hue while its scaled gain is clamped) and the gain knob over-range wedge | panels §3 |
| P-31 | Footer analyzer summary strings | panels §2.5 |
| P-32 | Number grouping (comma thousands separator) and "−∞ dB" display | panels §2.10 |
