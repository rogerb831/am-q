# Am-Q editor spec: overlays and the graph

This file is part of the [Am-Q editor specification](editor-spec.md), which covers the tokens, the region map, the conflicts (C-n) and the PROPOSED list (P-n). Components and states are in [editor-spec-panels.md](editor-spec-panels.md).

Coordinates are graph-local unless noted: the origin is the graph region's top-left, which is board y 44. Positions taken from the boards are written as "board: x, y", at the 1280×760 reference size.

---

## 4 Overlays and z-order

### 4.0 Z-order and general rules

These are the layers from bottom to top. Within one layer, later items draw on top.

| Z | Layer | Contents |
| --- | --- | --- |
| 0 | Ground | surface-100 |
| 1 | Graph content | Grid, analyzer, band fills and strokes, live curves, summed curve, axis labels, meters (order in §5.3) |
| 2 | Graph handles | Range stems and range handles, then band handles. The selected and focused handles draw last |
| 3 | Graph readouts | Selected-band readout, hover readout, add cursor, lasso |
| 4 | Docked panels | Band panel or EQ Match panel; analyzer or Match legend |
| 5 | Bars | Top bar and status bar (the board gives the top bar `z-index: 3`) |
| 6 | Popovers | Band list, Processing, Output, Analyzer and Interface panels, slope picker, side-chain filter popover, preset menu |
| 7 | Menus | Band menu and its submenus, the Shape menu, select menus. A submenu sits above its parent |
| 8 | Inline value editor | |
| 9 | Tooltips | Readout-chip style with the caption style as a second line. 600 ms hover delay, P-25 |

**General rules** (**PROPOSED** where the boards are silent, P-6):

- **One overlay family at a time.** These open together:
  - a menu with its submenus;
  - the Analyzer and Interface panels as a pair;
  - a popover with menus opened from inside it.

  Opening any other overlay closes the open one first.
- **Placement.** An anchored overlay opens on its preferred side. If it does not fit inside the window with a 16 px margin, it flips to the opposite side. If it still does not fit, it is shifted along the edge. Its size is never reduced below the board width, except by the clamp in main §1.2.
- **Dismissal.**
  - Escape closes the innermost overlay.
  - A click outside every open overlay closes them. That click is consumed and does not reach the graph.
  - Choosing a command item closes the menu chain. Choosing a checkbox or radio item also closes it (board items are single-shot).
  - Pressing the invoking control again closes the overlay it opened.
  - The overlays also close when the window loses focus, except the band list and EQ Match mode, which persist.
- **Focus.** Opening an overlay with the keyboard moves focus to its first enabled item. Opening it with the pointer moves focus to the overlay but highlights no item. Closing returns focus to the invoking control. Arrow keys move inside menus. → opens a submenu and ← closes it.
- **Modality.** No overlay is modal. The audio and host keep running, and keys that the overlay does not use pass to the host as usual (area 15).

### 4.1 Band menu (right-click)

The band menu is a menu 196 wide with padding 6 (board: at 892, 56 in the Interactions board, which shows two bands).

- **Opens on** right-click on a band node, or anywhere on the plot while a selection exists. Its targets are the selection, or the clicked band if that band is not selected (area 13 R8, area 14).
- **Anchor.** The top-left corner sits at the pointer. It flips left or up to fit.
- **Caption.** A caption row, padding 6 10, knob-label style, ink-muted:
  - several bands: "2 BANDS · #3, #4";
  - one band: "BAND #3 · BELL" (P-15);
  - more than 4 bands: the band list is truncated after 4 with "…".
- **Items.** Each item is 30 tall, with a 16 px icon, then the label, then the shortcut hint (ink-muted, 12 px, at the right). The order below is the board's, with the plan's additions marked (C-16, P-15):

