#!/usr/bin/env bash
# Builds Skia (chrome/m157, pinned) for desktop Linux / macOS / Windows with
# Graphite on Vulkan (Linux, Windows) or Metal (macOS) and on Dawn (WebGPU,
# header-only, see below), plus Ganesh (GL; Metal on macOS, Direct3D 12 on
# Windows) and the CPU raster, and installs
# headers + static libraries into third_party/skia-desktop in the layout
# cmake/Deps.cmake expects:
#
#   third_party/skia-desktop/include/...                  public headers (incl. include/third_party/vulkan)
#   third_party/skia-desktop/modules/*/include/...        module headers
#   third_party/skia-desktop/src/**/*.h                   private headers (VulkanMemoryAllocators, ...)
#   third_party/skia-desktop/third_party/externals/...    harfbuzz / icu headers
#   third_party/skia-desktop/dawn/include/...             Dawn's webgpu.h / webgpu_cpp.h (generated)
#   third_party/skia-desktop/dawn/REVISION                the Dawn commit those headers are from
#   third_party/skia-desktop/out/<os>-<cpu>/lib*.a        static libraries (*.lib on Windows)
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
#   Windows (x64, Git Bash; clang-cl from LLVM, MSVC + Windows SDK found by
#          gn): Graphite + Vulkan, Ganesh + GL + Direct3D 12 (Graphite has no
#          D3D backend), DirectWrite fonts, static CRT (/MT, clang-cl's
#          default). ICU loads icudtl.dat from beside the executable at run
#          time (SkLoadICU): the build installs it under
#          third_party/externals/icu/flutter/, where cmake/Deps.cmake finds it.
#   Both:  Graphite + Dawn (header-only). PNG / JPEG / WebP / GIF decode, PNG
#          encode; Skottie (Lottie); no PDF / SVG.
#   Vulkan headers are Skia's own (include/third_party/vulkan) and memory is
#   managed by the bundled VulkanMemoryAllocator; nothing Vulkan is linked
#   (the GDExtension gets vkGetInstanceProcAddr from the loader at run time).
#
# Dawn: Skia's own Dawn checkout (third_party/externals/dawn, at the revision
# m157's DEPS pins: DAWN_REVISION below) only provides the webgpu.h /
# webgpu_cpp.h headers, generated with Dawn's code generator. Dawn itself is
# neither built nor linked (scripts/patches/skia-m157-dawn-header-only.patch):
# whatever links these archives must provide the wgpu* entry points. JSUI
# resolves them at run time through the host's wgpuGetProcAddress
# (tools/dawn-shim, jsui_gpu_create_dawn_procs), e.g. from the godot-aw3
# Godot fork's WebGPU driver, which must link Dawn at exactly DAWN_REVISION
# (the webgpu.h ABI changes between revisions).
#
# Idempotent: an output whose .stamp matches the hash of this script and the
# Dawn patch is skipped, so CI caches third_party/skia-desktop keyed on
# hashFiles of both.
#
# Environment:
#   SKIA_SRC   Skia checkout (default third_party/skia-src-m157, shared with
#              build-skia-wasm-graphite.sh: the files that script patches are
#              reset to pristine first)
#   JOBS       ninja parallelism (default: all cores)
#   CC / CXX   compilers (Linux default clang / clang++)
#   CLANG_WIN  Windows: the LLVM install with clang-cl (default C:/Program Files/LLVM)
#   SKIA_DESKTOP_OUT  install dir (default third_party/skia-desktop)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKIA_BRANCH="chrome/m157"
SKIA_COMMIT="3c6589d4882124ecb315db04227eccd81fe0b435"  # same as build-skia-wasm-graphite.sh / build-skia-ios.sh
SKIA_SRC="${SKIA_SRC:-$ROOT/third_party/skia-src-m157}"
# What SKIA_COMMIT's DEPS pins (checked below): the Dawn a host must link.
DAWN_REVISION="ada567700e13aecf7989d78b21f7d330b522ea80"
DAWN_PATCH="$ROOT/scripts/patches/skia-m157-dawn-header-only.patch"
OUT_DIR="${SKIA_DESKTOP_OUT:-$ROOT/third_party/skia-desktop}"

case "$(uname -s)" in
  Linux) os=linux; JOBS="${JOBS:-$(nproc)}" ;;
  Darwin) os=macos; JOBS="${JOBS:-$(sysctl -n hw.ncpu)}" ;;
  MINGW*|MSYS*|CYGWIN*) os=win; JOBS="${JOBS:-${NUMBER_OF_PROCESSORS:-4}}" ;;
  *) echo "build-skia-desktop.sh supports Linux, macOS and Windows (Git Bash)" >&2; exit 1 ;;
