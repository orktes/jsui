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
# default 1.5) and writes <out>-gpu.png / <out>-cpu.png. It also drives
# hover springs and four surface views of the runtime for 30 frames and
# fails when the main thread blocked on Godot's render thread more than
# RENDER_MAX_WAITS times meanwhile (default 0; with
# GODOT_RENDER_THREAD=separate the frames of all the views are queued there
# as one job per frame, without waiting). Without it every run must use the
# CPU raster (headless, OpenGL Compatibility).
# Runs with a display (GODOT_RENDERING=vulkan or GODOT_DISPLAY) also read
# back what Godot drew for JsuiView (the viewport texture) and fail when it
# differs from JsuiView's own frame by more than VIEWPORT_MAX_DIFF (mean
# channel difference; default 2), writing <out>-shown.png / <out>-expected.png:
# the other checks read JsuiView's texture, which can be right while the
# screen shows another (e.g. Godot's magenta / black placeholder).
# GODOT_RENDER_THREAD=unsafe|safe|separate passes --render-thread to Godot
# (default: the project's thread model, "safe": it renders on the main
# thread; "separate" gives the RenderingServer its own thread, as some games
# run it).
# SIZES (default "1280x720 2560x1440") picks the window sizes.
#
# VK_VALIDATION=sync (with GODOT_RENDERING=vulkan) runs Godot under the
# Khronos validation layer (apt install vulkan-validationlayers) with
# synchronization validation and fails the run on any validation error: a
# hazard between JSUI's Graphite work and Godot's frame on the textures they
# share, an invalid view of them, and so on. The errors are printed.
#
# PIPELINE_CACHE=<file> sets JsuiView.pipeline_cache_path (main.gd
# --pipeline-cache; the renderer's name goes before the extension: x.bin ->
# x.vulkan.bin), so GPU runs record the pipelines they compile there.
# FIRST_SCROLL=1 measures the first scroll of the demo's list instead of the
# self-test (main.gd --first-scroll; one run per size, no capture), and
# EXPECT_WARM_PIPELINES=1 fails it unless the cache loaded and precompiled
# pipelines and the scroll compiled none (CI: over the file the smoke test
# and a first scroll with the same FIXED_FPS recorded). TOUR=1 also clicks
# through the rest of the UI after the self-test (main.gd --tour;
# record-pipelines.sh). Which pipelines the frames need depends on the frame
# rate (what the raster cache and opacity layers hold in which frame):
# FIXED_FPS=<n> runs the UI's clock at n frames a second whatever the
# machine's frame rate (JsuiView.fixed_fps, main.gd --ui-fixed-fps), so
# every run draws the same frames; the first scroll then starts at 2 s of UI
# time. NO_WORLD=1 leaves the demo's 3D scene out (main.gd --no-world): the
# same UI in faster frames.
#
# JS_THREAD=off|lockstep sets JsuiView.js_thread (main.gd --js-thread); by
# default the demo's JS runs on its own thread, and the self-test's first
# check stalls it (JsuiView.debug_stall_js) and requires Godot's frames to
# go on meanwhile.
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
  checks=(--expect-renderer vulkan --max-gpu-diff "${GPU_MAX_DIFF:-1.5}" --max-render-waits "${RENDER_MAX_WAITS:-0}")
elif [[ "${GODOT_DISPLAY:-headless}" != headless ]]; then
  display=(--display-driver "$GODOT_DISPLAY" --rendering-driver opengl3 --audio-driver Dummy)
fi
if [[ -n "${FAKE_WEBGPU_INTEROP:-}" ]]; then
  checks=(--expect-renderer cpu --fake-webgpu-interop "$FAKE_WEBGPU_INTEROP")
fi
if [[ "${display[0]}" != --headless ]]; then
  checks+=(--max-viewport-diff "${VIEWPORT_MAX_DIFF:-2}")
fi
if [[ -n "${GODOT_RENDER_THREAD:-}" ]]; then
  display+=(--render-thread "$GODOT_RENDER_THREAD")
fi
if [[ -n "${PIPELINE_CACHE:-}" ]]; then
  mkdir -p "$(dirname "$PIPELINE_CACHE")"  # absolute: Godot runs in the project directory
  checks+=(--pipeline-cache "$(cd "$(dirname "$PIPELINE_CACHE")" && pwd)/$(basename "$PIPELINE_CACHE")")
