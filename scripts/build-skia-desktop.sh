#!/usr/bin/env bash
# Builds Skia (chrome/m157, pinned) for desktop Linux / macOS with Graphite on
# Vulkan (Linux) or Metal (macOS), plus Ganesh (GL; Metal on macOS) and the
# CPU raster, and installs headers + static libraries into
# third_party/skia-desktop in the layout cmake/Deps.cmake expects:
#
#   third_party/skia-desktop/include/...                  public headers (incl. include/third_party/vulkan)
#   third_party/skia-desktop/modules/*/include/...        module headers
#   third_party/skia-desktop/src/**/*.h                   private headers (VulkanMemoryAllocators, ...)
#   third_party/skia-desktop/third_party/externals/...    harfbuzz / icu headers
#   third_party/skia-desktop/out/<os>-<cpu>/lib*.a        static libraries
#   third_party/skia-desktop/out/<os>-<cpu>/args.gn       the gn args used
#   third_party/skia-desktop/out/<os>-<cpu>/defines.txt   defines consumers must compile with
#
# Use it with: cmake -DJSUI_SKIA_DIR=third_party/skia-desktop ... (the Godot
# GDExtension's GPU path needs it: the m124 prebuilt has no Vulkan / Metal and
# no Graphite). The default native build keeps the prebuilt m124.
#
# Configuration:
#   Linux: Graphite + Vulkan, Ganesh + GL (+ Vulkan), fontconfig + bundled
#          FreeType (as the prebuilt), bundled ICU + HarfBuzz.
#   macOS: Graphite + Metal, Ganesh + GL + Metal, CoreText fonts.
#   Both:  PNG / JPEG / WebP / GIF decode, PNG encode; no PDF / SVG / Skottie.
#   Vulkan headers are Skia's own (include/third_party/vulkan) and memory is
#   managed by the bundled VulkanMemoryAllocator; nothing Vulkan is linked
#   (the GDExtension gets vkGetInstanceProcAddr from the loader at run time).
#
# Idempotent: an output whose .stamp matches the hash of this script is
# skipped, so CI caches third_party/skia-desktop keyed on
# hashFiles('scripts/build-skia-desktop.sh').
#
# Environment:
#   SKIA_SRC   Skia checkout (default third_party/skia-src-m157, shared with
#              build-skia-wasm-graphite.sh: the files that script patches are
#              reset to pristine first)
#   JOBS       ninja parallelism (default: all cores)
#   CC / CXX   compilers (Linux default clang / clang++)
#   SKIA_DESKTOP_OUT  install dir (default third_party/skia-desktop)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKIA_BRANCH="chrome/m157"
SKIA_COMMIT="3c6589d4882124ecb315db04227eccd81fe0b435"  # same as build-skia-wasm-graphite.sh / build-skia-ios.sh
SKIA_SRC="${SKIA_SRC:-$ROOT/third_party/skia-src-m157}"
OUT_DIR="${SKIA_DESKTOP_OUT:-$ROOT/third_party/skia-desktop}"

case "$(uname -s)" in
  Linux) os=linux; JOBS="${JOBS:-$(nproc)}" ;;
  Darwin) os=macos; JOBS="${JOBS:-$(sysctl -n hw.ncpu)}" ;;
  *) echo "build-skia-desktop.sh supports Linux and macOS" >&2; exit 1 ;;
esac
case "$(uname -m)" in
  x86_64|amd64) cpu=x64 ;;
  arm64|aarch64) cpu=arm64 ;;
  *) echo "unsupported CPU $(uname -m)" >&2; exit 1 ;;
esac
CPU="${SKIA_CPU:-$cpu}"
slice="$os-$CPU"
dest="$OUT_DIR/out/$slice"

hash_cmd() { if command -v sha256sum >/dev/null; then sha256sum; else shasum -a 256; fi; }
STAMP_VALUE="$(hash_cmd < "$ROOT/scripts/build-skia-desktop.sh" | cut -d' ' -f1)"
if [ -f "$dest/.stamp" ] && [ "$(cat "$dest/.stamp")" = "$STAMP_VALUE" ] && [ -f "$dest/libskia.a" ] && [ -d "$OUT_DIR/include" ]; then
  echo "skia-desktop $slice is up to date"
  exit 0
fi

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
# The wasm Graphite build patches these in the same checkout: build pristine.
git checkout -q -- src include third_party/dawn modules/skcms gn

# Only the third-party repos this configuration compiles.
python3 - <<'PY'
keep = {"brotli", "freetype", "harfbuzz", "icu", "libjpeg-turbo", "libpng", "libwebp", "wuffs", "zlib",
        "vulkanmemoryallocator"}
d = {}
with open("DEPS") as f:
    exec("def Var(x): return vars[x]\n" + f.read(), d)