esac
case "$(uname -m)" in
  x86_64|amd64) cpu=x64 ;;
  arm64|aarch64) cpu=arm64 ;;
  *) echo "unsupported CPU $(uname -m)" >&2; exit 1 ;;
esac
CPU="${SKIA_CPU:-$cpu}"
slice="$os-$CPU"
dest="$OUT_DIR/out/$slice"
if [ "$os" = win ]; then
  lib_main=skia.lib
  CLANG_WIN="${CLANG_WIN:-C:/Program Files/LLVM}"
  NM="$CLANG_WIN/bin/llvm-nm.exe"
else
  lib_main=libskia.a
  NM=nm
fi
PYTHON="${PYTHON:-$(command -v python3 || command -v python)}"

hash_cmd() { if command -v sha256sum >/dev/null; then sha256sum; else shasum -a 256; fi; }
STAMP_VALUE="$(cat "$ROOT/scripts/build-skia-desktop.sh" "$DAWN_PATCH" | hash_cmd | cut -d' ' -f1)"
if [ -f "$dest/.stamp" ] && [ "$(cat "$dest/.stamp")" = "$STAMP_VALUE" ] && [ -f "$dest/$lib_main" ] && [ -d "$OUT_DIR/include" ] \
   && [ -f "$OUT_DIR/dawn/include/dawn/webgpu.h" ]; then
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
git apply "$DAWN_PATCH"
grep -q "third_party/externals/dawn\".*dawn.git@$DAWN_REVISION\"" DEPS || {
  echo "Skia's DEPS does not pin Dawn $DAWN_REVISION: update DAWN_REVISION (and the host's Dawn)" >&2; exit 1; }

# Only the third-party repos this configuration compiles (Dawn and its code
# generator's jinja2 / markupsafe for the headers).
"$PYTHON" - "$os" <<'PY'
import sys
keep = {"brotli", "freetype", "harfbuzz", "icu", "libjpeg-turbo", "libpng", "libwebp", "wuffs", "zlib",
        "vulkanmemoryallocator", "dawn", "jinja2", "markupsafe"}
if sys.argv[1] == "win":  # Ganesh Direct3D: its allocator, and SPIRV-Cross for HLSL
    keep |= {"d3d12allocator", "spirv-cross"}
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
  "$PYTHON" tools/git-sync-deps
[ -x bin/gn ] || [ -f bin/gn.exe ] || "$PYTHON" bin/fetch-gn
command -v ninja >/dev/null || { echo "ninja not found" >&2; exit 1; }

# ------------------------------------------------------------ Dawn headers
# webgpu.h / webgpu_cpp.h, generated from dawn.json as Dawn's own build does
# (its CMake / GN headers targets), for Graphite's Dawn backend and JSUI's
# entry-point shim.
DAWN_SRC="$SKIA_SRC/third_party/externals/dawn"
DAWN_GEN="$SKIA_SRC/out/jsui-dawn-headers-$slice"
rm -rf "$DAWN_GEN"
"$PYTHON" "$DAWN_SRC/generator/dawn_json_generator.py" \
  --template-dir "$DAWN_SRC/generator/templates" \
  --root-dir "$DAWN_SRC" \
  --dawn-json "$DAWN_SRC/src/dawn/dawn.json" \
  --wire-json "$DAWN_SRC/src/dawn/dawn_wire.json" \
  --jinja2-path "$SKIA_SRC/third_party/externals/jinja2" \
  --markupsafe-path "$SKIA_SRC/third_party/externals/markupsafe" \
  --targets headers,cpp_headers \
  --output-dir "$DAWN_GEN"
[ -f "$DAWN_GEN/include/dawn/webgpu.h" ] && [ -f "$DAWN_GEN/include/dawn/webgpu_cpp.h" ] || {
  echo "Dawn header generation failed (no include/dawn/webgpu.h in $DAWN_GEN)" >&2; exit 1; }
# The hand-written headers (webgpu/webgpu_cpp.h, ... include the generated ones).
cp -R "$DAWN_SRC/include/." "$DAWN_GEN/include/"

# ------------------------------------------------------------------ build
# gn wants native paths (Git Bash's /d/a/... would become d/a/...).
DAWN_GEN_GN="$DAWN_GEN/include"
[ "$os" = win ] && DAWN_GEN_GN="$(cygpath -m "$DAWN_GEN_GN")"
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
skia_use_dawn=true
skia_dawn_header_only_include_dir=\"$DAWN_GEN_GN\"

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
skia_enable_skottie=true
skia_enable_tools=false
skia_enable_spirv_validation=false
skia_use_partition_alloc=false
skia_use_perfetto=false
"
if [ "$os" = win ]; then
  OS_ARGS="
