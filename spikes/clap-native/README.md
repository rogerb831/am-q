# Spike: CLAP-native plugin + clap-wrapper VST3 + Visage editor

Status: done on Windows x64 (2026-11-14). The spike tests one question: should Phase 1 use
JUCE 8 or a CLAP-native core? Spike code only; nothing here is used by `core/`. Nothing is
committed.

## What was built

- `AmQSpike.clap` and `AmQSpike.vst3`: one Bell band on `core/dsp`.
  - **Parameters:** Freq, Gain and Q, plus Output Gain and a Dummy Latency toggle (64 samples).
  - **CLAP side:** written with clap-helpers. It implements params (with gestures and flush), state, latency, render, audio ports and gui.
  - **VST3:** built from the same static library by clap-wrapper's `make_clapfirst_plugins`. The VST3 3.7 folder bundle is used, and the CLAP is linked into the `.vst3` statically.
- **Editor** (Visage, Direct3D 11): drawn from `docs/design/spike-reference/` tokens and geometry.
  - Dark ground, grid and axis labels, and the 0 dB line.
  - One amber curve, computed from the same design the audio uses.
  - One draggable band handle: drag sets Freq and Gain, wheel sets Q.
  - A small panel with the Freq, Gain and Q readouts.
- **Test hosts**, written for this spike:
  - `tests/clap_host_test.cpp`: CLAP automation and latency.
  - `tools-vst3/vst3_host_test.cpp`: VST3 automation and latency, using the SDK's hosting classes.
  - `tests/clap_gui_test.cpp`: embeds the editor, drives hover and drag, and reads the editor's own redraw counters.

## Pass criteria

| # | Criterion | Result | Evidence |
| --- | --- | --- | --- |
| 1 | Automation lands on the exact sample (CLAP and VST3) | **Pass.** CLAP: 6 of 6 parameter steps (offsets 0, 1, 37, 101, 200, 255) change the output at exactly that sample. VST3: 4 of 4. Method: two renders (with and without the event) are compared; the first differing sample must equal the event offset. | `evidence/clap-host-test.txt`, `evidence/vst3-host-test.txt` |
| 2 | Latency-change notification reaches the host | **Pass.** CLAP: toggle → `request_callback` → `request_restart` → `latency.changed()` inside `activate` → `latency.get() = 64` → impulse 64 samples late. VST3 (via clap-wrapper): `restartComponent(kLatencyChanged)` reaches the host → after the restart `getLatencySamples() = 64` → impulse 64 samples late. | same files |
| 3 | clap-validator, VST3 validator and pluginval run clean | **Pass with one warning.** See below. | `evidence/clap-validator-plain.txt`, `evidence/vst3-validator.txt`, `evidence/pluginval.txt` |
| 4 | Display-rate rendering, dirty-region redraw, lag-free drag, in Reaper plus other hosts | **Pass in our own host and in pluginval's editor tests.** In real hosts: Studio One scans it cleanly; Blue Cat's Axiom crashed on load; the fixed build loads, and drag and wheel work in its editor (user report). Fender Studio: loads, automation works (user report, VST3 inferred). Sample accuracy and the latency notification are not yet measured in a real host. Reaper not tested (see below). | `evidence/clap-gui-test.txt` |
| 5 | Accessibility support of the UI layer | **Visage has none** (no UIA, MSAA or NSAccessibility support, and no accessible names or roles). | source survey of Visage v1.0.0 |

### Criterion 3 in detail

- **clap-validator 0.4.1:** 44 tests run, 31 passed, **0 failed**, 1 warning, 12 skipped.
  - The skipped tests cover features the spike doesn't have: note ports, preset discovery, 64-bit audio and audio-port configuration.
  - The warning is `process-audio-denormals`: processing is 2.0–2.8× slower with denormal input. It persists with FTZ/DAZ set inside `process()`, and isn't caused by the clap-helpers checking level. Not investigated further.
  - The first run caught four real plugin bugs, all fixed:
    1. subnormal output;
    2. `latency.changed()` called outside `activate`;
    3. text-to-value failing on "Q 0.04" and "Off";
    4. no `params.rescan` after loading state, and nondeterministic struct padding in the saved state.
- **Steinberg VST3 validator (SDK 3.8.0):** 47 passed, **0 failed**. Its sample-accurate automation test passed, with an info note that "not all points have been read via IParameterChanges". The first run failed only on the single-file `.vst3`; the build now produces the folder bundle.
- **pluginval 1.0.4 at strictness 10:** **SUCCESS**. That covers editor open, editor open while processing, editor automation, state restoration, parameter thread safety and parameter fuzzing.

### Criterion 4 in detail (our own GUI host)

| Phase | Graph draws | Curve draws | Handle draws | Panel draws |
| --- | ---: | ---: | ---: | ---: |
| Open | 1 | 1 | 1 | 1 |
| Idle 1 s | 0 | 0 | 0 | 0 |
| Hover in and out of the handle | 0 | 0 | **2** | 0 |
| 3 s handle drag | **0** (cached) | 97 | 98 | 97 |

