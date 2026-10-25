#!/usr/bin/env bash
# Builds Skia (chrome/m157, the same pin and patch as
# scripts/build-skia-wasm-graphite.sh) for *threaded* WebAssembly and installs
# it into third_party/skia-wasm-mt, for JSUI inside a threaded Emscripten
# program: the Godot integration statically linked into the godot-aw3 fork's
# web export template (integrations/godot/README.md, "Threaded web build").
#
# What differs from build-skia-wasm-graphite.sh:
#
#   * -pthread (atomics + bulk-memory): objects without them cannot be linked
#     into a program with shared memory (wasm-ld refuses).
#   * -sSUPPORT_LONGJMP=wasm, as Godot's web build: libpng / libjpeg-turbo use
#     setjmp, and the two longjmp ABIs do not mix in one link.
#   * The toolchain of the program it is linked into: emsdk 4.0.20 (godot-aw3's
#     misc/web-export/wasm.sh EMSDK_VERSION) and that emsdk's own Emdawnwebgpu
#     port (--use-port=emdawnwebgpu, Dawn release v20251002.162335), the one
#     the fork compiles its WebGPU driver against. The Skia libraries, JSUI and
#     the engine then share one compiler, one libc++ and one webgpu.h.
#   * Graphite (WebGPU) + CPU raster only: no Ganesh / WebGL, which the
#     engine's WebGPU renderer has no use for.
#
# The non-threaded builds (build-skia-wasm.sh, build-skia-wasm-graphite.sh)
# and the npm web modules are untouched.
#
# Idempotent: skipped when third_party/skia-wasm-mt/.stamp matches the hash of
# the inputs (CI caches the directory keyed on the same files).
#
# Environment:
#   SKIA_SRC       Skia checkout (default third_party/skia-src-m157; shared
#                  with build-skia-wasm-graphite.sh, built into its own out dir)
#   EMSDK_VERSION  default 4.0.20 (must match the engine's emsdk)
#   EMSDK          emsdk (default third_party/emsdk-$EMSDK_VERSION)
#   EMDAWNWEBGPU_PORT  --use-port value (default emdawnwebgpu: the emsdk's own)
#   SKIA_WASM_OUT  install dir (default third_party/skia-wasm-mt)
#   JOBS           ninja parallelism (default 2)
#   SKIA_WASM_DEBUG=1  is_debug build
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKIA_BRANCH="chrome/m157"
SKIA_COMMIT="3c6589d4882124ecb315db04227eccd81fe0b435"
EMSDK_VERSION="${EMSDK_VERSION:-4.0.20}"
SKIA_SRC="${SKIA_SRC:-$ROOT/third_party/skia-src-m157}"
OUT_DIR="${SKIA_WASM_OUT:-$ROOT/third_party/skia-wasm-mt}"
EMSDK="${EMSDK:-$ROOT/third_party/emsdk-$EMSDK_VERSION}"
PATCH="$ROOT/scripts/patches/skia-m157-emdawnwebgpu.patch"
PORT="${EMDAWNWEBGPU_PORT:-emdawnwebgpu}"
JOBS="${JOBS:-2}"
DEBUG="${SKIA_WASM_DEBUG:-0}"

hash_inputs() {
  cat "$ROOT/scripts/build-skia-wasm-mt.sh" "$ROOT/scripts/setup-emsdk.sh" "$PATCH" | sha256sum | cut -d' ' -f1
}
STAMP_VALUE="$(hash_inputs)-debug$DEBUG-emsdk$EMSDK_VERSION-$PORT"
if [ -f "$OUT_DIR/.stamp" ] && [ "$(cat "$OUT_DIR/.stamp")" = "$STAMP_VALUE" ] && [ -f "$OUT_DIR/out/libskia.a" ]; then
  echo "skia-wasm-mt is up to date ($OUT_DIR)"
  exit 0
fi

EMSDK_VERSION="$EMSDK_VERSION" EMSDK_DIR="$EMSDK" "$ROOT/scripts/setup-emsdk.sh"

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
if [ "${SKIA_PATCH_DEV:-0}" != "1" ]; then
  git checkout -q -- src include third_party/dawn modules/skcms gn
  git apply "$PATCH"
fi

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
  BUILD_DIR="out/jsui-wasm-mt-debug"; IS_DEBUG=true; IS_OFFICIAL=false
else
  BUILD_DIR="out/jsui-wasm-mt"; IS_DEBUG=false; IS_OFFICIAL=true
fi

ARGS="
is_debug=$IS_DEBUG
is_official_build=$IS_OFFICIAL
is_component_build=false
werror=false
target_cpu=\"wasm\"
skia_emsdk_dir=\"$EMSDK\"
extra_cflags=[\"--use-port=$PORT\",\"-msimd128\",\"-msse4.1\",\"-pthread\",\"-sSUPPORT_LONGJMP=wasm\"]

skia_enable_ganesh=false
skia_use_gl=false
skia_use_webgl=false
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
[ -n "${SKIA_TARGETS:-}" ] && exit 0

# ---------------------------------------------------------------- install
rm -rf "$OUT_DIR.tmp"
mkdir -p "$OUT_DIR.tmp/out"
copy_headers() {
  [ -d "$1" ] || return 0
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

for lib in "$BUILD_DIR"/lib*.a; do
  name="$(basename "$lib")"
  cp "$lib" "$OUT_DIR.tmp/out/${name%.wasm.a}.a"
done
cp "$BUILD_DIR/args.gn" "$OUT_DIR.tmp/out/args.gn"
echo "$EMSDK_VERSION" > "$OUT_DIR.tmp/out/emsdk-version.txt"
echo "$SKIA_BRANCH $SKIA_COMMIT" > "$OUT_DIR.tmp/out/skia-version.txt"
# The Emdawnwebgpu release the libraries were compiled against (its Dawn
# revision is what JSUI reports as jsui_dawn_revision() on the web).
source "$EMSDK/emsdk_env.sh" > /dev/null 2>&1
echo 'int main(){}' > "$OUT_DIR.tmp/port-probe.c"
emcc --use-port="$PORT" -c "$OUT_DIR.tmp/port-probe.c" -o /dev/null
rm "$OUT_DIR.tmp/port-probe.c"
if [ -f "$PORT" ]; then
  port_dir="$(dirname "$PORT")"
else
  port_dir="$(em-config CACHE)/ports/emdawnwebgpu/emdawnwebgpu_pkg"
fi
cp "$port_dir/VERSION.txt" "$OUT_DIR.tmp/out/emdawnwebgpu-version.txt"

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
echo "skia-wasm-mt installed into $OUT_DIR"
