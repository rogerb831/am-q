# 10 Reference

## Band settings

Each of the 24 bands has these settings. All of them can be automated by the host.

| Setting | Range | Default |
| --- | --- | --- |
| Enabled | on / off | on |
| Frequency | 10 Hz – 30 kHz | 1 kHz |
| Gain | −30 to +30 dB | 0 dB |
| Q | 0.025 – 40 | 1 |
| Shape | Bell, Low Shelf, Low Cut, High Shelf, High Cut, Notch, Band Pass, Tilt Shelf, Flat Tilt, All Pass | Bell |
| Slope | 6, 12, 18, 24, 30, 36, 48, 72, 96 dB/oct, Brickwall | 12 dB/oct |
| Placement | Stereo, Left, Right, Mid, Side | Stereo |
| Dynamic | on / off | off |
| Dynamic Range | −30 to +30 dB | 0 dB (−6 dB when made dynamic from the menu) |
| Auto (threshold) | on / off | on |
| Threshold | −90 to 0 dB | −20 dB |
| Attack | 0 – 100 % (about 0.25 ms – 250 ms) | 50 % |
| Release | 0 – 100 % (about 5 ms – 1 s) | 50 % |
| Side chain | Int / Ext | Int |
| Side-chain filter | Band / Free | Band |
| Side-chain Low / High (Free) | 10 Hz – 20 kHz | 10 Hz / 20 kHz (fully open) |
| Side-chain Listen | on / off | off |
| Spectral | on / off | off |
| Density | 0 – 100 % (about 1/4 to 1/16 octave) | 50 % |
| Spectral Tilt | on / off | off |
| Solo | on / off | off |

## Global settings

| Setting | Range | Default | Automatable |
| --- | --- | --- | --- |
| Processing Mode | Zero Latency, Natural Phase, Linear Phase | Zero Latency | yes |
| Resolution | Low, Medium, High, Very High, Maximum | Medium | yes |
| Gain Scale | 0 – 200 % | 100 % | yes |
| Output Level | −∞, −36 to +36 dB | 0 dB | yes |
| Output Pan | full left (or mid) – full right (or side) | centre | yes |
| Pan Mode | L/R, M/S | L/R | yes |
| Invert polarity | on / off | off | yes |
| Bypass | on / off | off | yes (it is the host's bypass) |
| Auto Gain | on / off | off | yes |
| Solo Gain | −20 to +20 dB | 0 dB | yes |
| Analyzer: Show Pre / Post / Side chain | on / off | on / on / off | no |
| Analyzer Freeze, Peak hold, Collisions | on / off | off, on, off | no |
| Analyzer Resolution | Low, Medium, High, Max | Medium | no |
| Analyzer Speed | Very Slow – Very Fast | Medium | no |
| Analyzer Tilt | 0 – 6 dB/oct | 4.5 dB/oct | no |
| Analyzer Range | 60, 90, 120 dB | 90 dB | no |
| Display range | ±3, ±6, ±12, ±30 dB | ±12 dB | no |

The interface scale and Gain-Q coupling are editor settings: the scale is saved with the project,
coupling is not.

## Typing values

Wherever you can type a value (double-click a node, click a knob's value, double-click a
readout), Am-Q reads the usual forms:

| Value | You can type |
| --- | --- |
| Frequency | `1000`, `1k`, `1.2k`, `1.2 kHz`, `800 Hz` |
| Gain, levels | `3`, `+3`, `-3`, `−3`, `+3 dB` |
| Q | `0.71`, `4` |
| Silence (output level) | `-inf` |
| Pan | `C` for centre, `12 L`, `30 R`, and in M/S Pan Mode `20 M`, `40 S` |

Am-Q shows values the same way everywhere: frequencies below 1 kHz in whole Hz ("180 Hz"), from
1 to 10 kHz with two decimals ("1.82 kHz"), above with one ("14.5 kHz"); gains always with a sign
("+4.5 dB", "−3.5 dB"); latency in samples ("4,160 smp").

## Latency

| Mode | Latency |
| --- | --- |
| Zero Latency | 0 samples |
| Natural Phase | 320 samples, at every sample rate |
| Linear Phase at 44.1 / 48 kHz | Low 2,112 · Med 4,160 · High 8,256 · V. High 16,448 · Max 65,600 samples |
| Linear Phase at 88.2 / 96 kHz | twice the above |
| Linear Phase at 176.4 / 192 kHz | four times the above |
| Any mode, with a spectral band active | at least the spectral latency at the current resolution |

Placement (Stereo, Left, Right, Mid, Side) never changes the latency, and neither does bypass.

## Compatibility

- Projects and presets keep loading in later versions of Am-Q. A newer version reads older
  settings; an older version ignores settings it does not know.
- The band slots, setting names and their host automation targets never change, so automation
  written today keeps working.

## Not in this version

- **A/B comparison:** the A, B and arrow buttons in the top bar are shown but do nothing yet.
- **Split** in the band menu is always greyed out.
- **Full screen** is not available.
- **macOS** and the **AU** format: Am-Q 0.0.1 is for Windows only, as CLAP and VST3.
- **Reference files** for EQ Match: WAV only (not FLAC, MP3 or AIFF), up to 20 minutes.
- **Screen readers:** Am-Q can be used from the keyboard, but it does not yet talk to screen
  readers.
- **Surround:** mono and stereo only.

## Troubleshooting

**A band set to Ext does nothing.** Nothing is reaching Am-Q's side-chain input. Route a signal to
it in your host (chapter 1); turn on **Side chain** in the analyzer's Show row to see whether it
arrives.

**Keys do nothing, or go to the host.** Am-Q only takes the keys listed in chapter 9, and only
while its window has the focus: click a band or the graph first. Every other key, including the
space bar, goes to your host on purpose.

**A short silence when changing mode.** Changing to or from a mode with latency (or switching the
first spectral band on, or the last one off) fades the output briefly while the host adjusts for
the new latency. Some hosts adjust only at the next playback start. Choose the mode before you
mix.

**The sound changes timing after a mode change.** Your host has not yet applied the new latency.
Stop and start playback.

**The output is silent.** Check Output Level (−∞?), Bypass, and whether a band is soloed (a green
dot on the Solo button).

**High CPU.** Linear Phase at High resolution and above, many spectral bands, and very steep
slopes cost the most. Use Zero Latency or Natural Phase while tracking, and keep Linear Phase at
Med unless you need more.

**The editor is slow or jerky over remote desktop.** Am-Q draws in step with the display, and
remote desktop sessions often run the display at about 30 frames per second. The audio is not
affected.

**"Audio dropped" in EQ Match.** The host did not deliver audio to Am-Q for a while, for example
while it was busy. The captured audio is still valid; capture a little longer if you like.
