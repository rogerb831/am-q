# Am-Q

Am-Q is a 24-band parametric EQ (VST3 and CLAP planned). This repository is starting
**Phase 1**. It has the framework-free C++20 DSP core from Phase 0, with its analog prototypes,
analog-to-digital designs and accuracy harness, and the plugin and editor skeleton on the chosen
stack.

Read [CLEAN_ROOM.md](CLEAN_ROOM.md) before contributing. The only spec is
[`docs/plan/`](docs/plan/README.md) plus the public references it cites.

## Layout

| Path | What |
| --- | --- |
| `core/dsp/` | Framework-free DSP library (`amq::dsp`). No JUCE, no CLAP, no third-party code. |
| `core/dsp/include/amq/dsp/conventions.hpp` | Shared conventions: dB <-> linear, 90 %-step time constants, log2-Hz frequency. |
| `core/dsp/include/amq/dsp/analog_prototype.hpp` | The ten shapes as cascaded 2nd-order s-domain sections; `evaluate(Omega)` returns H(jOmega). |
| `core/dsp/include/amq/dsp/digital_design.hpp` | Matched designs (three-point, least-squares), joint minimax refinement (default, `src/joint_fit.cpp`) and bilinear with pre-warp. |
| `core/dsp/include/amq/dsp/update_policy.hpp` | PROVISIONAL R15/R17 update schedule, behind an interface. |
| `core/params/` | The parameter table (`amq::params`): 573 typed parameters with frozen hashed host ids, one lock-free setter, mappings, formatters and parsers. Spec: [docs/params/parameter-table.md](docs/params/parameter-table.md). |
| `core/analysis/` | Processor-to-editor snapshots (`amq::analysis`): snapshot types at a silence floor, a lock-free triple buffer per type, level meters (peak, RMS, hold) and a publisher that max-merges the peaks of unread frames. |
| `core/engine/` | The framework-free processor entry (`amq::engine`): parameter ramps, 24 bands in series as TPT SVF cascades (three-point matched design while parameters move, the refined R12 design from a worker thread once they settle, gliding coefficients), an on-demand M/S stage, the output Level/Pan/Invert stage, safe silence. The plugin calls it once per block piece. |
| `plugin/` | The CLAP plugin (clap-helpers) and its clap-first VST3 build (clap-wrapper). So far: the parameter table exported to the host, block splitting at every automation event, 64-bit I/O conversion, a versioned id/value state chunk, zero latency, and the editor (response graph: double-click to create a band, drag to move, wheel for Q, Alt-click to enable or disable). All five placements process audio (Stereo, Left, Right, and Mid/Side through an on-demand M/S stage); dynamics and spectral bands do not yet. |
| `ui/` | The editor. `amq_ui_model` holds the framework-free logic: edit controller, gestures, attachments, graph geometry, response curve and interaction. `amq_ui` holds the Visage frames: response graph, band nodes, readout. Also the accessible-control model (`ui/include/amq/ui/accessible.h`). |
| `cmake/AmqPluginDeps.cmake` | Pinned plugin dependencies (CLAP, clap-helpers, clap-wrapper, VST3 SDK, Visage), fetched at configure time. |
| `tools/vst3_validator/` | Builds the Steinberg VST3 validator from the pinned SDK, as a separate project. |
| `tools/ci/` | CI scripts: fetch the pinned clap-validator and pluginval, run all three validators. |
| `.github/workflows/ci.yml` | CI: Windows shipping and strict-host-check builds, and macOS (allowed to fail for now); ctest plus the validators. |
| `tools/accuracy_harness/` | Sweep shapes x slopes x frequencies x gains x Q x sample rates; report errors vs the analog prototype. |
| `tests/` | doctest unit tests. |
| `docs/plan/` | The sanitized engineering plan (spec). |
| `docs/dsp/phase0-design-notes.md` | Phase 0 design notes, references, results and open questions. |
| `spikes/clap-native/` | Framework spike: CLAP-native plugin + clap-wrapper VST3 + Visage editor. Not product code; see its README for results. |

Framework: a CLAP-native plugin, wrapped to VST3 by clap-wrapper, with a Visage editor (decided 2026-11-14; [docs/plan/decisions.md](docs/plan/decisions.md)). Phase 1 builds the plugin on this stack.

## Build (Windows, Visual Studio 2022)

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

```bash
cmake --build build --config Release --parallel
```

```bash
ctest --test-dir build -C Release --output-on-failure
```

Any CMake 3.24+ generator and C++20 compiler should work. Tests fetch doctest with
FetchContent at configure time (see [THIRD_PARTY.md](THIRD_PARTY.md)).

The plugin (`AMQ_BUILD_PLUGIN`, ON by default) also fetches its pinned dependencies. The
first configure takes about 10 minutes, because Visage builds its shader tools. Outputs are
`Am-Q.clap` and `Am-Q.vst3`. Other options:

- `-DAMQ_BUILD_PLUGIN=OFF` builds the DSP core, tools and tests only.
- `-DAMQ_STRICT_HOST_CHECKS=ON` is a development build where any CLAP host contract violation aborts. Never ship it ([decision D1](docs/plan/decisions.md)).
- `-DFETCHCONTENT_SOURCE_DIR_AMQ_<NAME>=<path>` reuses an existing clone instead of downloading. `<NAME>` is `CLAP`, `CLAP_HELPERS`, `CLAP_WRAPPER`, `VST3SDK` or `VISAGE`.

## Accuracy harness

```bash
build/tools/accuracy_harness/Release/amq_accuracy_harness.exe --out build/reports/accuracy_report.md --csv build/reports/accuracy_cases.csv
```

Options: `--quick` (small grid), `--no-bilinear`, `--default-only`, `--order-n` (never add the
correction biquad), `--always-extra` (always try it), `--threads N`, `--strict` (exit 1 if any
case misses the revised R12 tolerance; TBD cases excluded), `--case <shapeId> <slopeId> <f0> <gainDb> <q> <fs>` (per-section dump and
fine scan of one band), `--bench` (single-thread design cost per method).
