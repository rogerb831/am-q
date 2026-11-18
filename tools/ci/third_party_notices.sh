#!/usr/bin/env bash
# Writes THIRD_PARTY_NOTICES.txt: the licence of every third-party component linked or embedded in
# the Am-Q plugin binaries (THIRD_PARTY.md, "Yes" rows), as their licences require for binary
# distribution. Sources come from the build's fetched dependencies; Lato's OFL text, which Visage
# does not include, is kept in licenses/. Fails if any licence is missing.
#
#   tools/ci/third_party_notices.sh <build dir> <output file>
set -euo pipefail
build="$1"
out="$2"
deps="$build/_deps"
root="$(cd "$(dirname "$0")/../.." && pwd)"

section() {  # name, file
    if [ ! -f "$2" ]; then
        echo "third_party_notices: missing licence for $1: $2" >&2
        exit 1
    fi
    {
        printf '\n================================================================================\n'
        printf '%s\n' "$1"
        printf '================================================================================\n\n'
        tr -d '\r' < "$2"
    } >> "$out"
}

cat > "$out" <<'EOF'
Am-Q third-party notices

Am-Q includes the following third-party software. Each is listed with its licence text.
EOF

section "PFFFT (FFT library)" "$deps/amq_pffft-src/LICENSE.txt"
section "CLAP SDK" "$deps/amq_clap-src/LICENSE"
section "clap-helpers" "$deps/amq_clap_helpers-src/LICENSE"
section "clap-wrapper" "$deps/amq_clap_wrapper-src/LICENSE"
# {fmt}, bundled in clap-wrapper: its licence is the comment that opens format.h.
fmt_tmp="$(mktemp)"
awk 'NR==1 && /^\/\*/ {next} /\*\// {exit} {print}' "$deps/amq_clap_wrapper-src/libs/fmt/fmt/format.h" | sed 's/^  //' > "$fmt_tmp"
section "{fmt} (bundled in clap-wrapper)" "$fmt_tmp"
rm -f "$fmt_tmp"
section "VST3 SDK" "$deps/amq_vst3sdk-src/LICENSE.txt"
section "Visage" "$deps/amq_visage-src/LICENSE"
section "FreeType (used under the FreeType License)" "$deps/freetype-src/docs/FTL.TXT"
section "stb" "$deps/amq_visage-src/visage_graphics/third_party/stb/LICENSE"
section "Lato font (embedded by Visage)" "$root/licenses/Lato-OFL.txt"
section "Droid Sans Mono font (embedded by Visage)" "$deps/amq_visage-src/visage_graphics/fonts/LICENSE"
section "Lucide cursors (vendored in Visage)" "$deps/amq_visage-src/visage_windowing/cursors/LICENSE"
