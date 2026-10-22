#!/usr/bin/env bash
# Downloads Emdawnwebgpu (Dawn's webgpu.h / webgpu_cpp.h implementation for
# Emscripten, which replaced emscripten's removed -sUSE_WEBGPU) into
# third_party/emdawnwebgpu (gitignored), pinned to one Dawn release.
#
# Both the Skia Graphite wasm build (scripts/build-skia-wasm-graphite.sh) and
# JSUI's GPU wasm module (cmake/Emscripten.cmake, JSUI_WEBGPU) compile and link
# with --use-port=third_party/emdawnwebgpu/emdawnwebgpu_pkg/emdawnwebgpu.port.py.
# We pin a Dawn release instead of emscripten's bundled remote port so the
# headers are the newest ones and match across the two builds.
#
# EMDAWNWEBGPU_DIR overrides the install location.
set -euo pipefail

VERSION="v20261021.191805"  # Dawn 0c2f14b375ebf624fdb467845c29ba3043732993
SHA512="41521ac1fcd17cb041247cd6812208c241b4f5b5316e23e3346f669ab89198f119f224c4b74bb33a0f065802a561cebe7ced1303966b13356b56a10eb23adee7"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="${EMDAWNWEBGPU_DIR:-$ROOT/third_party/emdawnwebgpu}"

if [ -f "$DIR/.version" ] && [ "$(cat "$DIR/.version")" = "$VERSION" ] && [ -f "$DIR/emdawnwebgpu_pkg/emdawnwebgpu.port.py" ]; then
  echo "emdawnwebgpu $VERSION already in $DIR"
  exit 0
fi
rm -rf "$DIR"
mkdir -p "$DIR"
zip="$DIR/emdawnwebgpu_pkg-$VERSION.zip"
curl -sSfL -o "$zip" "https://github.com/google/dawn/releases/download/$VERSION/emdawnwebgpu_pkg-$VERSION.zip"
echo "$SHA512  $zip" | sha512sum -c -
(cd "$DIR" && unzip -q "$zip")
rm "$zip"
echo "$VERSION" > "$DIR/.version"
echo "emdawnwebgpu $VERSION installed in $DIR"
