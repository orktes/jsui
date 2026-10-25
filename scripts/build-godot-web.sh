#!/usr/bin/env bash
# Threaded WebAssembly build of the Godot integration, to link statically into
# a threaded Godot web export template of the godot-aw3 fork
# (integrations/godot/README.md, "Threaded web build"):
#
#   scripts/build-godot-web.sh [Release|Debug]
#   -> integrations/godot/demo/bin/libjsui_godot.web.threads.wasm32.a
#
# Steps (each skipped when up to date): emsdk 4.0.20 (the fork's), threaded
# Skia (scripts/build-skia-wasm-mt.sh), then JSUI + QuickJS + Yoga + godot-cpp
# + the integration with -pthread, linked into one relocatable object with
# local symbols (tools/wasm-localize) so it can sit next to the engine's own
# HarfBuzz / FreeType / ICU / libpng / zlib.
#
# JOBS (default 2) limits parallelism; BUILD_DIR (default build-godot-web)
# selects the CMake build dir; EMSDK_VERSION overrides 4.0.20 (it must be the
# engine's).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-Release}"
JOBS="${JOBS:-2}"
EMSDK_VERSION="${EMSDK_VERSION:-4.0.20}"
EMSDK_DIR="$ROOT/third_party/emsdk-$EMSDK_VERSION"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-godot-web}"
SKIA_DIR="$ROOT/third_party/skia-wasm-mt"

EMSDK_VERSION="$EMSDK_VERSION" JOBS="$JOBS" "$ROOT/scripts/build-skia-wasm-mt.sh"

# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh" > /dev/null 2>&1
emcmake cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" \
  -DJSUI_SKIA_DIR="$SKIA_DIR" -DJSUI_WASM_THREADS=ON -DJSUI_BUILD_GODOT=ON \
  -DJSUI_HOST_JS=OFF -DJSUI_EMDAWNWEBGPU_PORT=emdawnwebgpu
cmake --build "$BUILD_DIR" -j "$JOBS" --target jsui_godot_web
LIB="$ROOT/integrations/godot/demo/bin/libjsui_godot.web.threads.wasm32.a"
ls -la "$LIB"

# Link check: a threaded program with its own HarfBuzz / FreeType / libpng /
# zlib symbols (as a Godot template has) links the library with Godot's
# flags, and its calls still reach its own copies.
CHECK_DIR="$BUILD_DIR/link-check"
mkdir -p "$CHECK_DIR"
emcc -O2 -pthread -sSUPPORT_LONGJMP=wasm -fno-exceptions --use-port=emdawnwebgpu -sENVIRONMENT=node,worker \
  "$ROOT/integrations/godot/web-link-check.c" "$LIB" -o "$CHECK_DIR/check.js"
"${EMSDK_NODE:-node}" "$CHECK_DIR/check.js"
