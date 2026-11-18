# Am-Q editor spec: components and states

This is part of the [Am-Q editor specification](editor-spec.md), which covers tokens, primitives, the region map, the conflicts table (C-n) and the PROPOSED list (P-n). This file covers §2, the component inventory, and §3, the states.

Parameter ids are the string ids in `docs/params/parameter-table.md`. `bandNN.x` means the field of the focused band. Value formats are defined once, in §2.10, and referred to by name, for example *fmt-dB*.

---

## 2 Component inventory

### 2.1 Top bar

The top bar is 44 tall, surface-200, with padding 0 24. It has three clusters. The left and right clusters are space-between; the centre cluster sits at the absolute centre (see the main file, §1.2). In order:

| # | Component | Type, size | Tokens and label | Binds to | Format |
| --- | --- | --- | --- | --- | --- |
| 1 | Wordmark | Text + glyph, 30/32 | wordmark style, accent; `role=img`, name "Am-Q" | none | none |
| 2 | Separator | 1×20 | line-hairline; 24 either side (cluster gap) | none | none |
| 3 | Undo | Icon button 28 | Undo icon; tooltip "Undo (Ctrl+Z)" | `UndoController::undo` | none |
| 4 | Redo | Icon button 28, gap 4 | Redo icon; tooltip "Redo (Ctrl+Shift+Z)" | `UndoController::redo` | none |
| 5 | A, B | Chips 28, gap 4 | "A" on (accent fill), "B" off | A/B compare (C-25, pending a product decision) | none |
| 6 | Copy A to B | Icon button 28 | Arrow icon; name "Copy A to B" | A/B compare (C-25) | none |
| 7 | Previous preset | Icon button 28 | Chevron left | Preset system (area 21) | none |
| 8 | Preset button | Height 32, min-width 220, padding 0 16, gap 8, radius-md, surface-300 | The name in preset style (ink, centred), then "(edited)" in caption style, ink-muted, shown only when the state differs from the loaded preset | Opens the preset menu (graph file, §4.10) | Name as stored |
| 9 | Next preset | Icon button 28, gap 4 | Chevron right | Preset system | none |
| 10 | EQ Match | Height 32, padding 0 12, gap 8, radius-md, surface-300, label type | EQ Match glyph + "EQ Match" | Opens or closes EQ Match mode (§2.8) | none |
| 11 | Settings | Icon button 28, gap 8 | Gear; name "Interface and analyzer settings" | Opens the Analyzer and Interface panels (graph file, §4.6) | none |
| 12 | Full screen | Icon button 28 | Hidden until decided (C-26) | none | none |
| 13 | Help | Icon button 28 | Help icon | Toggles hint mode (graph file, §4.13, P-24) | none |

There are other top-bar variants:

- **EQ Match mode.** Only the wordmark, the preset name with "(edited)", and the EQ Match button are shown. The EQ Match button is filled with accent, with accent-ink text and font weight 600, and is marked as the current page (`aria-current`). Clicking it leaves EQ Match mode, like Cancel does.
- **Analyzer frozen.** While `analyzer.freeze` is on, a readout chip "Frozen" (accent fill, accent-ink text) sits at the far right of the top bar, before the Settings button. That is where the Analyzer board shows it. Clicking it unfreezes the analyzer.

### 2.2 Status bar

The status bar is 44 tall, surface-200, with padding 0 24 and a gap of 16 inside each group. Its type is label style. The groups are left and right, space-between.

**Left group**

| # | Component | Type, size | Tokens and label | Binds to | Format |
| --- | --- | --- | --- | --- | --- |
| 1 | Bands | Select, 28, no chevron | "Bands" ink, then "5 / 24" ink-muted | Opens the band list (graph file, §4.5) | used count, then " / 24" |
| 2 | Separator | 1×20 | line-hairline | none | none |
| 3 | Processing | Select, 28, chevron pointing up | Mode name (ink), then the latency (ink-muted) | `global.mode`; opens the Processing menu (graph file, §4.7) | *fmt-latency* of `latencySamples(mode, resolution, rate, spectralActive)` |
| 4 | Analyzer | Select, 28, no chevron when closed | "Analyzer" ink-muted, then the summary in ink | Opens the Analyzer panel | The summary (P-31): "Off"; "Pre"; "Post"; "SC"; or joined with " + ", for example "Pre + Post + SC" |
| 5 | Freeze | Icon button 28 | Snowflake; name "Freeze analyzer" | `analyzer.freeze` | On: accent icon on surface-300 (§3) |

**Right group**

