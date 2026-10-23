#!/usr/bin/env bash
# Builds Skia (chrome/m157, pinned) for WebAssembly with every backend the
# browser host can pick from, in ONE set of libraries, and installs it into
# third_party/skia-wasm-gpu in the layout cmake/Deps.cmake expects (same layout
# as scripts/build-skia-wasm.sh):
#
#   * Graphite on WebGPU (skia_enable_graphite + skia_use_dawn +
#     skia_use_webgpu: Graphite's Dawn backend compiled against Emdawnwebgpu's
#     webgpu.h / webgpu_cpp.h, which implement webgpu.h on navigator.gpu)
#   * Ganesh on WebGL 2 (skia_enable_ganesh + skia_use_webgl, as in
#     build-skia-wasm.sh WEBGL=1)
#   * CPU raster (always there)
#
# so a single jsui.wasm falls back WebGPU -> WebGL -> CPU at run time without
# fetching another module (scripts/build-wasm.sh GPU=1 builds it into
# web/dist-gpu).
#
# Versions (the newest at the time of writing, pinned):
#   Skia          chrome/m157 @ SKIA_COMMIT below
#   emsdk         6.0.13 (setup-emsdk.sh, into third_party/emsdk-6.0.13)
#   Emdawnwebgpu  Dawn release v20261021.191805 (setup-emdawnwebgpu.sh)
#
# Emscripten removed its built-in -sUSE_WEBGPU bindings (4.0.18) in favour of
# Emdawnwebgpu (--use-port=...), but Skia (m157 and main alike) still codes
# its __EMSCRIPTEN__ paths against the old USE_WEBGPU API (CanvasKit pins
# emsdk 4.0.7). scripts/patches/skia-m157-emdawnwebgpu.patch ports Graphite's
# Dawn backend to Emdawnwebgpu: guards that only choose an old API spelling
# now test SK_DAWN_LEGACY_EMSCRIPTEN_API (never defined here) and use the
# current webgpu.h API; guards about what the browser lacks (YCbCr, Dawn-native
# features, synchronous waits) keep testing __EMSCRIPTEN__. The patch also
# makes //third_party/dawn header-only on wasm: Skia would otherwise build
# Dawn *native* with CMake, which is meaningless where the browser is the
# WebGPU implementation.
#
# Three changes for JSUI's frames, marked "jsui:" (docs/ARCHITECTURE.md,
# WebGPU): DawnResourceProvider caches uniform bind groups (without push
# constants Graphite made one per pipeline / uniform change); ClipStack
# applies a whole-pixel device rect clip as a scissor instead of cropping
# draws to it or merging it into other clip shapes; Device rasterizes paths
# up to 1024 px whole into the path atlas instead of cut to the clip. The
# last two keep damage-only repaints identical to full ones.
#
# SIMD: as build-skia-wasm.sh (SKIA_WASM_SIMD=1, its default), Skia is built
# with -msimd128 -msse4.1 so the raster pipeline runs its SSE4.1 stages on
# WASM SIMD128; the patch also drops SKVX_DISABLE_SIMD (m157's name for
# m124's SKNX_NO_SIMD) from gn/toolchain/wasm.gni, removes skcms' AVX
# includes and maps _mm_cvtps_epi32 onto SIMD128 in SkRasterPipeline_opts.h.
# JSUI is compiled with the same flags (cmake/Emscripten.cmake).
#
# The m124 builds (build-skia-wasm.sh: CPU and WebGL, emsdk 3.1.44) are
# untouched. This build has its own emsdk because the Skia libraries and JSUI
# must be built with the same compiler / libc++.
#
# Idempotent: skipped when third_party/skia-wasm-gpu/.stamp matches the hash
# of the inputs, so CI caches third_party/skia-wasm-gpu keyed on
# hashFiles('scripts/build-skia-wasm-graphite.sh', 'scripts/setup-emsdk.sh',
#           'scripts/setup-emdawnwebgpu.sh', 'scripts/patches/skia-m157-emdawnwebgpu.patch').
#
# Environment:
#   SKIA_SRC       Skia checkout (default third_party/skia-src-m157; reused)
#   EMSDK          emsdk (default third_party/emsdk-6.0.13)
#   SKIA_WASM_OUT  install dir (default third_party/skia-wasm-gpu)
#   JOBS           ninja parallelism (default 2)
#   SKIA_WASM_DEBUG=1  is_debug build
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKIA_BRANCH="chrome/m157"
SKIA_COMMIT="3c6589d4882124ecb315db04227eccd81fe0b435"
EMSDK_VERSION="6.0.13"
SKIA_SRC="${SKIA_SRC:-$ROOT/third_party/skia-src-m157}"
OUT_DIR="${SKIA_WASM_OUT:-$ROOT/third_party/skia-wasm-gpu}"
EMSDK="${EMSDK:-$ROOT/third_party/emsdk-$EMSDK_VERSION}"
PATCH="$ROOT/scripts/patches/skia-m157-emdawnwebgpu.patch"
EMDAWNWEBGPU_PORT="$ROOT/third_party/emdawnwebgpu/emdawnwebgpu_pkg/emdawnwebgpu.port.py"
JOBS="${JOBS:-2}"
DEBUG="${SKIA_WASM_DEBUG:-0}"

