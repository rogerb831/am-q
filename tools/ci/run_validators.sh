#!/usr/bin/env bash
# Runs the plugin validators on a built Am-Q and writes one log per validator. Exits non-zero
# if any validator fails; every validator runs either way.
#
#   tools/ci/run_validators.sh <build dir> <validators dir> <log dir> [vst3 validator exe]
#
# <validators dir> is what fetch_validators.sh unpacked. Without the VST3 validator path only
# clap-validator and pluginval run; with the path "-" only clap-validator runs (strict builds).
set -uo pipefail

build=${1:?usage: run_validators.sh <build dir> <validators dir> <log dir> [vst3 validator]}
vdir=${2:?}
logs=${3:?}
vst3val=${4:-}
mkdir -p "$logs"

find_bundle() {  # name: the first match outside _deps, without descending into bundles
  find "$build" -path '*/_deps' -prune -o -name "$1" -print -prune | head -n 1
}

clap=$(find_bundle Am-Q.clap)
vst3=$(find_bundle Am-Q.vst3)
[ -n "$clap" ] || { echo "Am-Q.clap not found under $build" >&2; exit 1; }
echo "CLAP: $clap"
echo "VST3: ${vst3:-(not found)}"

cv=$(find "$vdir/clap-validator" -type f \( -name clap-validator -o -name clap-validator.exe \) | head -n 1)
if [ -x "$vdir/pluginval/pluginval.exe" ] || [ -f "$vdir/pluginval/pluginval.exe" ]; then
  pv="$vdir/pluginval/pluginval.exe"
else
  pv="$vdir/pluginval/pluginval.app/Contents/MacOS/pluginval"
fi
chmod +x "$cv" "$pv" 2>/dev/null || true

failed=()

run() {  # name, command...
  local name=$1; shift
  echo "::group::$name"
  "$@" 2>&1 | tee "$logs/$name.txt"
  local rc=${PIPESTATUS[0]}
  echo "::endgroup::"
  if [ "$rc" -ne 0 ]; then
    echo "::error::$name failed (exit $rc)"
    failed+=("$name")
  else
    echo "$name passed"
  fi
}

run clap-validator "$cv" validate "$clap"

if [ "$vst3val" != "-" ]; then
  [ -n "$vst3" ] || { echo "Am-Q.vst3 not found under $build" >&2; exit 1; }
  run pluginval "$pv" --strictness-level 10 --timeout-ms 600000 \
      --output-dir "$logs/pluginval" --validate "$vst3"
  if [ -n "$vst3val" ]; then
    run vst3-validator "$vst3val" "$vst3"
  fi
fi

if [ ${#failed[@]} -ne 0 ]; then
  echo "Failed: ${failed[*]}" >&2
  exit 1
fi
echo "All validators passed"
