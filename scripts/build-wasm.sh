#!/usr/bin/env bash
# One-stop WASM build: emsdk -> Skia for WASM -> JSUI -> web/dist/jsui.{js,wasm}.
#
#   scripts/build-wasm.sh [Release|Debug]
#   WEBGL=1 scripts/build-wasm.sh       # GPU variant -> web/dist-webgl/jsui.{js,wasm}
#
# Each step is skipped when already up to date. JOBS (default 2) limits
# parallelism; BUILD_DIR (default build-wasm, build-wasm-webgl with WEBGL=1)
# selects the CMake build dir.
#
# WEBGL=1 builds Skia with Ganesh + WebGL (third_party/skia-wasm-webgl) and
# JSUI with JSUI_GPU, so web/jsui-web.js can draw through Skia on the canvas'
# WebGL context (createJsui({ renderer: 'gpu', moduleUrl: './dist-webgl/jsui.js' })).
# The CPU path is in that build too.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-Release}"
JOBS="${JOBS:-2}"
WEBGL="${WEBGL:-0}"
if [ "$WEBGL" = "1" ]; then
  BUILD_DIR="${BUILD_DIR:-$ROOT/build-wasm-webgl}"
  SKIA_DIR="$ROOT/third_party/skia-wasm-webgl"
  DIST="$ROOT/web/dist-webgl"
else
  BUILD_DIR="${BUILD_DIR:-$ROOT/build-wasm}"
  SKIA_DIR="$ROOT/third_party/skia-wasm"
  DIST="$ROOT/web/dist"
fi

"$ROOT/scripts/setup-emsdk.sh"
WEBGL="$WEBGL" JOBS="$JOBS" "$ROOT/scripts/build-skia-wasm.sh"

# shellcheck disable=SC1091
source "$ROOT/third_party/emsdk/emsdk_env.sh" > /dev/null 2>&1
emcmake cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" \
  -DJSUI_SKIA_DIR="$SKIA_DIR" -DJSUI_WEB_DIST="$DIST"
cmake --build "$BUILD_DIR" -j "$JOBS" --target jsui-wasm
ls -la "$DIST"