clang_win=\"$CLANG_WIN\"
skia_use_vulkan=true
skia_use_direct3d=true
skia_enable_direct3d_debug_layer=false
skia_use_metal=false
skia_use_fontconfig=false
skia_use_freetype=false
"
elif [ "$os" = linux ]; then
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
rm -f "$BUILD_DIR"/lib*.a "$BUILD_DIR"/*.lib
TARGETS="skia skparagraph skshaper skunicode skottie sksg"
# shellcheck disable=SC2086
nice ninja -C "$BUILD_DIR" -j "$JOBS" $TARGETS

rm -rf "$dest"
mkdir -p "$dest"
find "$BUILD_DIR" -maxdepth 1 \( -name 'lib*.a' -o -name '*.lib' \) -exec cp {} "$dest/" \;
cp "$BUILD_DIR/args.gn" "$dest/args.gn"
# Defines dependents get in gn (public + all-dependent configs: SK_GANESH,
# SK_GRAPHITE, SK_VULKAN, SK_METAL, SK_DAWN, SK_GL,
# SK_SHAPER_HARFBUZZ_AVAILABLE, SK_USE_INTERNAL_VULKAN_HEADERS, ...). They
# change public headers, so consumers must compile with exactly these.
: > "$dest/defines.txt"
for target in //:skia //modules/skshaper:skshaper //modules/skunicode:skunicode //modules/skparagraph:skparagraph //modules/skottie:skottie; do
  for config in $(bin/gn desc "$BUILD_DIR" "$target" public_configs) \
                $(bin/gn desc "$BUILD_DIR" "$target" all_dependent_configs); do
    { bin/gn desc "$BUILD_DIR" "$config" defines 2>/dev/null || true; } \
      | tr -d '\r' | grep -E '^[A-Za-z_][A-Za-z0-9_]*(=.*)?$' >> "$dest/defines.txt" || true
  done
done
sort -u -o "$dest/defines.txt" "$dest/defines.txt"
echo "[$slice] public defines:"; cat "$dest/defines.txt"
# No Dawn library may be linked: the archives must not define wgpu* entry points.
wgpu_defs="$("$NM" -g --defined-only "$dest"/*.a "$dest"/*.lib 2>/dev/null | grep -E ' T _?wgpu[A-Z]' || true)"
if [ -n "$wgpu_defs" ]; then
  echo "$wgpu_defs" | head -5 >&2
  echo "[$slice] a Skia archive defines wgpu* entry points (Dawn implementation linked in)" >&2; exit 1
fi

# ---------------------------------------------------------------- headers
rm -rf "$OUT_DIR/include" "$OUT_DIR/src" "$OUT_DIR/modules" "$OUT_DIR/third_party" "$OUT_DIR/dawn"
copy_headers() {  # copy_headers <src dir> (relative to SKIA_SRC); no rsync on Windows
  [ -d "$1" ] || return 0
  "$PYTHON" - "$1" "$OUT_DIR/$1" <<'PY'
import os, shutil, sys
src, dst = sys.argv[1], sys.argv[2]
for d, _, files in os.walk(src):
    for f in files:
        if f.endswith((".h", ".hpp", ".inc")):
            out = os.path.join(dst, os.path.relpath(d, src))
            os.makedirs(out, exist_ok=True)
            shutil.copy2(os.path.join(d, f), out)
PY
}
copy_headers include
copy_headers src
copy_headers modules
copy_headers third_party/externals/harfbuzz/src
copy_headers third_party/externals/icu/source/common/unicode
copy_headers third_party/externals/icu/source/i18n/unicode
cp LICENSE "$OUT_DIR/"
if [ "$os" = win ]; then
  # The ICU data SkLoadICU reads at run time (the small Flutter set, as the
  # m124 prebuilt shipped): hosts copy it beside their executable.
  mkdir -p "$OUT_DIR/third_party/externals/icu/flutter"
  cp third_party/externals/icu/flutter/icudtl.dat "$OUT_DIR/third_party/externals/icu/flutter/"
fi
mkdir -p "$OUT_DIR/dawn"
cp -R "$DAWN_GEN/include" "$OUT_DIR/dawn/include"
cp "$DAWN_SRC/LICENSE" "$OUT_DIR/dawn/LICENSE"
echo "$DAWN_REVISION" > "$OUT_DIR/dawn/REVISION"
echo "$STAMP_VALUE" > "$dest/.stamp"
ls -la "$dest"
echo "skia-desktop installed into $OUT_DIR ($slice)"