hash_inputs() {
  cat "$ROOT/scripts/build-skia-wasm-graphite.sh" "$ROOT/scripts/setup-emsdk.sh" \
    "$ROOT/scripts/setup-emdawnwebgpu.sh" "$PATCH" | sha256sum | cut -d' ' -f1
}
STAMP_VALUE="$(hash_inputs)-debug$DEBUG"
if [ -f "$OUT_DIR/.stamp" ] && [ "$(cat "$OUT_DIR/.stamp")" = "$STAMP_VALUE" ] && [ -f "$OUT_DIR/out/libskia.a" ]; then
  echo "skia-wasm-gpu is up to date ($OUT_DIR)"
  exit 0
fi

# ------------------------------------------------------- emsdk + emdawnwebgpu
EMSDK_VERSION="$EMSDK_VERSION" EMSDK_DIR="$EMSDK" "$ROOT/scripts/setup-emsdk.sh"
"$ROOT/scripts/setup-emdawnwebgpu.sh"

# ----------------------------------------------------------------- source
if [ ! -d "$SKIA_SRC/.git" ]; then
  echo "Cloning Skia $SKIA_BRANCH into $SKIA_SRC"
  git clone --depth 1 --branch "$SKIA_BRANCH" https://skia.googlesource.com/skia "$SKIA_SRC"
fi
cd "$SKIA_SRC"
if [ "$(git rev-parse HEAD)" != "$SKIA_COMMIT" ]; then
  git fetch --depth 1 origin "$SKIA_COMMIT"
  git checkout -q --force "$SKIA_COMMIT"
fi
# Apply the patch to pristine files (idempotent across patch edits).
# SKIA_PATCH_DEV=1 leaves the tree as it is (while editing the patch:
# regenerate it with `git diff -- src include third_party/dawn modules/skcms gn`).
if [ "${SKIA_PATCH_DEV:-0}" != "1" ]; then
  git checkout -q -- src include third_party/dawn modules/skcms gn
  git apply "$PATCH"
fi

# Only the third-party repos this configuration compiles (no dawn: on wasm
# the WebGPU headers come from the Emdawnwebgpu port).
python3 - <<'PY'
keep = {"brotli", "freetype", "harfbuzz", "icu", "libjpeg-turbo", "libpng", "libwebp", "wuffs", "zlib"}
d = {}
with open("DEPS") as f:
    exec("def Var(x): return vars[x]\n" + f.read(), d)
deps = {k: v for k, v in d["deps"].items()
        if isinstance(v, str) and k.split("/")[-1] in keep and k.startswith("third_party/externals/")}
missing = keep - {k.split("/")[-1] for k in deps}
assert not missing, f"DEPS is missing {missing}"
with open("DEPS.jsui", "w") as f:
    f.write("deps = " + repr(deps) + "\n")
PY
GIT_SYNC_DEPS_PATH="$SKIA_SRC/DEPS.jsui" GIT_SYNC_DEPS_SKIP_EMSDK=1 GIT_SYNC_DEPS_QUIET=1 \
  python3 tools/git-sync-deps
[ -x bin/gn ] || python3 bin/fetch-gn

# ------------------------------------------------------------------ build
if [ "$DEBUG" = "1" ]; then
  BUILD_DIR="out/jsui-wasm-gpu-debug"; IS_DEBUG=true; IS_OFFICIAL=false
else
  BUILD_DIR="out/jsui-wasm-gpu"; IS_DEBUG=false; IS_OFFICIAL=true
fi