deps = {k: v for k, v in d["deps"].items()
        if isinstance(v, str) and k.split("/")[-1] in keep and k.startswith("third_party/externals/")}
missing = keep - {k.split("/")[-1] for k in deps}
assert not missing, f"DEPS is missing {missing}"
with open("DEPS.jsui-desktop", "w") as f:
    f.write("deps = " + repr(deps) + "\n")
PY
GIT_SYNC_DEPS_PATH="$SKIA_SRC/DEPS.jsui-desktop" GIT_SYNC_DEPS_SKIP_EMSDK=1 GIT_SYNC_DEPS_QUIET=1 \
  python3 tools/git-sync-deps
[ -x bin/gn ] || python3 bin/fetch-gn
command -v ninja >/dev/null || { echo "ninja not found" >&2; exit 1; }

# ------------------------------------------------------------------ build
COMMON_ARGS="
is_debug=false
is_official_build=true
is_component_build=false
werror=false
target_cpu=\"$CPU\"

skia_enable_ganesh=true
skia_use_gl=true
skia_use_angle=false
skia_enable_graphite=true
skia_use_dawn=false

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
skia_use_expat=false

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
skia_enable_skottie=false
skia_enable_tools=false
skia_enable_spirv_validation=false
skia_use_partition_alloc=false
skia_use_perfetto=false
"
if [ "$os" = linux ]; then
  OS_ARGS="
cc=\"${CC:-clang}\"
cxx=\"${CXX:-clang++}\"
skia_use_vulkan=true
skia_use_metal=false
skia_use_fontconfig=true
skia_use_freetype=true
skia_use_system_freetype2=false
skia_enable_fontmgr_custom_empty=true
"
else
  OS_ARGS="
skia_use_vulkan=false
skia_use_metal=true
skia_use_fontconfig=false
skia_use_freetype=false
"
fi
BUILD_DIR="out/jsui-desktop-$slice"
ARGS="$(printf '%s\n%s\n' "$COMMON_ARGS" "$OS_ARGS" | sed '/^$/d' | tr '\n' ' ')"
echo "[$slice] gn args: $ARGS"
bin/gn gen "$BUILD_DIR" --args="$ARGS"
# Archives are copied by glob below: drop stale ones (ninja re-archives).
rm -f "$BUILD_DIR"/lib*.a
TARGETS="skia skparagraph skshaper skunicode"
# shellcheck disable=SC2086
nice ninja -C "$BUILD_DIR" -j "$JOBS" $TARGETS

rm -rf "$dest"
mkdir -p "$dest"
find "$BUILD_DIR" -maxdepth 1 -name 'lib*.a' -exec cp {} "$dest/" \;
cp "$BUILD_DIR/args.gn" "$dest/args.gn"
# Defines dependents get in gn (public + all-dependent configs: SK_GANESH,
# SK_GRAPHITE, SK_VULKAN, SK_METAL, SK_GL, SK_SHAPER_HARFBUZZ_AVAILABLE,
# SK_USE_INTERNAL_VULKAN_HEADERS, ...). They change public headers, so
# consumers must compile with exactly these.
: > "$dest/defines.txt"
for target in //:skia //modules/skshaper:skshaper //modules/skunicode:skunicode //modules/skparagraph:skparagraph; do
  for config in $(bin/gn desc "$BUILD_DIR" "$target" public_configs) \
                $(bin/gn desc "$BUILD_DIR" "$target" all_dependent_configs); do
    { bin/gn desc "$BUILD_DIR" "$config" defines 2>/dev/null || true; } \
      | grep -E '^[A-Za-z_][A-Za-z0-9_]*(=.*)?$' >> "$dest/defines.txt" || true
  done
done
sort -u -o "$dest/defines.txt" "$dest/defines.txt"
echo "[$slice] public defines:"; cat "$dest/defines.txt"

# ---------------------------------------------------------------- headers
rm -rf "$OUT_DIR/include" "$OUT_DIR/src" "$OUT_DIR/modules" "$OUT_DIR/third_party"
copy_headers() {  # copy_headers <src dir> (relative to SKIA_SRC)
  [ -d "$1" ] || return 0
  mkdir -p "$OUT_DIR/$1"
  rsync -a --prune-empty-dirs --include='*/' --include='*.h' --include='*.hpp' --include='*.inc' --exclude='*' \
    "$1/" "$OUT_DIR/$1/"
}
copy_headers include
copy_headers src
copy_headers modules
copy_headers third_party/externals/harfbuzz/src
copy_headers third_party/externals/icu/source/common/unicode
copy_headers third_party/externals/icu/source/i18n/unicode
cp LICENSE "$OUT_DIR/"
echo "$STAMP_VALUE" > "$dest/.stamp"
ls -la "$dest"
echo "skia-desktop installed into $OUT_DIR ($slice)"
