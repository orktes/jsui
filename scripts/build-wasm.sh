#!/usr/bin/env bash
# One-stop WASM build: emsdk -> Skia for WASM -> JSUI -> web/dist/jsui.{js,wasm}.
#
#   scripts/build-wasm.sh [Release|Debug]
#
# Each step is skipped when already up to date. JOBS (default 2) limits
# parallelism; BUILD_DIR (default build-wasm) selects the CMake build dir.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-Release}"
JOBS="${JOBS:-2}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-wasm}"

"$ROOT/scripts/setup-emsdk.sh"
JOBS="$JOBS" "$ROOT/scripts/build-skia-wasm.sh"

# shellcheck disable=SC1091
source "$ROOT/third_party/emsdk/emsdk_env.sh" > /dev/null 2>&1
emcmake cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build "$BUILD_DIR" -j "$JOBS" --target jsui-wasm
ls -la "$ROOT/web/dist"