| Group | Item | Kind | Icon | Hint | Action |
| --- | --- | --- | --- | --- | --- |
| 1 | Enabled | Checkbox (mixed state allowed) | Check (accent) when checked | "Alt-click" | Enable or disable the targets |
| 1 | Invert Gain | Command | Invert Gain | | Negate gain (and range) |
| 1 | Make Dynamic | Checkbox | Make Dynamic | | Checked when every target is dynamic. Choosing it while checked clears dynamics |
| 1 | Make Spectral | Checkbox | Make Spectral | | As above, for spectral (turning it on also makes the band dynamic) |
| — | separator | | | | |
| 2 | Shape | Submenu | Bell glyph | "▸" | §4.2 |
| 2 | Slope | Submenu | Slope | "▸" | The slope grid (§4.3) as a submenu panel |
| 2 | Placement | Submenu | Two rings | "▸" | Radio items Stereo, Left, Right, Mid, Side (C-18) |
| — | separator | | | | |
| 3 | Cut (plan addition) | Command | Scissors: circles (4,12) r 2 and (12,12) r 2, `M5.5 10.5 L12 2 M10.5 10.5 L4 2` | "Ctrl X" | Copy then delete, one undo step |
| 3 | Copy | Command | Copy | "Ctrl C" | |
| 3 | Paste | Command, disabled when the clipboard holds no Am-Q bands | Paste | "Ctrl V" | |
| 3 | Select All (plan addition) | Command | none | "Ctrl A" | |
| — | separator | | | | |
| 4 | Split | Command, **disabled** until defined (area 14 open question) | Split | | |
| 4 | Delete | Command | × | "Del" | One undo step |

The board uses the scissors-like Split glyph. Cut and Split need distinct icons, so the designer chooses one (P-15).

- **Dismissal** follows §4.0. Right-clicking elsewhere on the plot reopens the menu at the new point, for the new targets.

### 4.2 Shape menu

The Shape menu is a submenu 176 wide with padding 6 (board: at 1092, 196).

- **Anchor.** Its left edge sits 4 px to the right of the parent menu's right edge, and its top is aligned so the first item lines up with the Shape item (parent item top − 6). It flips to the left if it does not fit.
- **Items** are radio items, each with its shape glyph, in this order: Bell, Low Shelf, Low Cut, High Shelf, High Cut, Notch, Band Pass, Tilt Shelf, Flat Tilt, All Pass. This is the `dsp::Shape` id order, area 15 R7.
- **Checked state.** The current shape's glyph is stroked in accent. With mixed targets, nothing is checked.
- The **same menu** opens below the band panel's Shape select, or above it when the select is near the bottom, which is the usual case. It is anchored to the select's left edge, with an 8 px gap.

### 4.3 Slope picker

The slope picker is a panel 236 wide, with padding 10 and gap 6 (board: at 292, 432).

- **Opens from** the band panel's Slope select. It is placed above the select, left-aligned to it, with an 8 px gap, and flips below if needed. As the Slope submenu (§4.1), it is anchored like the Shape menu.
- **Contents:**
  - the caption "SLOPE · BAND N" (knob-label style, ink-muted, padding 2 4);
  - a 3-column grid of 28-tall options with gap 2: 6, 12, 18 / 24, 30, 36 / 48, 72, 96;
  - a full-width "Brickwall" option on surface-300;
  - the caption "dB/oct. Brickwall on Low and High Cut only." (caption style, ink-muted, padding 0 4).
- **States** are in panels §3.1. Options the shape does not allow are disabled (`dsp::slopeAllowed`).
- **Dismissal.** Choosing an option writes it (one gesture) and closes the picker. Otherwise §4.0 applies. The arrow keys move through the grid.

### 4.4 Side-chain filter popover (PROPOSED P-11)

- **Opens from** the SIDE CHAIN column's filter select. It is a panel 220 wide, with padding 12 and gap 8, placed above the select and left-aligned to it.
- **Contents:**
  - a segmented control "Band | Free" (`bandNN.scMode`);
  - in Band mode, the caption "Follows the band: 120–270 Hz";
  - in Free mode, two readout rows, "Low" and "High" (`scLow`, `scHigh`, *fmt-Hz*), each draggable and inline-editable.