fi
[[ -n "${EXPECT_WARM_PIPELINES:-}" ]] && checks+=(--expect-warm-pipelines)
[[ -n "${TOUR:-}" ]] && checks+=(--tour)
[[ -n "${NO_WORLD:-}" ]] && checks+=(--no-world)
[[ -n "${FIXED_FPS:-}" ]] && checks+=(--ui-fixed-fps "$FIXED_FPS")
[[ -n "${JS_THREAD:-}" ]] && checks+=(--js-thread "$JS_THREAD")
log="$(mktemp)"
vkdir=""
trap 'rm -f "$log"; if [[ -n "$vkdir" ]]; then rm -rf "$vkdir"; fi' EXIT
if [[ -n "${VK_VALIDATION:-}" ]]; then
  if [[ "$VK_VALIDATION" != sync || "${GODOT_RENDERING:-}" != vulkan ]]; then
    echo "VK_VALIDATION=sync needs GODOT_RENDERING=vulkan" >&2
    exit 1
  fi
  vkdir="$(mktemp -d)"
  # Errors only, into a log checked after each run. Skia probes a few formats
  # the Vulkan 1.1 device it is given does not know (the 4:4:4 two-plane
  # YCbCr ones): harmless, filtered.
  filter=VUID-vkGetPhysicalDeviceFormatProperties-format-parameter
  # With a separate render thread Godot 4.7 does not finalize its
  # RenderingDevice when quitting ("This function (finalize) can only be
  # called from the render thread", also without JSUI on the GPU), so its
  # VkDevice and everything on it outlive the instance: filtered there.
  if [[ "${GODOT_RENDER_THREAD:-}" == separate ]]; then
    filter+=",VUID-vkDestroyDevice-device-05137,VUID-vkDestroyInstance-instance-00629"
  fi
  cat > "$vkdir/vk_layer_settings.txt" <<EOF
khronos_validation.enables = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
khronos_validation.report_flags = error
khronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG
khronos_validation.log_filename = $vkdir/validation.log
khronos_validation.message_id_filter = $filter
EOF
  export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_SETTINGS_PATH="$vkdir"
fi

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
  local mode=(--capture "$png" --frames "$frames" --self-test)
  if [[ -n "${FIRST_SCROLL:-}" ]]; then
    mode=(--first-scroll)
  else
    rm -f "$png"
  fi
  if [[ -n "$vkdir" ]]; then rm -f "$vkdir/validation.log"; fi
  echo "== Godot $GODOT_VERSION, window $size${GODOT_RENDER_THREAD:+, render thread $GODOT_RENDER_THREAD}${VK_VALIDATION:+, Vulkan $VK_VALIDATION validation}${JS_THREAD:+, JS thread $JS_THREAD}"
  "$GODOT" "${display[@]}" --path "$demo" --resolution "$size" -- "${mode[@]}" --window "$size" "${checks[@]}" 2>&1 | tee "$log"
  local status=${PIPESTATUS[0]}
  if (( status != 0 )); then
    echo "godot exited with $status ($size)" >&2
    exit "$status"
  fi
  if grep -qE "SCRIPT ERROR|^FAIL |self-test failed|Cannot get class 'JsuiView'|GDExtension dynamic library not found|\[jsui error\]|^renderer: |gpu-vs-cpu: .*FAIL|viewport-vs-frame: .*FAIL" "$log"; then
    echo "errors in the Godot log ($size)" >&2
    exit 1
  fi
  if [[ -n "${EXPECT_LOG:-}" ]] && ! grep -qE "$EXPECT_LOG" "$log"; then
    echo "no log line matching '$EXPECT_LOG' ($size)" >&2
    exit 1
  fi
  if [[ -n "${FIRST_SCROLL:-}" ]]; then
    local want="^first-scroll: "
    [[ "${GODOT_RENDERING:-}" == vulkan ]] && want="^first-scroll: vulkan: "
    if ! grep -qE "$want" "$log"; then
      echo "no first-scroll result${GODOT_RENDERING:+ on $GODOT_RENDERING} ($size)" >&2
      exit 1
    fi
  else
    if ! grep -q "^PASS mapping" "$log"; then
      echo "the input self-test did not run ($size)" >&2
      exit 1
    fi
    if ! grep -q "^PASS js stall" "$log"; then
      echo "the JS stall check did not run ($size)" >&2
      exit 1
    fi
    if [[ ! -s "$png" ]]; then
      echo "no capture written to $png" >&2
      exit 1
    fi
  fi
  if [[ -n "$vkdir" ]]; then
    if [[ ! -f "$vkdir/validation.log" ]]; then
      echo "VK_VALIDATION: the validation layer did not run (no VK_LAYER_KHRONOS_validation?)" >&2
      exit 1
    fi
    if grep -q "Validation Error" "$vkdir/validation.log"; then
      echo "Vulkan validation errors ($size):" >&2
      grep -A 3 "Validation Error" "$vkdir/validation.log" | cut -c1-1000 | head -n 40 >&2
      exit 1
    fi
    echo "Vulkan $VK_VALIDATION validation: no errors"
  fi
}

written=()
for size in ${SIZES:-1280x720 2560x1440}; do
  png="$out"
  [[ "$size" != 1280x720 ]] && png="${out%.png}-${size}.png"
  [[ "$size" == 2560x1440 ]] && png="${out%.png}-2x.png"
  run "$size" "$png"
  if [[ -n "${FIRST_SCROLL:-}" ]]; then written+=("first-scroll@$size"); else written+=("$png"); fi
done
echo "ok: ${written[*]}"