| # | Component | Type, size | Tokens and label | Binds to | Format |
| --- | --- | --- | --- | --- | --- |
| 6 | Auto Gain | Chip 28 | "Auto Gain" | `global.autoGain` (default Off) | none |
| 7 | Gain Scale | Two texts, gap 8 | "Gain Scale" ink-muted, then the value in ink. Dragging the value changes it vertically (P-27); double-clicking opens the inline editor | `global.gainScale` | *fmt-pct* |
| 8 | Separator | 1×20 | line-hairline | none | none |
| 9 | "Output" | Text | ink-muted | none | none |
| 10 | Pan mode | Chip 28 | "L/R" or "M/S"; name "Output pan mode" | Opens the Output menu (graph file, §4.8). The chip shows `global.panMode` | Choice name |
| 11 | Output level | Readout chip, accent variant | For example "+0.0 dB". Dragging changes it (vertical, Shift fine); double-clicking opens the inline editor | `global.outputLevel` | *fmt-dB*, silence as *fmt-inf* |
| 12 | Invert | Icon button 28 | Name "Invert polarity" | `global.invert` | On-state per §3 |
| 13 | Bypass | Icon button 28 | Power icon; name "Bypass plugin" | `global.bypass` | On-state per §3 |

Other status-bar variants:

- **EQ Match mode.** The left side shows "Fitting bands", a 160×6 progress track (surface-000, pill) with an accent fill, and "60 %" in ink-muted, gap 12. These appear only while a fit runs. Otherwise it shows the ready state from §2.8. The right side shows the caption "Power average · frames below −90 dBFS skipped · Apply = one undo step" in ink-muted.
- **Hint mode** (P-24): see the graph file, §4.13.

### 2.3 Band panel

The panel is a floating panel: surface-200, radius-lg, shadow-panel, padding 16 20, gap 16. It is centred horizontally, 32 above the graph's bottom edge. It holds one row of columns with gap 24, aligned to centre, and all knobs are bottom-aligned.

**Width and height** at the default density:

| Band state | Width | Height |
| --- | --- | --- |
| Static | 632 | 166 |
| Dynamic | 1112 | 166 |
| Dynamic + Spectral | 1180 | 166 |

Each knob's column is as wide as the knob or its caption, whichever is wider, as the board's flex columns are: THRESHOLD needs 72, RELEASE and DENSITY 56 (`EditorLayout::knobSlot`). That makes the dynamic widths 1112 and 1180 rather than the 1080 and 1140 a fixed 48 px column would give.

The height of 166 comes from the 88 Gain knob, its 14 label, its 16 value and two 8 gaps, plus padding.

**Column A: identity** (148 wide, gap 8)

| # | Component | Type, size | Tokens and label | Binds to | Format |
| --- | --- | --- | --- | --- | --- |
| A1 | Band dot | 10×10, pill | The band colour | none | none |
| A2 | Title | Text | "Band #N", 13/16 600 ink | Band index | N without padding |
| A3 | Enabled | Icon button 28, pushed right | Power icon; name "Band enabled"; tooltip "Enable/disable band (Alt-click the handle)" | `bandNN.enabled` | On: ink icon. Off: ink-disabled icon (§3) |
| A4 | Shape | Select 32, width 148 | Shape glyph (16, ink) + shape name | `bandNN.shape`, through `BandOps::setShape`; opens the Shape menu (graph file, §4.2) | Shape name |
| A5 | Slope | Select 32, width 148 | Slope text | `bandNN.slope`; opens the slope picker (graph file, §4.3). Hidden for shapes without slope (Flat Tilt) | *fmt-slope* |

**Column B: main knobs** (gap 16, bottom-aligned)

| # | Knob | Size | Arc | Label | Binds to | Format | Shown when |
| --- | --- | --- | --- | --- | --- | --- | --- |
| B1 | Freq | 64 (size-knob-sm) | Unipolar from −135° | "FREQ" | `bandNN.freq` | *fmt-Hz* | Always |
| B2 | Gain | 88 (size-knob-lg) | Bipolar from 0° (top) | "GAIN" | `bandNN.gain` | *fmt-dB* | `shapeHasGain` (Bell, Low and High Shelf, Tilt Shelf, Flat Tilt). Hidden otherwise (C-19), and the row then holds Freq and Q only |
| B3 | Q | 64 | Unipolar | "Q" | `bandNN.q` (normalised position n; Q = 0.025·1600ⁿ) | *fmt-Q* | `shapeUsesQ` (every shape except Flat Tilt) |

**Knob drawing.** For a knob of size s:

