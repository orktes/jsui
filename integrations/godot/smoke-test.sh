#!/usr/bin/env bash
# Runs the Godot demo for a number of frames and saves what JsuiView rendered.
#
#   integrations/godot/smoke-test.sh [out.png]
#
# Expects the GDExtension in integrations/godot/demo/bin (cmake
# -DJSUI_BUILD_GODOT=ON) and the demo bundle in demo/ui/dist (npx jsui build).
# Uses $GODOT if set, otherwise downloads the official Godot 4.3 Linux build
# (SHA-256 checked) into $GODOT_CACHE (default: build/_deps/godot).
#
# GODOT_DISPLAY=x11 runs with a real (OpenGL) renderer, e.g. under xvfb-run,
# and additionally saves the composited window as <out>-viewport.png.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
demo="$here/demo"
out="$(realpath -m "${1:-$repo/build/godot-smoke.png}")"
frames="${FRAMES:-90}"

GODOT_VERSION=4.3-stable
GODOT_SHA256=7de56444b130b10af84d19c7e0cf63cf9e9937ee4ba94364c3b7dd114253ca21

if [[ -z "${GODOT:-}" ]]; then
  cache="${GODOT_CACHE:-$repo/build/_deps/godot}"
  GODOT="$cache/Godot_v${GODOT_VERSION}_linux.x86_64"
  if [[ ! -x "$GODOT" ]]; then
    mkdir -p "$cache"
    zip="$cache/godot.zip"
    url="https://github.com/godotengine/godot/releases/download/${GODOT_VERSION}/Godot_v${GODOT_VERSION}_linux.x86_64.zip"
    echo "Downloading $url"
    curl -fsSL -o "$zip" "$url"
    echo "$GODOT_SHA256  $zip" | sha256sum -c -
    unzip -o -q "$zip" -d "$cache"
    rm -f "$zip"
    chmod +x "$GODOT"
  fi
fi

shopt -s nullglob
libs=("$demo"/bin/libjsui_godot.*)
if (( ${#libs[@]} == 0 )); then
  echo "no GDExtension in $demo/bin: build with cmake -DJSUI_BUILD_GODOT=ON first" >&2
  exit 1
fi
if [[ ! -f "$demo/ui/dist/bundle.js" ]]; then
  echo "no demo bundle: run npx jsui build integrations/godot/demo/ui/app.jsx -o integrations/godot/demo/ui/dist/bundle.js" >&2
  exit 1
fi

# The editor registers GDExtensions (.godot/extension_list.cfg) and imports
# resources; a fresh checkout needs one pass before the game can run.
if [[ ! -f "$demo/.godot/extension_list.cfg" ]]; then
  "$GODOT" --headless --path "$demo" --import || true
fi

mkdir -p "$(dirname "$out")"
rm -f "$out"
display=(--headless)
if [[ "${GODOT_DISPLAY:-headless}" != headless ]]; then
  display=(--display-driver "$GODOT_DISPLAY" --rendering-driver opengl3 --audio-driver Dummy)
fi
log="$(mktemp)"
trap 'rm -f "$log"' EXIT
"$GODOT" "${display[@]}" --path "$demo" --resolution 1280x720 -- --capture "$out" --frames "$frames" --self-test 2>&1 | tee "$log"
status=${PIPESTATUS[0]}

if (( status != 0 )); then
  echo "godot exited with $status" >&2
  exit "$status"
fi
if grep -qE "SCRIPT ERROR|self-test failed|Cannot get class 'JsuiView'|GDExtension dynamic library not found|\[jsui error\]" "$log"; then
  echo "errors in the Godot log" >&2
  exit 1
fi
if [[ ! -s "$out" ]]; then
  echo "no capture written to $out" >&2
  exit 1
fi
echo "ok: $out"
