#!/usr/bin/env bash
# Downloads the pinned clap-validator and pluginval release binaries into <dest>, checks their
# SHA-256 against the values below and unpacks them. Versions are recorded in THIRD_PARTY.md.
#
#   tools/ci/fetch_validators.sh <dest> windows|macos
set -euo pipefail

dest=${1:?usage: fetch_validators.sh <dest> windows|macos}
platform=${2:?usage: fetch_validators.sh <dest> windows|macos}

CV_BASE=https://github.com/free-audio/clap-validator/releases/download/0.4.1
PV_BASE=https://github.com/Tracktion/pluginval/releases/download/v1.0.4

case "$platform" in
  windows)
    cv_zip=clap-validator-0.4.1-127-g152b982-windows.zip
    cv_sha=d935c3af0a45c3911ea2e900f4aa5d6709dac82bb485f0c4ce28648ab2cd0c10
    pv_zip=pluginval_Windows.zip
    pv_sha=c08e61ce3b96db41636f8ec7e76f4c7e2c13ebdac7fa1b5a1f52b4f32ec715ab
    ;;
  macos)
    cv_zip=clap-validator-0.4.1-127-g152b982-macos-universal.zip
    cv_sha=bbec8cd7d18274e549d5d8c12ece3cec54be966129388dd2e742b9957f2ba9f1
    pv_zip=pluginval_macOS.zip
    pv_sha=3c4c533bda0c5059eea3ddaea752d757ee2025041f0f47e6bcb0e87f6082b29f
    ;;
  *) echo "unknown platform: $platform" >&2; exit 2 ;;
esac

sha256() {  # reads stdin, so a backslash in the path can't prefix the hash (Git Bash does that)
  if command -v sha256sum >/dev/null; then sha256sum < "$1" | cut -d' ' -f1
  else shasum -a 256 < "$1" | cut -d' ' -f1; fi
}

fetch() {  # url zip sha subdir
  local zip="$dest/$2"
  curl -fsSL --retry 3 -o "$zip" "$1/$2"
  local got; got=$(sha256 "$zip")
  if [ "$got" != "$3" ]; then
    echo "SHA-256 mismatch for $2: got $got, expected $3" >&2
    exit 1
  fi
  mkdir -p "$dest/$4"
  unzip -oq "$zip" -d "$dest/$4"
}

mkdir -p "$dest"
fetch "$CV_BASE" "$cv_zip" "$cv_sha" clap-validator
fetch "$PV_BASE" "$pv_zip" "$pv_sha" pluginval
echo "validators unpacked in $dest"
