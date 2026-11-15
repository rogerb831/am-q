# Third-party dependencies

Every third-party dependency (SDKs, frameworks, libraries, test tools) and its licence.
Counsel must clear an entry before it ships in a product build.

## Main repository (core/dsp, tests, tools)

| Name | Version | Licence | Used for | Shipped in product? | Source |
| --- | --- | --- | --- | --- | --- |
| doctest | 2.4.11 | MIT | Unit tests (`tests/`), fetched at configure time with CMake FetchContent | No (test only) | https://github.com/doctest/doctest |

## Framework spike (`spikes/clap-native/`, not product code)

Cloned into `spikes/clap-native/deps/` (git-ignored) or fetched by Visage's CMake at configure
time. The CLAP-native route was chosen on 2026-11-14 (`docs/plan/decisions.md`, D1), so the
entries linked into the plugin become Phase 1 product dependencies and need counsel clearance
before a product build ships.

| Name | Version / commit | Licence | Used for | Linked into the plugin binary? | Source |
| --- | --- | --- | --- | --- | --- |
| CLAP SDK | 1.2.10 (`195b42a`) | MIT | Plugin API headers | Yes (headers) | https://github.com/free-audio/clap |
| clap-helpers | main @ `a380c97` (no release tags) | MIT | C++ plugin base class, host proxy, contract checks | Yes (headers) | https://github.com/free-audio/clap-helpers |
| clap-wrapper | v0.17.0 (`806bba0`) | MIT | CLAP → VST3 wrapper (clap-first build) | Yes | https://github.com/free-audio/clap-wrapper |
| {fmt} (bundled in clap-wrapper) | as bundled | MIT | String formatting inside clap-wrapper | Yes | https://github.com/fmtlib/fmt |
| VST3 SDK | 3.8.0 (`9fad977`) | MIT (since 3.8.0) | VST3 interfaces used by clap-wrapper; the SDK's validator for testing | Yes (wrapper part) | https://github.com/steinbergmedia/vst3sdk |
| Visage | v1.0.0 (`5754ff8`) | MIT | GPU UI (Direct3D 11 on Windows) | Yes | https://github.com/VitalAudio/visage |
| FreeType (via Visage) | 2.14.1 | FreeType License (FTL, BSD-style with credit clause) or GPL-2.0, at our choice; we would use FTL | Font rasterisation | Yes | https://gitlab.freedesktop.org/freetype/freetype |
| stb (vendored in Visage) | as vendored | MIT or public domain | Image resize | Yes | https://github.com/nothings/stb |
| Lato Regular (embedded by Visage) | as vendored | SIL Open Font License 1.1 | Visage default font resource | Yes (embedded) | Visage `visage_graphics/fonts` |
| Droid Sans Mono (embedded by Visage) | as vendored | Apache-2.0 | Visage default monospace font resource | Yes (embedded) | Visage `visage_graphics/fonts` |
| Lucide cursors (vendored in Visage) | as vendored | ISC | Drag cursors | Yes | Visage `visage_windowing/cursors` |
| glslang (via Visage) | 16.2.0 | BSD-3-Clause and others (see its LICENSE.txt) | Build-time shader compiler only | No | https://github.com/KhronosGroup/glslang |
| SPIRV-Cross (via Visage) | vulkan-sdk-1.4.341.0 | Apache-2.0 | Build-time shader transpiler only | No | https://github.com/KhronosGroup/SPIRV-Cross |
| clap-validator | 0.4.1 (built from source with cargo) | MIT | CLAP conformance testing | No (test tool) | https://github.com/free-audio/clap-validator |
| pluginval | v1.0.4 (official Windows release binary, unsigned; size matches the GitHub asset, no digest published) | GPL-3.0 | VST3 conformance testing (external tool, never linked) | No (test tool) | https://github.com/Tracktion/pluginval |

The editor loads the system Segoe UI font at runtime (the design tokens' fallback for Inter);
no Segoe UI file is redistributed. Inter itself (SIL OFL 1.1) is not yet bundled.
