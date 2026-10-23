#!/usr/bin/env bash
# Builds Skia (chrome/m157, pinned) for iOS (Ganesh on Metal, CPU raster) and installs headers +
# static libraries into third_party/skia-ios in the layout cmake/Deps.cmake
# expects:
#
#   third_party/skia-ios/include/...                     public headers
#   third_party/skia-ios/modules/*/include/...           module headers
#   third_party/skia-ios/src/**/*.h                      private headers some modules include
#   third_party/skia-ios/third_party/externals/...       harfbuzz / icu headers
#   third_party/skia-ios/out/<sdk>-<cpu>/lib*.a          static libraries per slice
#   third_party/skia-ios/out/<sdk>-<cpu>/args.gn         the gn args used
#   third_party/skia-ios/out/<sdk>-<cpu>/defines.txt     defines consumers must compile with
#
# Slices (SLICES, space separated, default "iphoneos-arm64 iphonesimulator-arm64"):
#   iphoneos-arm64          devices
#   iphonesimulator-arm64   simulator on Apple silicon Macs
#   iphonesimulator-x64     simulator on Intel Macs
#
# Configuration: Ganesh + Metal (skia_use_metal), no GL / Vulkan / Graphite;
# CoreText fonts (the platform font manager, like the macOS build); bundled
# ICU + HarfBuzz for skparagraph shaping (ICU data is linked into libicu);
# PNG / JPEG / WebP / GIF decode, PNG encode. Needs macOS with Xcode.
#
# Idempotent: a slice whose .stamp matches the hash of this script is
# skipped, so CI can cache third_party/skia-ios keyed on
# hashFiles('scripts/build-skia-ios.sh').
#
# Environment:
#   SKIA_SRC   Skia checkout (default third_party/skia-src-ios; reused if present)
#   SLICES     see above
#   JOBS       ninja parallelism (default: all cores)
#   IOS_MIN    minimum iOS version (default 15.0)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKIA_BRANCH="chrome/m157"
SKIA_COMMIT="3c6589d4882124ecb315db04227eccd81fe0b435"  # same as build-skia-wasm-graphite.sh
SKIA_SRC="${SKIA_SRC:-$ROOT/third_party/skia-src-ios}"
OUT_DIR="${SKIA_IOS_OUT:-$ROOT/third_party/skia-ios}"
SLICES="${SLICES:-iphoneos-arm64 iphonesimulator-arm64}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
IOS_MIN="${IOS_MIN:-15.0}"

if [ "$(uname)" != "Darwin" ]; then
  echo "build-skia-ios.sh needs macOS with Xcode" >&2
  exit 1
fi

STAMP_VALUE="$(cat "$ROOT/scripts/build-skia-ios.sh" | shasum -a 256 | cut -d' ' -f1)-ios$IOS_MIN"
todo=""
for slice in $SLICES; do
  s="$OUT_DIR/out/$slice"
  if [ -f "$s/.stamp" ] && [ "$(cat "$s/.stamp")" = "$STAMP_VALUE" ] && [ -f "$s/libskia.a" ] && [ -d "$OUT_DIR/include" ]; then
    echo "skia-ios $slice is up to date"
  else
    todo="$todo $slice"
  fi
done
[ -z "$todo" ] && exit 0

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

# Only sync the third-party repos this configuration compiles.
python3 - <<'PY'
keep = {"brotli", "harfbuzz", "icu", "libjpeg-turbo", "libpng", "libwebp", "wuffs", "zlib"}
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
command -v ninja >/dev/null || { echo "ninja not found (brew install ninja)" >&2; exit 1; }

# ------------------------------------------------------------------ build
COMMON_ARGS="
is_debug=false
is_official_build=true
is_component_build=false
werror=false
target_os=\"ios\"
ios_min_target=\"$IOS_MIN\"
skia_ios_use_signing=false

skia_enable_ganesh=true
skia_use_metal=true
skia_use_gl=false
skia_enable_graphite=false
skia_use_dawn=false
skia_use_vulkan=false
skia_use_angle=false

skia_use_fontconfig=false
skia_use_freetype=false
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

TARGETS="skia skparagraph skshaper skunicode skottie sksg"
for slice in $todo; do
  sdk="${slice%-*}"
  cpu="${slice##*-}"
  sim=false
  [ "$sdk" = "iphonesimulator" ] && sim=true
  BUILD_DIR="out/jsui-$slice"
  ARGS="$COMMON_ARGS
target_cpu=\"$cpu\"
ios_use_simulator=$sim
xcode_sysroot=\"$(xcrun --sdk "$sdk" --show-sdk-path)\"
"
  ARGS="$(echo "$ARGS" | sed '/^$/d' | tr '\n' ' ')"
  echo "[$slice] gn args: $ARGS"
  bin/gn gen "$BUILD_DIR" --args="$ARGS"
  # Archives are copied by glob below: drop ones a previous Skia version left
  # behind (m157 turned skunicode into a group; a stale libskunicode.a
  # duplicates skunicode_icu). Ninja only re-archives the rest.
  rm -f "$BUILD_DIR"/lib*.a
  # shellcheck disable=SC2086
  ninja -C "$BUILD_DIR" -j "$JOBS" $TARGETS

  dest="$OUT_DIR/out/$slice"
  rm -rf "$dest"
  mkdir -p "$dest"
  find "$BUILD_DIR" -maxdepth 1 -name 'lib*.a' -exec cp {} "$dest/" \;
  cp "$BUILD_DIR/args.gn" "$dest/args.gn"
  # Defines dependents get in gn (public + all-dependent configs: SK_GANESH, SK_METAL,
  # SK_SHAPER_HARFBUZZ_AVAILABLE, ...). They change public headers, so
  # consumers must compile with exactly these.
  : > "$dest/defines.txt"
  for target in //:skia //modules/skshaper:skshaper //modules/skunicode:skunicode //modules/skparagraph:skparagraph //modules/skottie:skottie; do
    for config in $(bin/gn desc "$BUILD_DIR" "$target" public_configs) \
                  $(bin/gn desc "$BUILD_DIR" "$target" all_dependent_configs); do
      # (a config without defines makes gn desc print a message and fail)
      { bin/gn desc "$BUILD_DIR" "$config" defines 2>/dev/null || true; } \
        | grep -E '^[A-Za-z_][A-Za-z0-9_]*(=.*)?$' >> "$dest/defines.txt" || true
    done
  done
  sort -u -o "$dest/defines.txt" "$dest/defines.txt"
  echo "[$slice] public defines:"; cat "$dest/defines.txt"
  echo "$STAMP_VALUE" > "$dest/.stamp"
done

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
ls -la "$OUT_DIR/out"/*
echo "skia-ios installed into $OUT_DIR"