- **On the graph.** While the popover is open, the side-chain range is drawn as a vertical span over the plot. The span is filled with analyzer-sc at 0.12 opacity, and its edges are 1 px analyzer-sc lines. In Free mode the edges can be dragged, and a drag is one gesture.
- **Dismissal:** §4.0.

### 4.5 Band list

The band list is a panel 260 wide (board: at 16, 16 in the graph). Its contents are in panels §2.4.

- **Opens from** the status bar's Bands select. It is docked at the graph's top-left (16, 16), as the board shows, not next to its invoker.
- **Height** fits its rows, up to graph height − 32. Beyond that it scrolls.
- **Non-dismissing.** Clicking band nodes in the graph keeps the list open, and the selection is shared both ways.
- **Closes on** Escape (when focus is in the list), on the Bands select, or on a click on empty plot. A click on empty plot closes the list and also clears the selection, which is the normal empty-click behaviour.

### 4.6 Analyzer and Interface panels

Both panels are bottom-anchored: their bottom edge sits 8 px above the status bar.

- The **ANALYZER panel** is 360 wide (board: left 160).
- The **INTERFACE panel** is 300 wide (board: right 24).

How they open:

- **From the status bar's Analyzer select**, only the Analyzer panel opens. Its left edge is aligned to the select's left edge. The board's 160 is close to that edge at 1280 px.
- **From the top bar's Settings button**, both panels open (Analyzer board), at the same positions.
- **While the Analyzer panel is open**, the analyzer legend (§5.3) is shown at graph-local (24, 24).

Dismissal follows §4.0.

### 4.7 Processing menu

The Processing menu is a panel 340 wide with padding 8 (board: left 160, bottom 8 above the status bar). Its contents are in panels §2.5.

- **Opens from** the status bar's Processing select, upward. Its left edge is aligned to the select's left edge.
- **Choosing a mode** writes `global.mode` (one gesture), and the menu stays open. This lets the user see the latency update and choose a resolution.
- **Closes on** Escape, an outside click or the select. Changing the resolution segments also keeps it open.

### 4.8 Output menu

The Output menu is a panel 320 wide with padding 16 (board: right 24, bottom 8). Its contents are in panels §2.7.

- **Opens from** the status bar's pan mode chip, upward, right-aligned to the window edge minus 24. It stays open while its controls are used.
- **Dismissal** follows §4.0.

### 4.9 Inline value editor (PROPOSED P-26)

Area 13 requires that double-clicking a band opens Am-Q's inline editor, not a separate popup.

- **On a band node.** Double-clicking a node replaces the selected-band readout chip, at the same anchor, with an editor chip:
  - surface-200 fill, a 1 px line-hairline ring, radius-sm, padding 3 8;
  - three fields, Freq, Gain and Q (Gain only for gain shapes, Q only for shapes that use Q);
  - each field is surface-000 with radius 2, readout style, at least 64 wide, with a 2 px accent focus ring.

  The Freq field starts focused, with its text selected.
- **On a value.** The same single-field editor opens:
  - when you click a knob's value text;
  - when you double-click an editable readout chip (Output Level, Pan, Solo Gain, Tilt, Gain Scale).

  It sits in place of the text, the same size.
- **Parsing.** Text is parsed by the shared parsers (area 10). The editor accepts "1.2k", "1200", "1.2 kHz", "-3", "−3" and "+3 dB".
- **Commit.** Enter commits, as one gesture per changed field. Tab commits and moves to the next field. A click outside commits.
- **Cancel.** Escape cancels.
- **Invalid text.** The field's ring turns signal-clip, and the value is not written until the text is fixed or cancelled.
- **Keyboard.** While a field has focus, it takes every key (area 15 R4).
- **Double-click on knobs.** Double-clicking a knob (not its value text) resets the knob to its default. This matches the current value boxes.

### 4.10 Preset menu (PROPOSED P-28)

- **Opens from** the preset name button, downward, centred under it. It is a menu 280 wide.
- **Contents:**
  - a search field (focused text field, area 15 R4);
  - "Save", "Save As…", "Revert";
  - a separator;
  - the preset list, grouped by folder as submenus, with the current preset checked.

