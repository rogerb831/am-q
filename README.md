# Am-Q

Am-Q is a 24-band parametric EQ (VST3 and CLAP planned). This repository is at
**Phase 0**: a framework-free C++20 DSP sandbox for the analog prototypes, the
analog-to-digital designs and the accuracy harness.

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
| `tools/accuracy_harness/` | Sweep shapes x slopes x frequencies x gains x Q x sample rates; report errors vs the analog prototype. |
| `tests/` | doctest unit tests. |
| `docs/plan/` | The sanitized engineering plan (spec). |
| `docs/dsp/phase0-design-notes.md` | Phase 0 design notes, references, results and open questions. |
| `spikes/clap-native/` | Framework spike: CLAP-native plugin + clap-wrapper VST3 + Visage editor. Not product code; see its README for results. |

The framework choice (JUCE 8 vs CLAP-native) is still open, so no plugin wrappers exist yet.

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

## Accuracy harness

```bash
build/tools/accuracy_harness/Release/amq_accuracy_harness.exe --out build/reports/accuracy_report.md --csv build/reports/accuracy_cases.csv
```

Options: `--quick` (small grid), `--no-bilinear`, `--default-only`, `--order-n` (never add the
correction biquad), `--always-extra` (always try it), `--threads N`, `--strict` (exit 1 if any
case misses the revised R12 tolerance; TBD cases excluded), `--case <shapeId> <slopeId> <f0> <gainDb> <q> <fs>` (per-section dump and
fine scan of one band), `--bench` (single-thread design cost per method).