ARGS="
is_debug=$IS_DEBUG
is_official_build=$IS_OFFICIAL
is_component_build=false
werror=false
target_cpu=\"wasm\"
skia_emsdk_dir=\"$EMSDK\"
extra_cflags=[\"--use-port=$EMDAWNWEBGPU_PORT\",\"-msimd128\",\"-msse4.1\"]

skia_enable_ganesh=true
skia_use_gl=true
skia_use_webgl=true
skia_gl_standard=\"webgl\"
skia_enable_graphite=true
skia_use_dawn=true
skia_use_webgpu=true
skia_use_angle=false
skia_use_vulkan=false

skia_use_fontconfig=false
skia_use_freetype=true
skia_use_system_freetype2=false
skia_enable_fontmgr_custom_empty=true
skia_enable_fontmgr_custom_embedded=false
skia_enable_fontmgr_custom_directory=false
skia_use_expat=false

skia_use_icu=true
skia_use_client_icu=false
skia_use_libgrapheme=false
skia_use_icu4x=false
skia_use_system_icu=false
skia_use_harfbuzz=true
skia_use_system_harfbuzz=false
skia_enable_skshaper=true
skia_enable_skunicode=true
skia_enable_skparagraph=true

skia_use_libjpeg_turbo_decode=true
skia_use_libjpeg_turbo_encode=false
skia_use_no_jpeg_encode=true
skia_use_libpng_decode=true
skia_use_libpng_encode=true
skia_use_libwebp_decode=true
skia_use_libwebp_encode=false
skia_use_no_webp_encode=true
skia_use_wuffs=true
skia_use_zlib=true
skia_use_system_libjpeg_turbo=false
skia_use_system_libpng=false
skia_use_system_libwebp=false
skia_use_system_zlib=false
skia_use_dng_sdk=false
skia_use_piex=false
skia_use_lua=false

skia_enable_pdf=false
skia_enable_svg=false
skia_enable_skottie=true
skia_enable_tools=false
skia_enable_spirv_validation=false
skia_use_partition_alloc=false
"
ARGS="$(echo "$ARGS" | sed '/^$/d' | tr '\n' ' ')"
echo "gn args: $ARGS"
bin/gn gen "$BUILD_DIR" --args="$ARGS"

TARGETS="${SKIA_TARGETS:-skia skparagraph skshaper skunicode skottie sksg}"
nice ninja -C "$BUILD_DIR" -j "$JOBS" $TARGETS
[ -n "${SKIA_TARGETS:-}" ] && exit 0  # partial build (debugging the patch)

# ---------------------------------------------------------------- install
rm -rf "$OUT_DIR.tmp"
mkdir -p "$OUT_DIR.tmp/out"
copy_headers() {  # copy_headers <src dir> (relative to SKIA_SRC)
  [ -d "$1" ] || return 0
  # rsync, not GNU `cp --parents` (macOS cp has no --parents).
  mkdir -p "$OUT_DIR.tmp/$1"
  rsync -a --prune-empty-dirs --include='*/' --include='*.h' --include='*.hpp' --include='*.inc' --exclude='*' \
    "$1/" "$OUT_DIR.tmp/$1/"
}
copy_headers include
copy_headers src
copy_headers modules
copy_headers third_party/externals/harfbuzz/src
copy_headers third_party/externals/icu/source/common/unicode
copy_headers third_party/externals/icu/source/i18n/unicode
copy_headers third_party/externals/freetype/include
cp LICENSE "$OUT_DIR.tmp/"

# Recent Skia names wasm archives lib<name>.wasm.a; install them as lib<name>.a
# like the m124 build so CMake finds them.
for lib in "$BUILD_DIR"/lib*.a; do
  name="$(basename "$lib")"
  cp "$lib" "$OUT_DIR.tmp/out/${name%.wasm.a}.a"
done
cp "$BUILD_DIR/args.gn" "$OUT_DIR.tmp/out/args.gn"
echo "$EMSDK_VERSION" > "$OUT_DIR.tmp/out/emsdk-version.txt"
echo "$SKIA_BRANCH $SKIA_COMMIT" > "$OUT_DIR.tmp/out/skia-version.txt"

: > "$OUT_DIR.tmp/out/defines.txt"
for target in //:skia //modules/skshaper:skshaper //modules/skunicode:skunicode //modules/skparagraph:skparagraph //modules/skottie:skottie; do
  for config in $(bin/gn desc "$BUILD_DIR" "$target" public_configs); do
    bin/gn desc "$BUILD_DIR" "$config" defines >> "$OUT_DIR.tmp/out/defines.txt"
  done
done
sort -u -o "$OUT_DIR.tmp/out/defines.txt" "$OUT_DIR.tmp/out/defines.txt"
echo "public defines:"; cat "$OUT_DIR.tmp/out/defines.txt"

rm -rf "$OUT_DIR"
mv "$OUT_DIR.tmp" "$OUT_DIR"
echo "$STAMP_VALUE" > "$OUT_DIR/.stamp"
ls -la "$OUT_DIR/out"
echo "skia-wasm-gpu installed into $OUT_DIR"