- **Geometry.** Centre c = s/2. Stroke width sw = 4 if s > 70, else 3. Radius r = c − sw/2 − 1. The sweep runs from −135° to +135°, measured clockwise from the top.
- **Face.** A circle of radius r − sw − 3, filled surface-300, with the shadow-knob drop part (0 4 10 rgba(0,0,0,.5)).
- **Track.** The full sweep, stroked line-hairline at width sw, with round caps.
- **Value arc.** Stroked accent at width sw, round caps. It runs from −135° to the value angle, or from 0° to the value angle for bipolar knobs. It is not drawn when shorter than 0.5°.
- **Tick.** A 2 px ink line, round cap, from 0.3·face radius to face radius − 4, at the value angle.
- **Over-range wedge.** Drawn in signal-clip (P-30); see §3.
- **Labels.** The label sits 8 below the knob in knob-label style, ink. The value sits 8 below that in readout style, accent.

**Dynamics group** (shown when `bandNN.dynEnabled` is on). It has padding-left 24 and a left divider (a 1 px line-hairline inset edge running the group's height). Its knobs are bottom-aligned with gap 12.

| # | Component | Size | Arc | Label | Binds to | Format | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| D0 | Auto | Chip 28, at the group's top-left, above the knobs (the knobs are 40 shorter than column B, which leaves room) | none | "Auto" | `bandNN.dynAuto` (default On) | none | **PROPOSED** P-9 (C-21) |
| D1 | Range | 48 | Bipolar | "RANGE" | `bandNN.dynRange` | *fmt-dB* | Its inner arc shows the live gain change (P-10) |
| D2 | Threshold | 48 | Unipolar | "THRESHOLD" | `bandNN.threshold` | *fmt-dBFS*; shows "Auto" while Auto is on | Greyed in Auto (C-21) |
| D3 | Attack | 48 | Unipolar | "ATTACK" | `bandNN.attack` | *fmt-pct0* | Greyed in Auto |
| D4 | Release | 48 | Unipolar | "RELEASE" | `bandNN.release` | *fmt-pct0* | Greyed in Auto |
| D5 | Density | 48 | Unipolar | "DENSITY" | `bandNN.spectralDensity` | *fmt-pct0* | Only while `bandNN.spectral` is on |

**SIDE CHAIN column** (148 wide, gap 8). It is shown with the dynamics group and sits to its right.

| # | Component | Type | Label | Binds to | Format |
| --- | --- | --- | --- | --- | --- |
| S1 | Caption | Text | "SIDE CHAIN", knob-label style, ink-muted | none | none |
| S2 | Int / Ext | Two chips 28, flexible width, gap 4 | "Int" and "Ext"; the active one is on | `bandNN.scExternal` (Int = Off) | none |
| S3 | Listen | Icon button 28 | Headphones; name "Listen to side chain"; tooltip "Listen" | `bandNN.scListen` | On: per §3 (signal-solo dot + label) |
| S4 | SC filter | Select 28, width 148 | Mode name in ink, then the range in 12 px ink-muted; for example "Band  120–270 Hz" | `bandNN.scMode`, `bandNN.scLow`, `bandNN.scHigh`; opens the side-chain filter popover (P-11, graph file, §4.4) | *fmt-Hz-range*. In Band mode the range is the band's own derived range (read-only) |
| S5 | Spectral Tilt | Chip 28, width 148 | "Spectral Tilt" | `bandNN.spectralTilt` | Only while spectral |

**Column D: placement and modes** (148 wide, gap 8)

| # | Component | Type | Label | Binds to | Format |
| --- | --- | --- | --- | --- | --- |
| D-1 | Placement | Select 32, width 148 | Two-ring glyph + placement name | `bandNN.placement`; opens a menu of Stereo, Left, Right, Mid and Side (C-18) | Choice name |
| D-2 | Dynamic | Chip 32, flexible width | "Dynamic" | `bandNN.dynEnabled` | none |
| D-3 | Spectral | Chip 32, flexible width, gap 4 | "Spectral" | `bandNN.spectral`. Turning it on also turns Dynamic on (board behaviour). It shows as on only while the band is dynamic and spectral | none |
| D-4 | Solo | Icon button 28, right-aligned row, gap 4 | Headphones; name "Solo band" | `bandNN.solo` | On: per §3 |
| D-5 | Delete | Icon button 28 | ×; name "Delete band" | `BandOps::remove` (one undo step) | none |

**How the panel changes with the band's state**

| State | Panel |
| --- | --- |
| Gain shape, static | A, B (Freq, Gain, Q), D |
| Gainless shape (Low Cut, High Cut, Notch, Band Pass, All Pass) | A, B (Freq, Q), D. Width 632 − 88 − 16 = 528 |
| Flat Tilt | A without Slope; B (Freq, Gain); D |
| Dynamic | A, B, then the dynamics group (Auto, Range, Threshold, Attack, Release), SIDE CHAIN, D |
| Dynamic + Spectral | As Dynamic, plus Density in the dynamics group and Spectral Tilt in SIDE CHAIN |
| Dynamic on a gainless shape | Range has no meaning without gain (`dynRange` applies to gain shapes; the range handle is not drawn). The dynamics group shows Auto, Threshold, Attack and Release, with Range hidden (**PROPOSED**, part of P-8) |
| Bypassed band (`enabled` off) | The panel stays fully editable. The title and dot take opacity-bypassed, and the Enabled icon turns ink-disabled |
| Soloed band | The Solo button is on (§3). The panel is otherwise unchanged |
| **No band selected** (P-8) | The panel is hidden. If no band exists at all, the plot shows the hover readout "Double-click to add" at the pointer (graph file, §5.6) |
| **Multi-selection** (P-8) | The panel shows the focused band (the last one clicked). The title reads "Band #N" followed by "(multiple)" in caption style, ink-muted, which is the token's documented use. Panel edits apply to the focused band only. Graph drags, wheel, keys and the band menu apply to the whole selection |

**Gain-reduction indicator** (P-10, C-30). While the band is dynamic, the Range knob draws a second arc inside its track:

- Radius r − sw − 1, width 2.
- Colour: the band colour.
- It runs from 0° towards the current gain change in dB, on the same scale as Range.
- It is updated at the snapshot rate from the per-band gain-reduction array (area 14).

The graph shows the same value as the live curve (graph file, §5.4).

### 2.4 Band list (Interactions board)

See the graph file, §4.5, for anchoring. The list is a panel 260 wide, padding 8, gap 2.

| # | Component | Type | Tokens and label | Binds to | Format |
| --- | --- | --- | --- | --- | --- |
| L1 | Header | Row, padding 4 10 8, space-between | "BANDS" (knob-label style, ink-muted) and "5 of 24" (caption style, ink-muted) | Used count | "N of 24" |
| L2 | Band row (one per used band, in frequency order) | Button 36 tall, full width, padding 0 10, gap 10, radius-md | "#N" (micro type, ink-muted, 22 wide), a 10 px band dot, the shape name with state suffixes (label type), and the frequency (600, pushed right) | Selection and focus; `bandNN.enabled` | Name, then " · off" when disabled, " · Dyn" when dynamic, or " · Spec" when spectral (P-15 extends the board's "off" and "Dyn"). Frequency in *fmt-Hz* |
| L3 | Hint | Caption, padding 8 10 2, ink-muted | "↑ ↓ move · Shift adds · Enter enables · Del removes" (C-15) | none | none |

A disabled row has ink-muted text, and its dot is an outline (a 2 px inset ring in the band colour, transparent fill). Selected and focused rows are styled per §3.

### 2.5 Processing menu (Processing board)

The menu is a panel 340 wide, padding 8, gap 4. Anchoring is in the graph file, §4.7.

| # | Component | Type | Label | Binds to | Format |
| --- | --- | --- | --- | --- | --- |
| P1 | Caption | Text, padding 8 10 4 | "PROCESSING" | none | none |
| P2 | Zero Latency | Radio item: padding 8 10, gap 12, radius-md, label type. A 14 px radio ring (inset 1.5 ink-muted). The latency on the right in ink-muted | "Zero Latency" | `global.mode` = 0 | *fmt-latency* of `latencySamples(ZeroLatency, …)`: "0 smp", or the FFT latency while a spectral band is active |
| P3 | Natural Phase | Radio item with a two-line label | "Natural Phase" / "Analog-matched phase near Nyquist" (caption, ink-muted) | `global.mode` = 1 | "320 smp" (or more with spectral, C-5) |
| P4 | Linear Phase | Radio item with a two-line label | "Linear Phase" / "No phase shift, pre-ringing on steep cuts" | `global.mode` = 2 | *fmt-latency* at the current resolution |
| P5 | Resolution block | padding 8 10, gap 8 | "Resolution" (label type, ink-muted), with sub-caption "Linear Phase, and spectral bands in every mode" (C-6) | none | none |
| P6 | Resolution | Segmented control, five segments | "Low", "Med", "High", "V. High", "Max" | `global.resolution` | none |
| P7 | Latency caption | Caption, ink-muted | "Linear Phase latency: 2,112 · 4,160 · 8,256 · 16,448 · 65,600 smp", computed per resolution at the session rate | none | *fmt-latency* (C-4) |
| P8 | Divider | 1 px line-hairline, margin 4 0 | none | none | none |
| P9 | Spectral note | Caption, padding 4 10 | "While a spectral band is active, latency is at least the FFT latency at this resolution." (C-5) | none | none |

### 2.6 Analyzer panel and Interface panel (Analyzer board)

**ANALYZER panel.** 360 wide, padding 16, gap 10. Each row is space-between with gap 12, label type.

| # | Row | Control | Binds to | Format and notes |
| --- | --- | --- | --- | --- |
| Z1 | Caption "ANALYZER" | none | none | none |
| Z2 | "Show" | Three chips 28, gap 4: "Pre", "Post", "Side chain" | `analyzer.showPre`, `showPost`, `showExternal` (defaults On, On, Off) | none |
| Z3 | "Freeze" | Chip "On"/"Off" | `analyzer.freeze` | The chip label is the state |
| Z4 | "Peak hold" | Chip | No parameter exists (C-27). P-14: add `analyzer.peakHold`, non-automatable, default On | none |
| Z5 | "Collisions", with caption "Where input and side chain overlap" | Chip | `analyzer.collisions` (default Off) | none |
| Z6 | Divider | 1 px line-hairline | none | none |
| Z7 | "Resolution (FFT size)" caption, ink-muted | Segmented control: Low, Medium, High, Max (C-8). Below it, a caption with the actual size: "4096-point FFT at 48 kHz" | `analyzer.resolution` | Size from the rate-scaled table (area 19 R2) |
| Z8 | "Speed" caption, ink-muted | Segmented control: V. Slow, Slow, Medium, Fast, V. Fast (C-7) | `analyzer.speed` | none |
| Z9 | "Tilt" | A readout chip, drag to change, double-click for the inline editor, with a 12 px chevron that opens presets 0, 1.5, 3, 4.5 and 6 (P-12, C-9) | `analyzer.tilt` (0–6, default 4.5) | *fmt-dBoct* |
| Z10 | "Range" | Select 28: 60, 90 or 120 dB | `analyzer.range` (default 90) | "90 dB" |

`analyzer.grab` (default On) is not placed on the boards (C-28). P-14: if the designer keeps it, it becomes a row "Grab", with a chip, after Collisions.

**INTERFACE panel.** 300 wide, padding 16, gap 10.

| # | Row | Control | Binds to |
| --- | --- | --- | --- |
| I1 | Caption "INTERFACE" | none | none |
| I2 | "Scale" caption, ink-muted | Segmented control: 100 %, 150 %, 200 %, 300 % | Interface zoom (P-7, editor state) |
| I3 | Caption | "Drag the window corner for any size in between." | none |
| I4 | "Display range" (**PROPOSED** P-13 duplicate) | Select 28: ±3, ±6, ±12 or ±30 dB | `display.range` (default 12) |
| I5 | "Gain-Q coupling" (**PROPOSED**, area 13 R20; an open question whether it ships in v1) | Chip, default Off | `GraphInteraction::gainQCoupling` (editor state) |

### 2.7 Output menu (Processing board)

The menu is 320 wide, padding 16, gap 10. Each row is space-between, label type. Second lines are caption style, ink-muted.

| # | Row | Control | Binds to | Format |
| --- | --- | --- | --- | --- |
| O1 | Caption "OUTPUT" | none | none | none |
| O2 | "Pan Mode" | Segmented control, 140 wide: "L/R" and "M/S" | `global.panMode` | none |
| O3 | "Output Level" | Readout chip, accent variant; drag or inline edit | `global.outputLevel` | *fmt-dB* or *fmt-inf* |
| O4 | "Output Pan" | Readout chip; drag or inline edit | `global.outputPan` | *fmt-pan* (L/R or M/S by Pan Mode) |
| O5 | "Invert polarity" | Chip "Off"/"On" | `global.invert` | none |
| O6 | "Bypass" / "Smoothed, latency-compensated" | Chip | `global.bypass` | none |
| O7 | Divider | none | none | none |
| O8 | "Auto Gain" / "Keeps loudness level while you EQ" | Chip | `global.autoGain` | none |
| O9 | "Gain Scale", with its value on the right in accent 600 | Range slider (§2.9), 0–200 % (C-12) | `global.gainScale` | *fmt-pct* |
| O10 | Caption | "Scales every band's gain and dynamic range together." | none | none |
| O11 | "Solo Gain" / "Audition level while a band is soloed" (C-13) | Readout chip; drag or inline edit | `global.soloGain` (±20 dB) | *fmt-dB* |

### 2.8 EQ Match mode (Match board)

EQ Match mode replaces the band panel with the EQ Match panel, replaces the graph layers (graph file, §5.7), and switches the top and status bars to their EQ Match variants (§2.1, §2.2). The panel is built lazily the first time it opens (area 12 R2).

**Legend.** A panel at graph-local (24, 24), padding 10 14, gap 16, label type. It has four entries, each a 20×8 swatch and a name, gap 8:

| Entry | Swatch |
| --- | --- |
| Input | analyzer-post, 1.5 |
| Reference | analyzer-sc, 1.5 solid |
| Difference | ink, 1, dash 4 3 |
| Match | accent, 2 |

P-18 adds a fifth entry, Residual.

**Panel.** A floating panel, centred, 32 above the bottom, padding 16 20. Its columns are stretch-aligned with gap 24 and separated by 1 px line-hairline rules.

| Column | Width | Components |
| --- | --- | --- |
| **1 · REFERENCE** | 170 | Caption "1 · REFERENCE". A select 32: "Side-chain" or "Audio file…" (area 17 R2), where choosing a file opens the system file dialog. Caption "Or load an audio file. Another Am-Q instance: v2." With a file reference, the select shows the file name (ellipsized), and a 4 px accent progress line under it shows the analysis progress (area 17 R9, P-16) |
| **2 · CAPTURE** | 220 | Caption "2 · CAPTURE". Two capture rows (P-16, C-22), each: an 8 px dot (analyzer-post for Input, analyzer-sc for Reference), the name, the counted seconds right-aligned in ink-muted (for example "12.4 s"; frames in the tooltip, "1,284 frames"), a record icon button (a filled 8 px circle; on: signal-clip dot on surface-300), and a reset icon button (a ↺ arc). Not ready: the seconds read "Need 2 s" (area 17 R8 planning value) in ink-muted. Ready: a ✓ in accent before the seconds. Then a select 28, "Window" in ink-muted and the value in ink: "Whole capture" (default, C-24) or "Last N s" (N 1–60, area 18 R3). Dropped audio (area 17 R6): a caption "Audio dropped: N frames" in ink-muted with a 12 px warning triangle in meter-hot |
| **3 · ALIGN** | 170 | Caption "3 · ALIGN". Row "Level alignment" with a chip On/Off (area 20 R3, default On). Caption "Removes the loudness gap: −2.4 dB" (*fmt-dB* of the offset; "—" before both captures are ready). Select 28, "Smoothing" in ink-muted and the value: 1/3, 1/6 (default), 1/12 or 1/24 oct (area 20 R4) |
| **4 · FIT** | 240 | Caption "4 · FIT". Label row "Amount" with the value in accent 600 (*fmt-pct0*), above a 240-wide range slider, 0–100 % (area 16 R11). Label row "Bands to apply" with "4 of 5 fitted" in accent 600, above a range slider from 0 to the number of fitted bands (area 16, strongest first). Both sliders are disabled until a fit exists, and update the applied bands live after Apply (area 16 R13) |
| **Actions** | 120 | Buttons are bottom-aligned, gap 8, each 36 tall with radius-md and 600 13 type. The primary button is filled accent with accent-ink text and steps through the sequence below (P-17, C-23). The secondary button "Cancel" is surface-300 with ink text and leaves EQ Match mode. An applied result stays, as one undo step |

The primary button's sequence:

1. **Match.** Disabled (§3) until both captures are ready or a file reference is analysed (area 16 R15). Tooltip "Fit bands to the difference".
2. **Stop.** While fitting. Cancelling leaves the bands unchanged (area 16 R16).
3. **Apply.** Tooltip "Writes bands as one undo step".
4. **Done.** After Apply. It finishes the undo step and leaves EQ Match mode.

If the fit needs more bands than are free, a caption under the actions reads the code's message, "Not enough free bands: N needed, M free."

### 2.9 Range slider (PROPOSED drawing)

The boards use the browser's native range input with an accent colour, so the drawing below is **PROPOSED** (P-27). The track is 4 tall, a surface-000 pill. The filled part, from the minimum (or from 100 % for Gain Scale), is accent. The thumb is a 14 px circle in ink with shadow-handle. The hit height is 20. Shift-drag is fine (× 0.1), and double-clicking the thumb resets to the default. A drag is one gesture.

### 2.10 Value formats

These formats follow the boards (C-29). They use the Unicode minus sign U+2212 ("−") and tabular figures. Parsers accept "-", "−" and "+".

| Name | Rule | Examples |
| --- | --- | --- |
| *fmt-Hz* | Below 1 kHz: whole Hz, then " Hz". 1 to 9.99 kHz: two decimals, then " kHz". 10 kHz and up: one decimal, then " kHz". "k" is always written as "kHz" with a space before it, never "k" alone, except on axis labels | 32 Hz · 180 Hz · 1.82 kHz · 9.00 kHz · 14.5 kHz |
| *fmt-Hz-axis* | Gridline labels with no unit: 20, 50, 100, 200, 500, 1k, 2k, 5k, 10k, 20k. Lower-case k, no space | 1k |
| *fmt-Hz-range* | Two values sharing one unit, joined by an en dash ("–") with no spaces. Below 1 kHz: whole Hz. 1 kHz and up: kHz with one decimal below 10 kHz and none at 10 kHz or more | 20–45 Hz · 120–270 Hz · 0.9–3.6 kHz · 6.0–14 kHz |
| *fmt-dB* | A sign is always shown ("+" for positive and zero, "−" for negative), one decimal, then " dB" | +4.5 dB · −3.5 dB · +0.0 dB |
| *fmt-dBFS* | One decimal, then " dB" (the threshold is in dBFS) | −20.0 dB |
| *fmt-inf* | The silence floor | −∞ dB (display; the host text stays "-inf dB") (P-32) |
| *fmt-Q* | Below 10: two decimals. 10 and up: one decimal. The knob value has no prefix; the graph readout prefixes "Q " | 0.71 · 4.00 · 12.5 · readout "Q 0.71" |
| *fmt-slope* | Whole number, then " dB/oct", or "Brickwall". The picker shows the number only | 24 dB/oct |
| *fmt-dBoct* | One decimal, then " dB/oct" | 4.5 dB/oct |
| *fmt-pct* | Whole percent, a space, then "%" | 100 % |
| *fmt-pct0* | Knob 0–100, whole number, then " %" | 50 % |
| *fmt-pan* | "C" at centre (\|v\| < 0.05). Otherwise one decimal of \|v\| followed by L/R, or M/S in M/S Pan Mode (code format) | C · 12.0 L · 30.0 S |
| *fmt-latency* | Whole samples with a comma thousands separator, then " smp" (P-32) | 0 smp · 320 smp · 4,160 smp |
| *fmt-count* | Comma thousands separator | 1,284 frames |
| *fmt-dB-axis* | Gain scale: "+18", "+12", "0", "−6". The positive sign is shown; no unit. Analyzer axis: "−10" … "−80" | |

---

## 3 States

Every state uses tokens only. Where the boards draw a state, it is given as drawn. Everything else is **PROPOSED** (P-27). The states are idle, hover, pressed, focused, selected, multi-selected, disabled or bypassed, soloed, dynamic-active and dragging. A state an element does not have is marked "n/a".

### 3.1 Bar and panel controls

**Icon button**

| State | Style |
| --- | --- |
| Idle | Transparent; icon in ink |
| Hover | surface-300 fill (board) |
| Pressed | surface-400 fill (P-27) |
| Focused | Focus ring |
| On (Freeze, Invert, Bypass, Solo, Listen, Enabled) | surface-300 fill, icon in accent. Solo and Listen also show an 8 px signal-solo dot at the top-right corner (2, 2 inset) and a text label in the tooltip and accessible name ("Solo on"). The token requires the dot to be paired with a text label (P-19) |
| Disabled | Icon in ink-disabled; no hover |
| Muted (`is-muted`) | Icon in ink-muted |

**Chip**

| State | Style |
| --- | --- |
| Idle off | surface-300, ink |
| Hover off | surface-400 |
| Pressed off | surface-400, inset 0 1 0 surface-000 (P-27) |
| On | accent fill, accent-ink text |
| Hover on | accent-hover fill |
| Pressed on | accent-hover fill (P-27) |
| Focused | Focus ring |
| Disabled | surface-300 fill, ink-disabled text, no hover (P-27) |
| Open, when it anchors a popover (Pan mode chip) | surface-400 fill (Processing board) |

**Select**

| State | Style |
| --- | --- |
| Idle | surface-300 |
| Hover | surface-400 (bundle.css) |
| Pressed | surface-400 |
| Open | surface-400 fill. The chevron points away from the menu while closed (down for menus below, up in the status bar) and flips while open (boards) |
| Focused | Focus ring |
| Disabled | ink-disabled text and glyph |

**Segmented control**

| State | Style |
| --- | --- |
| Segment idle | Transparent, ink-muted |
| Segment hover | ink text (P-27) |
| Segment on | surface-400 fill, ink |
| Focused | Focus ring around the whole track; ← and → move the choice |
| Disabled | Every segment ink-disabled |

**Readout chip** (editable values: Output Level, Pan, Solo Gain, Tilt, Gain Scale)

| State | Style |
| --- | --- |
| Idle | As drawn |
| Hover | Ring changes to accent-hover at 1 px; cursor `ns-resize` |
| Dragging | Accent variant text in accent-hover |
| Focused | Focus ring |
| Editing | The inline editor (graph file, §4.9) |

**Knob**

| State | Style |
| --- | --- |
| Idle | As drawn in §2.3 |
| Hover | Face surface-400, value arc accent-hover (P-27) |
| Dragging | Value arc and value text accent-hover. The value text updates every frame |
| Focused | Focus ring (2 px accent, offset 2) as a circle around the face. ↑ and ↓ nudge (Shift fine) |
| Disabled or greyed (Dyn Auto, C-21) | Value arc and tick ink-disabled. Value text ink-disabled ("Auto" for Threshold). Label ink-muted. Still focusable, but not draggable |
| Over-range (P-30) | When Gain × Gain Scale is clamped at ±30 dB, the part of the arc past the clamp is drawn as a signal-clip wedge (`.over`, bundle.css) |

**Menu item**

| State | Style |
| --- | --- |
| Idle | Transparent, ink |
| Hover or keyboard-highlighted | surface-300 (`.hl`, Interactions board) |
| Open submenu parent | Highlighted, as hover |
| Checked | A 16 px check (or shape glyph) stroked in accent |
| Mixed (P-15) | A 16 px dash `M4 8 H12` in accent |
| Disabled | ink-muted text (board: Paste), no hover. The shortcut hint is in ink-muted, 12 px, at the right |

**Processing radio item**

| State | Style |
| --- | --- |
| Idle | Transparent, with a 14 px ring (inset 1.5 ink-muted) |
| Hover | surface-300 |
| On | surface-300 fill, ring inset 4 accent |
| Focused | Focus ring |

**Slope option**

| State | Style |
| --- | --- |
| Idle | Transparent, ink |
| Hover | surface-300 (P-27) |
| On | accent fill, accent-ink text, weight 600 |
| Disabled (Brickwall on a non-cut shape, or slopes the shape does not allow) | ink-muted text |
| Brickwall row idle | surface-300 fill (board) |

**Band list row**

| State | Style |
| --- | --- |
| Idle | Transparent |
| Hover | surface-300 at 50 % opacity (P-27) |
| Selected and multi-selected | surface-300 fill with an inset 1 px line-hairline ring (board) |
| Focused (`aria-current`) | Focus ring |
| Disabled band | ink-muted text, dot as an outline ring |
| Soloed | An 8 px signal-solo dot after the name, plus the " · Solo" suffix (P-19) |

**Primary button** (EQ Match)

| State | Style |
| --- | --- |
| Idle | accent fill, accent-ink |
| Hover | accent-hover |
| Disabled | surface-300 fill, ink-disabled text |
| Focused | Focus ring |

### 3.2 Graph elements

**Band handle (node)**

| State | Style |
| --- | --- |
| Idle | 18 px circle filled with the band colour, shadow-handle |
| Hover | Ring `0 0 0 2px surface-100, 0 0 0 3px ink-muted`, plus the shadow part of shadow-handle (P-27). Cursor: move |
| Pressed | As hover |
| Selected | 22 px. Ring `0 0 0 3px ink, 0 0 0 5px surface-100, 0 2px 8px rgba(0,0,0,.6)` (boards) |
| Multi-selected | Every selected node uses the selected style (Interactions board) |
| Focused (keyboard focus, or the focused band of a multi-selection) | Selected style, plus the focus ring (2 px accent) at offset 2 outside the 5 px ground ring (P-27) |
| Disabled or bypassed | Transparent fill, `inset 0 0 0 2px <band colour>` plus shadow-handle, whole node at opacity-bypassed 0.35 (boards). Its curve and fill are also drawn at 0.35. It is excluded from the summed curve (C-20) |
| Soloed (P-19) | Selected or idle style as applicable, plus a 6 px signal-solo dot at the node's top-right (45°, touching the ring). A "Solo" readout is shown beside the node while dragging. Every other band's fill drops to opacity-band-idle and its stroke to 0.5 opacity |
| Dynamic-active | The range handle and its dashed stem are shown (graph file, §5.4). When the live gain change is non-zero, the live curve is shown (P-10) |
| Dragging | The dragged node, and all selected nodes, keep the selected style. The node under the pointer uses accent-hover for its ring instead of ink (P-27). Fills of non-selected bands are opacity-band-idle 0.18 (token). The readout chip follows |
| Clipping (P-30) | While the band's scaled gain is clamped at ±30 dB, the node and its curve take the band-2 hue (token note), and the readout gain value is in signal-clip |

**Range handle**

| State | Style |
| --- | --- |
| Idle | 14 px circle, fill surface-100, `inset 0 0 0 2px <band colour>, 0 2px 6px rgba(0,0,0,.6)`. The stem is a 1 px dashed line (CSS dashed) in the band colour from the node centre to the handle (Main board) |
| Hover | Fill surface-300 (P-27) |
| Dragging | Fill surface-300. The readout shows "Range +3.0 dB" |
| Dynamics disabled (Alt-click) | Not drawn |
| Focused | Focus ring |

**Lasso**

| State | Style |
| --- | --- |
| Dragging | Rectangle filled ink at 0.04 opacity, stroked ink 1 px, dash 4 3 (Interactions board) |

**Add cursor** (empty-plot hover)

| State | Style |
| --- | --- |
| Hover, with no node under the pointer | A dashed circle r 9 (ink, 1.5, dash 3 3) with a plus sign (8 px, ink 1.5) at the pointer, and the hover readout (graph file, §5.6) |
