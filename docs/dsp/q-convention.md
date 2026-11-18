# Displayed Q to design Q, per shape

Area 01 R14 and area 09 R12. This is the convention the build uses, decided 2026-11-18: the product
owner kept the full Q range on every shape ("Keep the full range, as built"). The code is
`core/dsp/src/analog_prototype.cpp` (`designAnalogPrototype`); the analog prototypes are then
mapped to digital sections (phase0-design-notes.md, "Analog → digital").

## The Q control

- **Range:** 0.025 to 40 for every shape (`kMinQ`, `kMaxQ`, R8).
- **Host value:** a position from 0 to 1 on a log scale, Q = 0.025 · 1600^position, so each
  equal step of the control is the same ratio of Q (`params::qFromPosition`). Q 1 sits at
  position 0.5; Q 0.71 at about 0.45.
- **Order:** each 6 dB/oct of slope is one filter order. Bell, Notch and Band Pass have at least
  order 2. Brickwall ignores the slope's order (it is a fixed order-16 design).

## Per shape

| Shape | What the displayed Q is | Q-independent cases |
| --- | --- | --- |
| **Bell** | The centre frequency divided by the bandwidth between the two frequencies where the boost or cut is half its peak in dB: Q = f0 / (f_upper − f_lower), with f_lower · f_upper = f0². It does not change with gain (constant Q). | none |
| **Notch** | f0 divided by the bandwidth between the −3 dB points. | none |
| **Band Pass** | f0 divided by the bandwidth between the −3 dB points. | none |
| **Low Cut, High Cut** | A multiplier on the Q of the cascade's strongest (highest-Q) Butterworth pole pair. Q 1 is maximally flat (Butterworth); below 1 the corner softens, above 1 it gains a resonant peak. | 6 dB/oct (first order: no pole pair); Brickwall |
| **Low Shelf, High Shelf** | The same multiplier, applied to the strongest pole pair and the matching zero pair. Q 1 is maximally flat; above 1 the shelf overshoots at its corner; below 1 the transition widens. | 6 dB/oct |
| **Tilt Shelf** | As a high shelf (above), re-centred so f0 is the 0 dB pivot: −gain/2 below, +gain/2 above. | 6 dB/oct |
| **All Pass** | As the cuts: the multiplier on the strongest pole pair. Higher Q makes the phase turn more abruptly around f0. The magnitude is always flat. | 6 dB/oct |
| **Flat Tilt** | Q is not used (the Q control is disabled for it, and so is Slope). | always |

How the bell's bandwidth Q comes about: the bell is a low shelf of half its order whose half-gain
point is at Ω = 1, moved to f0 by the low-pass to band-pass mapping s → Q (s + 1/s). The mapping
puts the half-gain points at the two frequencies where Q (Ω − 1/Ω) = ±1, which is the bandwidth
above. Notch and Band Pass use the same mapping on a high-pass and a low-pass prototype, whose
−3 dB points are at Ω = 1.

## High Q on cuts and shelves (decided: full range)

On cuts, shelves and Tilt Shelf, a Q above 1 adds a resonant bump at the corner, and the full
range is kept: at the top of the control the bump is very large (roughly +20 to +46 dB on a
shelf at Q 10–40). This is deliberate, a resonant sound the user asks for, and the same control
range as every other shape. Band gain is still limited to ±30 dB (R13), but that limit is on the
gain parameter, not on the resonance.

## Flat Tilt

- A constant dB/octave tilt from 20 Hz to 20 kHz: first-order pole and zero pairs spaced half an
  octave apart.
- **Gain** is the total tilt across 20 Hz–20 kHz (about 10 octaves, so a gain of 10 dB is about
  1 dB per octave).
- **Pivot:** 0 dB at the band frequency f0.
- **Gain limit:** the level at the 1 kHz reference is limited to ±30 dB (R14's fixed mid-band
  reference). When the pivot is far from 1 kHz, that limit is reached before the gain parameter's
  own limit.
