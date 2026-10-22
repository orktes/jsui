#!/usr/bin/env bash
# Builds Skia m124 for WebAssembly (CPU raster) and installs headers + static
# libraries into third_party/skia-wasm, the layout cmake/Deps.cmake expects:
#
#   third_party/skia-wasm/include/...            public headers
#   third_party/skia-wasm/modules/*/include/...  module headers (skparagraph, ...)
#   third_party/skia-wasm/src/**/*.h             private headers some modules include
#   third_party/skia-wasm/third_party/externals/{harfbuzz,icu,freetype} headers
#   third_party/skia-wasm/out/lib*.a             static libraries
#   third_party/skia-wasm/out/defines.txt        defines consumers must compile with
#   third_party/skia-wasm/out/args.gn            the gn args used
#
# The gn args follow modules/canvaskit/compile.sh (CPU variant): FreeType
# (bundled), HarfBuzz + ICU shaping (skshaper/skunicode/skparagraph), PNG/JPEG/
# WebP/GIF decode, PNG encode, no fontconfig/expat/system libs, no PDF/SVG/
# Skottie/GPU. Optimised with -O3 (frame time beats a few hundred KB of wasm).
#
# Idempotent: when third_party/skia-wasm/.stamp matches the hash of this
# script the build is skipped, so CI can cache third_party/skia-wasm keyed on
# hashFiles('scripts/build-skia-wasm.sh', 'scripts/setup-emsdk.sh').
#
# Environment:
#   SKIA_SRC    Skia checkout (default third_party/skia-src; reused if present)
#   EMSDK       activated emsdk (default third_party/emsdk, see setup-emsdk.sh)
#   JOBS        ninja parallelism (default 2)
#   SKIA_WASM_DEBUG=1  is_debug build (for diagnosing crashes)
#   SKIA_WASM_SIMD=0   build without WebAssembly SIMD (-msimd128). SIMD is on by
#                      default (all current browsers support it); CMake detects
#                      it from out/args.gn and builds JSUI with SIMD too.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKIA_BRANCH="chrome/m124"
SKIA_SRC="${SKIA_SRC:-$ROOT/third_party/skia-src}"
OUT_DIR="${SKIA_WASM_OUT:-$ROOT/third_party/skia-wasm}"
EMSDK="${EMSDK:-$ROOT/third_party/emsdk}"
JOBS="${JOBS:-2}"
DEBUG="${SKIA_WASM_DEBUG:-0}"
SIMD="${SKIA_WASM_SIMD:-1}"

hash_inputs() { cat "$ROOT/scripts/build-skia-wasm.sh" "$ROOT/scripts/setup-emsdk.sh" | sha256sum | cut -d' ' -f1; }
STAMP_VALUE="$(hash_inputs)-debug$DEBUG-simd$SIMD"
if [ -f "$OUT_DIR/.stamp" ] && [ "$(cat "$OUT_DIR/.stamp")" = "$STAMP_VALUE" ] && [ -f "$OUT_DIR/out/libskia.a" ]; then
  echo "skia-wasm is up to date ($OUT_DIR)"
  exit 0
fi

# ------------------------------------------------------------------ emsdk
if [ ! -x "$EMSDK/upstream/emscripten/emcc" ]; then
  "$ROOT/scripts/setup-emsdk.sh"
fi

# ----------------------------------------------------------------- source
if [ ! -d "$SKIA_SRC/.git" ]; then
  echo "Cloning Skia $SKIA_BRANCH into $SKIA_SRC"
  git clone --depth 1 --branch "$SKIA_BRANCH" https://skia.googlesource.com/skia "$SKIA_SRC"
fi
cd "$SKIA_SRC"

# Only sync the third-party repos this configuration compiles (the full DEPS
# list includes dawn, angle, swiftshader, vulkan, ... several GB we never use).
python3 - <<'PY'
import re
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
  BUILD_DIR="out/jsui-wasm-debug"; IS_DEBUG=true; IS_OFFICIAL=false
else
  BUILD_DIR="out/jsui-wasm"; IS_DEBUG=false; IS_OFFICIAL=true
fi

ARGS="
is_debug=$IS_DEBUG
is_official_build=$IS_OFFICIAL
is_component_build=false
werror=false
$([ "$SIMD" = "1" ] && echo 'extra_cflags=["-msimd128"]' || true)
target_cpu=\"wasm\"
skia_emsdk_dir=\"$EMSDK\"

skia_enable_ganesh=false
skia_enable_graphite=false
skia_use_webgl=false
skia_use_webgpu=false
skia_use_dawn=false
skia_use_angle=false
skia_use_vulkan=false
skia_use_gl=false

skia_use_fontconfig=false
skia_use_freetype=true
skia_use_system_freetype2=false
skia_enable_fontmgr_custom_empty=true
skia_enable_fontmgr_custom_embedded=false
skia_enable_fontmgr_custom_directory=false
skia_use_expat=false

skia_use_icu=true
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
skia_use_libheif=false
skia_use_dng_sdk=false
skia_use_piex=false
skia_use_lua=false

skia_enable_pdf=false
skia_enable_svg=false
skia_enable_skottie=false
skia_enable_tools=false
skia_enable_spirv_validation=false
"
[ "$SIMD" = "1" ] && BUILD_DIR="$BUILD_DIR-simd"
ARGS="$(echo "$ARGS" | sed '/^$/d' | tr '\n' ' ')"
echo "gn args: $ARGS"
bin/gn gen "$BUILD_DIR" --args="$ARGS"

TARGETS="skia skparagraph skshaper skunicode"
nice ninja -C "$BUILD_DIR" -j "$JOBS" $TARGETS

# ---------------------------------------------------------------- install
rm -rf "$OUT_DIR.tmp"
mkdir -p "$OUT_DIR.tmp/out"
copy_headers() {  # copy_headers <src dir> (relative to SKIA_SRC)
  [ -d "$1" ] || return 0
  (find "$1" -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.inc' \) -print0 |
    xargs -0 -I{} cp --parents {} "$OUT_DIR.tmp/")
}
copy_headers include
copy_headers src
for m in modules/*/; do
  copy_headers "${m%/}/include"
  copy_headers "${m%/}/src"
done
copy_headers third_party/externals/harfbuzz/src
copy_headers third_party/externals/icu/source/common/unicode
copy_headers third_party/externals/icu/source/i18n/unicode
copy_headers third_party/externals/freetype/include
cp LICENSE "$OUT_DIR.tmp/"

find "$BUILD_DIR" -maxdepth 1 -name 'lib*.a' -exec cp {} "$OUT_DIR.tmp/out/" \;
cp "$BUILD_DIR/args.gn" "$OUT_DIR.tmp/out/args.gn"

# Defines that dependents of these targets get in gn (their public configs:
# skia_public incl. wasm_defines such as SK_FORCE_8_BYTE_ALIGNMENT, plus
# SK_SHAPER_HARFBUZZ_AVAILABLE, SK_UNICODE_ICU_IMPLEMENTATION, ...). They change
# public headers, so consumers must compile with exactly these.
: > "$OUT_DIR.tmp/out/defines.txt"
for target in //:skia //modules/skshaper:skshaper //modules/skunicode:skunicode //modules/skparagraph:skparagraph; do
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
echo "skia-wasm installed into $OUT_DIR"
