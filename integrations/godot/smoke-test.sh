#!/usr/bin/env bash
# Runs the Godot demo headless with its input self-test, at content scale 1
# and 2, and saves what JsuiView rendered.
#
#   integrations/godot/smoke-test.sh [out.png]    # also writes <out>-2x.png
#
# Expects the GDExtension in integrations/godot/demo/bin (cmake
# -DJSUI_BUILD_GODOT=ON) and the demo bundle in demo/ui/dist (npx jsui build).
# Uses $GODOT if set, otherwise downloads the official Godot 4.7.2 Linux build
# (SHA-256 checked) into $GODOT_CACHE (default: build/_deps/godot).
#
# GODOT_DISPLAY=x11 runs with a real (OpenGL) renderer, e.g. under xvfb-run,
# and additionally saves the composited window as <out>-viewport.png.
#
# GODOT_RENDERING=vulkan runs Godot on Vulkan (Forward+, x11 display: run it
# under xvfb-run; Mesa's lavapipe is enough) and checks that JsuiView renders
# with Skia Graphite straight into its RenderingDevice texture: the run fails
# on the CPU fallback, or when the GPU frame differs from the CPU raster of
# the same frame by more than GPU_MAX_DIFF (mean channel difference, 0..255;
# default 1.5) and writes <out>-gpu.png / <out>-cpu.png. Without it every run
# must use the CPU raster (headless, OpenGL Compatibility).
# SIZES (default "1280x720 2560x1440") picks the window sizes.
#
# FAKE_WEBGPU_INTEROP=<rev> registers a stand-in for the godot-aw3 fork's
# WebGPUInterop singleton (main.gd --fake-webgpu-interop) that reports Dawn
# revision <rev> and null handles. Whatever the driver, JsuiView must then
# fall back to the CPU raster; EXPECT_LOG=<regex> also requires a log line
# (e.g. the revision-mismatch warning).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
demo="$here/demo"
out="${1:-$repo/build/godot-smoke.png}"
# Absolute, without GNU realpath -m (macOS realpath has no -m).
mkdir -p "$(dirname "$out")"
out="$(cd "$(dirname "$out")" && pwd)/$(basename "$out")"
frames="${FRAMES:-90}"

# GODOT_VERSION=4.3-stable runs the oldest Godot the extension supports
# (compatibility_minimum in demo/jsui.gdextension). Other versions need
# GODOT_SHA256 for their Linux x86_64 zip.
GODOT_VERSION="${GODOT_VERSION:-4.7.2-stable}"
# SHA-256 of Godot_v<version>_linux.x86_64.zip (their SHA-512 match the
# releases' SHA512-SUMS.txt).
case "$GODOT_VERSION" in
  4.7.2-stable) known_sha256=cadd3204e728a35d3f13adb7fd0d7902636b79f6b95c40c265eb73b6c35329e4 ;;
  4.3-stable) known_sha256=7de56444b130b10af84d19c7e0cf63cf9e9937ee4ba94364c3b7dd114253ca21 ;;
  *) known_sha256= ;;
esac
GODOT_SHA256="${GODOT_SHA256:-$known_sha256}"

if [[ -z "${GODOT:-}" ]]; then
  if [[ -z "$GODOT_SHA256" ]]; then
    echo "set GODOT_SHA256 (or GODOT) for Godot $GODOT_VERSION" >&2
    exit 1
  fi
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
# Godot 4.7.2 aborts while exiting the very first headless --import of a
# project that has a GDExtension (reproduced with an empty godot-cpp 10
# extension); the import itself is complete by then, hence `|| true`.
if [[ ! -f "$demo/.godot/extension_list.cfg" ]]; then
  "$GODOT" --headless --path "$demo" --import || true
fi

mkdir -p "$(dirname "$out")"
display=(--headless)
checks=(--expect-renderer cpu)
if [[ "${GODOT_RENDERING:-}" == vulkan ]]; then
  display=(--display-driver "${GODOT_DISPLAY:-x11}" --rendering-driver vulkan --rendering-method forward_plus --audio-driver Dummy)
  checks=(--expect-renderer vulkan --max-gpu-diff "${GPU_MAX_DIFF:-1.5}")
elif [[ "${GODOT_DISPLAY:-headless}" != headless ]]; then
  display=(--display-driver "$GODOT_DISPLAY" --rendering-driver opengl3 --audio-driver Dummy)
fi
if [[ -n "${FAKE_WEBGPU_INTEROP:-}" ]]; then
  checks=(--expect-renderer cpu --fake-webgpu-interop "$FAKE_WEBGPU_INTEROP")
fi
log="$(mktemp)"
trap 'rm -f "$log"' EXIT

# One run per window size. main.gd's --self-test drives the UI through
# Godot's input pipeline (Input.parse_input_event, as real devices do): hover,
# click (checking the exact logical position JSUI saw), gamepad focus + A,
# wheel and trackpad-pan scrolling, click-to-focus + typing + Enter in a
# TextInput, the Ping call, call_ui and an unhandled click. It prints PASS /
# FAIL for each.
#   1280x720   content scale 1 (the project's base size)
#   2560x1440  content scale 2, as on a Retina Mac: the canvas_items stretch
#              doubles everything, JSUI renders at pixel ratio 2, and input
#              maps back to the same logical px
# --window goes to the script too: the headless display server ignores
# --resolution (its window is 64x64).
run() {
  local size="$1" png="$2"
  rm -f "$png"
  echo "== Godot $GODOT_VERSION, window $size"
  "$GODOT" "${display[@]}" --path "$demo" --resolution "$size" -- --capture "$png" --frames "$frames" --self-test --window "$size" "${checks[@]}" 2>&1 | tee "$log"
  local status=${PIPESTATUS[0]}
  if (( status != 0 )); then
    echo "godot exited with $status ($size)" >&2
    exit "$status"
  fi
  if grep -qE "SCRIPT ERROR|^FAIL |self-test failed|Cannot get class 'JsuiView'|GDExtension dynamic library not found|\[jsui error\]|^renderer: |gpu-vs-cpu: .*FAIL" "$log"; then
    echo "errors in the Godot log ($size)" >&2
    exit 1
  fi
  if [[ -n "${EXPECT_LOG:-}" ]] && ! grep -qE "$EXPECT_LOG" "$log"; then
    echo "no log line matching '$EXPECT_LOG' ($size)" >&2
    exit 1
  fi
  if ! grep -q "^PASS mapping" "$log"; then
    echo "the input self-test did not run ($size)" >&2
    exit 1
  fi
  if [[ ! -s "$png" ]]; then
    echo "no capture written to $png" >&2
    exit 1
  fi
}

written=()
for size in ${SIZES:-1280x720 2560x1440}; do
  png="$out"
  [[ "$size" != 1280x720 ]] && png="${out%.png}-${size}.png"
  [[ "$size" == 2560x1440 ]] && png="${out%.png}-2x.png"
  run "$size" "$png"
  written+=("$png")
done
echo "ok: ${written[*]}"