The preset system itself is area 21's. This menu only presents it. Built 2026-11-18, the search field included (editor-spec §8 Presets).

### 4.11 EQ Match mode

EQ Match mode is a mode, not a popover.

- **Entry:** the top bar's EQ Match button.
- **Exit:** Cancel, Done, the EQ Match button, or Escape when no overlay is open.

While it is on:

- the band panel is hidden;
- the graph shows the Match layers (§5.7);
- the bars switch to their Match variants (panels §2.1–2.2);
- the EQ Match panel sits where the band panel sits (centred, bottom 32);
- the Match legend sits at (24, 24);
- graph gestures are disabled except the frequency zoom (P-17). Fitted nodes are display-only and show a tooltip, for example "Fitted band 1, Low Shelf".

### 4.12 Hover readout and add cursor

- **Shown when** the pointer is over the plot with no node, range handle or overlay under it, and no drag is active.
- **Add cursor:** a dashed circle at the pointer (panels §3.2).
- **Readout chip.** Its top-left sits at pointer + (16, −32) (board: crosshair at 560, 200; chip at 576, 168). It holds "Double-click to add", "·", the frequency (*fmt-Hz*), "·" and the gain (*fmt-dB*) under the pointer, with the "·" separators in ink-muted and a 1 px line-hairline ring.
- **Placement.** It flips to the left of the pointer if it would cross the plot's right edge minus 8, and below the pointer if it would cross the plot's top.
- **Hidden** when the pointer leaves the plot or a band reaches the 24-band limit. In that case the readout says "24 bands in use" in ink-muted (P-27).

### 4.13 Hint mode (PROPOSED P-24)

The Interactions board's status bar shows gesture hints instead of controls. This spec makes that a toggle:

- **Toggle:** the top bar's Help button turns hint mode on and off (the button is on per panels §3.1).
- **While on,** the status bar shows the hint strings from main §6.4, with the context-specific left text: "N bands selected · …" with a multi-selection, otherwise "Double-click to add a band · Right-click for the band menu".
- **Turns off** with Escape or the Help button.

The controls return when hint mode is off.

---

## 5 The graph

### 5.1 Axes

- **Frequency (x).** Logarithmic. The full span is 10 Hz to 30 kHz: the band frequency range, which the board's 1180 px plot matches exactly (20 Hz at 102, 1 kHz at 679, 20 kHz at 1120).
  - x = plotLeft + plotWidth · log10(f / fLo) / log10(fHi / fLo).
  - The wheel zooms the span about the pointer, within 10 Hz–30 kHz (main §6.2).
- **Gain (y).** Linear dB, centred on 0 dB at the plot's vertical middle.
  - y = plotTop + plotHeight/2 − gain · plotHeight / (2 · R), where R is `display.range`.
  - The default R is 12 dB. The boards show 18, which is not a choice (C-11).
- **Analyzer level (y, independent).** Linear dBFS, with 0 dBFS at plotTop and −`analyzer.range` at plotBottom.
  - y = plotTop + (−L / range) · plotHeight.
  - The default range is 90 dB (board: −10 at y 93.3).

### 5.2 Gridlines and labels

- **Frequency gridlines.** Vertical 1 px line-grid lines from plotTop to plotBottom, at 20, 50, 100, 200, 500, 1k, 2k, 5k, 10k and 20k. Minor lines and thinning follow main §1.2.
- **Frequency labels.** *fmt-Hz-axis*, in scale style, ink-axis, centred on the line, with the baseline at plotBottom + 16.
- **Gain gridlines.** Horizontal 1 px line-grid lines from x 0 to plotRight (not into the gutter), every R/6 dB from −R to +R. The 0 dB line is drawn separately in line-zero, across the plot width.
  - At R = 18 (board) that gives 3 dB lines, labelled every 6 dB.
  - At the default R = 12: 2 dB lines, labelled ±4, ±8, ±12 and 0.