- **Display rate:** during the drag the curve redraws once per display frame (31.3 ms mean interval, 32.1 ms p95). This machine's primary display is a **Microsoft Remote Display Adapter limited to 32 Hz** (remote session), so display rate here is 32 fps. A local 60/120 Hz panel was not available to test.
- **Input lag:** mouse input to the next handle draw takes 7.8 ms on average and 17.2 ms at worst, which is under one frame.
- **Dirty regions:** hover redraws only the handle frame, and the cached ground never redraws during a drag.
- **Not done: Reaper.** It isn't installed on this machine, and installing it means accepting its licence, so I didn't.
- **Studio One 7.2.3:** the VST3 is installed in the system VST3 folder and Studio One's scanner loaded it cleanly (scan result 0, class data found). Loading it on a track and using the editor there is not yet reported.
- **Fender Studio:** loads, and automation works (user report, 2026-11-14; not measured). Format: VST3, inferred because the VST3 is the only AmQSpike format installed in a system plugin folder (no `.clap` in the CLAP folders). Sample-accurate automation and the latency-change notification were not measured in a real DAW for this spike, by decision: our own CLAP and VST3 test hosts measure both exactly. Per-host measurement belongs to the manual host matrix (plan: Phase 3), and per-build checks to the Phase 1 test harness.
- **Blue Cat's Axiom (standalone VST3 host): crashed on load, fixed; the fixed build loads (user retest, 2026-11-14).** In the editor, handle drag and wheel-to-Q work (user report; no lag noted, not measured). Automation and latency in Axiom were not reported. See "Host crash: Blue Cat's Axiom" below.
- **Display:** no local high-refresh display was available, as noted above.

## Host crash: Blue Cat's Axiom

- **Symptom:** loading AmQSpike.vst3 in Axiom killed Axiom (Windows event 1000: faulting module `AmQSpike.vst3`, exception `0xc0000409` fail-fast, offset `0x1a5d51`). Studio One's scan of the same binary was clean.
- **Cause** (diagnosed by the coordinating session from the dump against our own binary only):
  - Axiom calls `IAudioProcessor::getLatencySamples()` off the main thread. VST3 allows that.
  - clap-wrapper forwards the call to our `clap_plugin_latency.get`.
  - The plugin base was `clap::helpers::Plugin<Terminate, Maximal>`. Its `ensureMainThread("clap_plugin_latency.get")` check failed, so `hostMisbehaving` called `std::terminate()` → `abort()`, which is the fail-fast.
  - Call chain (offsets into the crashing build): latency.get wrapper `0xbd70` → ensureMainThread `0x10b44` → hostMisbehaving `0x1188c` → terminate `0x1925f9` → abort `0x1a5d51`.
- **Fix:**
  1. **Checking policy.** The default build uses `Plugin<Ignore, None>`. `Terminate` + `Maximal` is only compiled in with `-DAMQ_STRICT_HOST_CHECKS=ON`, for development and contract testing, and must never ship. `Ignore` + `Minimal` was rejected: clap-helpers still runs the thread checks there and, on each violation, builds a message string and calls `host.log` on the calling thread. That allocates on the audio thread, and since `host.log` isn't virtual it can't be throttled.
  2. **Latency.** `activeLatency_` is now `std::atomic<uint32_t>`, written in `activate()` and read relaxed by `latency.get` from any thread.
  3. **Everything else a wrapped VST3 host can reach off the main thread** was reviewed:
     - Already thread-safe, unchanged: params count/info/value/text conversion (constant tables and atomics), audio-ports count/info (constant), `state.save` (atomic reads) and render mode.
     - `state.load` used to push into the GUI→audio SPSC queue, where the editor is the only producer; that is a second producer when called from another thread. It also touched the editor and called `params.rescan` directly. It now stores the values into the atomics, sets a flag the audio thread applies on its next process or flush, and, when off the main thread, defers `params.rescan` and the editor refresh to `on_main_thread`.
     - `gui.get_size` no longer touches the editor off the main thread; it returns the fixed size.
- **Regression test:** `clap_host_test` case 3. The test host now implements `thread_check`. From a second thread it calls latency.get, params get_value/value_to_text/text_to_value, audio-ports count/get, gui get_size, state save, and state load with a patched gain. It then checks that `params.rescan` is deferred to `on_main_thread` and that the loaded gain reaches the audio thread. It passes on the default build. A strict build would abort on it by design.
- **Validators after the fix:** clap-validator 44 run, 31 passed, 0 failed, 1 warning (the same denormal timing warning); VST3 validator 47/47; pluginval strictness 10 SUCCESS; CLAP and VST3 host tests all pass.
- **Retest:** the fixed build loads in Axiom without crashing (user report, 2026-11-14).
- **Lesson for Phase 1:** with a CLAP-first build wrapped to VST3, CLAP's main-thread rules don't hold. The wrapper passes VST3's looser threading straight through. Every plugin callback reachable from a VST3 interface has to be safe on any thread, and nothing may abort the host.

