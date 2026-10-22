#!/usr/bin/env bash
# Installs the Emscripten SDK into third_party/emsdk (gitignored).
#
# The version is pinned to the one Skia m124 uses for CanvasKit
# (skia/bin/activate-emsdk at chrome/m124), so the WASM Skia libraries and
# JSUI are built with the same compiler and libc++ ABI.
#
#   scripts/setup-emsdk.sh                    # install + activate
#   source third_party/emsdk/emsdk_env.sh     # then use emcc / emcmake
#
# EMSDK_VERSION=x.y.z overrides the version (the Skia libraries must then be
# rebuilt with the same version). EMSDK_DIR overrides the install location.
set -euo pipefail

EMSDK_VERSION="${EMSDK_VERSION:-3.1.44}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EMSDK_DIR="${EMSDK_DIR:-$ROOT/third_party/emsdk}"

if [ ! -d "$EMSDK_DIR/.git" ]; then
  mkdir -p "$(dirname "$EMSDK_DIR")"
  git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
fi

# Idempotent (and CI-cache friendly): skip the slow install when the pinned
# version is already there.
stamp="$EMSDK_DIR/.jsui-installed"
if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$EMSDK_VERSION" ] && [ -x "$EMSDK_DIR/upstream/emscripten/emcc" ]; then
  echo "emsdk $EMSDK_VERSION already installed in $EMSDK_DIR"
  exit 0
fi

cd "$EMSDK_DIR"
./emsdk install "$EMSDK_VERSION"
./emsdk activate "$EMSDK_VERSION"
echo "$EMSDK_VERSION" > "$stamp"
echo "emsdk $EMSDK_VERSION installed. Run: source $EMSDK_DIR/emsdk_env.sh"