- **Gain labels.** *fmt-dB-axis*, scale style, **accent**, right-aligned at plotRight + 30, with the baseline at line y + 4.
- **Analyzer labels.** Scale style, ink-axis, right-aligned at plotRight + 66 (board: 1246), every 10 dB from −10 down to −(range − 10), baseline at y + 4. When EQ curves are hidden (no band in use, or in the Analyzer board's illustration), the analyzer labels move to plotRight + 42 (board: 1222). The horizontal grid always follows the gain scale, not the analyzer.
- **Display range selector** (P-13). Clicking the gain-scale label column opens a select menu with ±3, ±6, ±12 and ±30 dB (`display.range`). The column shows a hover state: ink-muted labels become accent-hover, and the cursor is a pointer. The same setting appears in the Interface panel.

### 5.3 Drawing order and styles

This is the order inside layer 1, from bottom to top (Main and Analyzer boards):

| # | Layer | Style | Shown when |
| --- | --- | --- | --- |
| 1 | Grid | line-grid, 1 px | Always |
| 2 | Pre spectrum | Solid fill, analyzer-pre, from the spectrum down to plotBottom | `analyzer.showPre` |
| 3 | Collisions | Hatch pattern: 6×6 tile rotated 45°, a 2-wide stripe of signal-clip at 0.55 opacity. Fills the area under the lower of the pre and side-chain spectra wherever both are active (area 19 collisions layer) | `analyzer.collisions` and the side chain present |
| 4 | Peak hold | ink-muted, 1 px, dash 1 3. The held maximum of the post spectrum (area 19) | Peak hold on (C-27) and the post spectrum shown |
| 5 | Post spectrum | analyzer-post, 1 px line | `analyzer.showPost` |
| 6 | Side-chain spectrum | analyzer-sc, 1 px, dash 2 2 | `analyzer.showExternal` |
| 7 | 0 dB line | line-zero, 1 px, across the plot | Always |
| 8 | Band fills | For each used band in index order, the area between its own response curve and 0 dB, filled with its band colour. Opacity: 0.38 normally; 0.55 when selected; 0.18 for unselected bands while any band is dragged; × 0.35 when bypassed | Each used band |
| 9 | Band strokes | Band colour, 1.5 px, same order. × 0.35 when bypassed | Each used band |
| 10 | Live dynamic curves (P-10) | For each dynamic band whose current gain change is non-zero: the band's response at gain + current change, stroked in its band colour, 1 px, dash 3 2, 0.8 opacity | Dynamic bands with signal |
| 11 | Summed curve | accent, 2 px. The combined static response of all enabled bands after Gain Scale. Disabled bands are excluded (C-20) | At least one band used |
| 12 | Axis labels | §5.2 | Always |
| 13 | Meters | §5.5 | Always |

All curves are clipped to the plot rectangle. They are sampled per device pixel column, and fills close along 0 dB, or along plotBottom for spectra. Curve values come from the shared design code (area 12 R12, 0.1 dB).

**Analyzer legend.** It is shown while the Analyzer panel is open. It is a panel at (24, 24) with padding 10 14, gap 16, label type, max width 560, and it wraps. Each entry has a swatch, an 8 px gap and the name:

| Entry | Swatch |
| --- | --- |
| Pre | 16×10 analyzer-pre with a 1 px analyzer-post ring |
| Post | 20×8, 1.5 analyzer-post line |
| Side chain | 1.5 analyzer-sc, dash 2 2 |
| Peak hold | 1.5 ink-muted, dash 1 3 |
| Collision | 16×10 signal-clip at 0.35 |

### 5.4 Nodes and handles

- **Band node.** Its centre is at (x(freq), y(gain)) for gain shapes. For gainless shapes it sits on the 0 dB line at the band frequency. The board places the Low Cut node at about −3 dB, at the curve's knee. This spec puts gainless nodes at 0 dB, where the vertical drag maps to Q (P-27). Size and states are in panels §3.2. The hit target is a 24 px circle, which wins over curves and the empty plot.
- **Range handle** (dynamic bands with a gain shape and non-zero range).
  - Its centre is at (x(freq), y(gain + dynRange)), using gains after Gain Scale.
  - The stem is a 1 px dashed line in the band colour, from the node centre to the handle.
  - The hit target is 20 px.
  - It is drawn below the band nodes (Main board order).
- **Solo** (P-19). The soloed band's node gets the signal-solo badge. While its vertical drag sets Solo Gain, the readout reads "Solo Gain +x.x dB" (code; area 13).
- **Overlap.** When nodes overlap, the selected or focused node wins hit-testing, then the higher band index.

### 5.5 Meters (P-29)

There are two channels (L and R, or M and S in M/S pan mode), in the right gutter: x = plotRight + 78 and + 86, each 6 wide (size-meter), with a 2 gap. They run from plotTop to plotBottom.

- **Trough:** surface-000, radius 2.
- **Level:** output peak. The fill is meter-ok below −18 dBFS, meter-hot from −18 to −3 dBFS, and signal-clip above −3 dBFS, stacked in segments as the board draws them.
- **Scale:** linear in dB, from 0 dBFS at the top to −60 dBFS at the bottom. The board's level heights are illustrative and do not imply a scale.
- **Peak-hold tick:** 2 px, meter-hot. It holds for 1.5 s, then falls (main §7).
- **RMS mark** (from code): a 1 px ink-muted line across the channel.
- **Clip LED:** a 6×4 cap 4 px above each channel. It is surface-000 when idle and signal-clip after a sample above 0 dBFS. A click resets it.
- **Micro scale** (0, −12, −24, −48) is drawn only on hover, in micro type, ink-axis, left of the meters.

### 5.6 Readouts on the graph

- **Selected-band readout.**
  - **Anchor:** the chip's top-left sits at node + (18, −44) (board: `rx = hx + 18`, `ry = hy − 44`).
  - **Flipping:** it flips to node − (18 + chip width) when it would cross plotRight − 8. The board clamps x at 1000, which is the same rule at 1180. It flips below the node (+26) when it would cross plotTop.
  - **Contents:** an 8 px band dot, then the frequency, "·", the gain and "·" "Q x.xx", with the separators in ink-muted. For gainless shapes the gain part is left out. For Flat Tilt the Q part is left out.
  - **Style:** a readout chip with a 1 px line-hairline ring.
  - **Visibility:** shown for the focused band while anything is selected. During a range drag it reads "Range +x.x dB". During a solo drag it reads "Solo Gain +x.x dB".
- **Hover readout:** §4.12.
- **Over the panel.** Readouts never draw under the band panel. If the anchor falls inside the panel's rectangle, the chip moves above the panel's top edge, keeping the same x.

### 5.7 EQ Match layers (Match board)

These replace layers 2–11 of §5.3 while EQ Match mode is on. The grid, the 0 dB line and the gain labels stay. There are no analyzer labels and no meters (board).

| # | Layer | Style | Scale |
| --- | --- | --- | --- |
| 1 | Input spectrum | analyzer-post, 1.5 px | Analyzer dBFS |
| 2 | Reference spectrum | analyzer-sc, 1.5 px, solid. In the analyzer it is dashed; here it is solid, per the board | Analyzer dBFS |
| 3 | Difference | ink, 1 px, dash 4 3. Smoothed, and level-aligned when alignment is on. Points with low confidence (fit weight below 0.5, area 20 R5) are drawn at 0.35 opacity (P-18) | Gain dB |
| 4 | Residual (P-18) | ink-muted, 1 px, dash 1 2, after a fit. Its legend entry is "Residual" | Gain dB |
| 5 | Match (fitted response) | accent, 2 px. The response of the applied bands at the current Amount and Bands settings | Gain dB |
| 6 | Fitted nodes | 18 px in band colours by fit order (band-1, band-2, …), with shadow-handle. Bands beyond the Bands slider are drawn as a transparent fill with a 2 px inset band-colour ring at 0.6 opacity (board, C-17). Display only | Gain dB |

The spectra are drawn only while a capture has data. Before any capture, the plot shows the caption "Start a capture on both sources, or load a reference file." centred in ink-muted (P-16).