## Build complexity

- **One CMake project (`spikes/clap-native/`).** It adds:
  - `core/dsp` from the main repo;
  - CLAP, clap-helpers, Visage and clap-wrapper, all as subdirectories.
- **VST3 tools project (`tools-vst3/`).** A separate project builds the SDK validator and the VST3 test host, so the SDK's own targets don't clash with clap-wrapper's copy.
- **First configure takes about 10 minutes.** Visage downloads FreeType, glslang and SPIRV-Cross, and builds its shader compiler.
- **Incremental builds take seconds.**
- **Binaries:** `.clap` 2.2 MB, `.vst3` 2.9 MB (Release).
- **Version friction found:**
  - clap-wrapper's older release (v0.9.1) doesn't compile against VST3 SDK 3.8. v0.17.0 targets 3.8.0.
  - clap-helpers has no release tags, and its main branch needs CLAP 1.2.10's draft `param-hovered` extension.
  - windows.h's `min`/`max` macros break the VST3 SDK headers unless NOMINMAX is defined project-wide.
- **Visage gotcha:** `ApplicationWindow::onResize` didn't fire for an embedded window, so frames have to be laid out explicitly.
- **Static MSVC runtime** throughout, to match clap-wrapper's default.
- **Release builds emit PDBs** (`/Zi`, `/DEBUG` with `/OPT:REF /OPT:ICF` kept), so host crash dumps can be symbolised.
- **`AMQ_STRICT_HOST_CHECKS`** (default OFF) selects clap-helpers' `Terminate` + `Maximal` checking. Development only.

Rebuild from scratch:

```bash
cd spikes/clap-native/deps && git clone --depth 1 --branch 1.2.10 https://github.com/free-audio/clap.git clap
```

The other clones follow the same pattern; versions are in `THIRD_PARTY.md` (clap-helpers
`a380c97`, clap-wrapper `v0.17.0`, Visage `v1.0.0`, VST3 SDK `v3.8.0_build_66` with
submodules `base pluginterfaces public.sdk cmake`, clap-validator `0.4.1`). Then:

```bash
cmake -S spikes/clap-native -B spikes/clap-native/build -G "Visual Studio 17 2022" -A x64
```

```bash
cmake --build spikes/clap-native/build --config Release
```

## Licences

Everything linked into the plugin is MIT, plus three permissive licences: FreeType FTL,
fonts under OFL 1.1 and Apache-2.0, and ISC cursors. Since 3.8.0 the VST3 SDK is MIT too, which
removes the old GPL3-or-Steinberg-agreement choice. pluginval (GPL-3.0) is used only as an
external test tool. Full table: `THIRD_PARTY.md`.

## Spike limitations (not framework findings)

- **Design on the audio thread:** coefficients are designed on the audio thread with the three-point matched design, which allocates. Phase 1 moves the design off-thread (R19).
- **Curve sample rate:** the editor computes the curve at 48 kHz instead of the host rate.
- **Gain change:** gain steps are applied without ramps, so exactness can be measured. R3 ramps are Phase 1 work.
- **Not implemented:** 64-bit audio, bypass, side chain and resizing.

## Recommendation

- **Host and format layer: CLAP-native + clap-wrapper works.**
  - Sample-accurate automation and latency notification hold in both formats.
  - All three validators are clean once real plugin bugs were fixed. clap-validator's contract checks found those bugs; a framework would have hidden them.
  - Licences are all permissive, and build complexity is moderate.
  - Caveat from the Axiom crash: the CLAP core must treat every VST3-reachable callback as callable from any thread, and must never use clap-helpers' `Terminate` handler in a shipping build.
- **UI layer: Visage does not fit the plan as it stands.**
  - Rendering performance, dirty-region redraw and drag latency are good.
  - It has **no accessibility support**, yet the plan requires screen-reader names for every control and keyboard navigation (GUI design; area 15 R3). Adding a UIA (Windows) and NSAccessibility (macOS) layer ourselves is a substantial, unbudgeted piece of work.
  - Visage v1.0 is also young: one tagged release, and the embedding gotcha above.
- **Choice for the project:**
  - **(a) JUCE 8** gets accessibility and a mature editor host for free. This is the plan's fallback, and I recommend it **if accessibility must ship in v1**.
  - **(b) CLAP-native core + clap-wrapper with Visage** only if the team budgets building the accessibility layer, or takes Visage's accessibility to its maintainers.
  - **Not evaluated:** a CLAP-native core with a different, accessible UI layer.
- **Before deciding**, the GUI should be checked in at least two real DAWs (Reaper, Studio One) on a local display. macOS is untested.
