#!/usr/bin/env bash
# Collects one platform's release binaries from CMake build trees (configured
# with -DJSUI_BUILD_SHARED=ON, see the dist-* jobs in .github/workflows/ci.yml)
# into the layout the CI artifacts and the release archives use:
#
#   <out>/shared/include/jsui/jsui.h         C API
#   <out>/shared/lib/libjsui.so | .dylib     the shared library (Windows: lib/jsui.lib,
#   <out>/shared/bin/jsui.dll, icudtl.dat      bin/jsui.dll + the ICU data next to it)
#   <out>/shared/lib/cmake/jsui/             find_package(jsui) -> jsui::jsui
#   <out>/viewer/jsui-viewer[.exe]           the desktop viewer (+ icudtl.dat on Windows)
#   <out>/godot/libjsui_godot.<os>.<arch>.*  the Godot GDExtension library
#   <out>/unity/<plugin>                     the Unity package's native plugin
#
# A build tree that lacks a target (Unity on Windows) just leaves that part out.
#
#   scripts/dist/stage-native.sh <platform> <out> <build> [<build-x64>]
#
# platform: linux-x64 | linux-arm64 | windows-x64 | macos-universal. For
# macos-universal pass the arm64 and the x86_64 build trees: each binary is
# merged with lipo.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PLATFORM="$1"
OUT="$2"
BUILD="$3"
BUILD2="${4:-}"

rm -rf "$OUT"
mkdir -p "$OUT/shared/include/jsui" "$OUT/shared/lib/cmake/jsui" "$OUT/viewer" "$OUT/godot" "$OUT/unity"
cp "$ROOT/include/jsui/jsui.h" "$OUT/shared/include/jsui/"
cp "$ROOT/cmake/dist/jsuiConfig.cmake" "$OUT/shared/lib/cmake/jsui/"

# merge <out> <in-arm64> <in-x64>: a universal Mach-O.
merge() { lipo -create "$2" "$3" -output "$1"; lipo -info "$1"; }
# one <build> <relative path>: the path, which must exist.
need() { [ -e "$1" ] || { echo "stage-native: missing $1" >&2; exit 1; }; }

case "$PLATFORM" in
  linux-*)
    need "$BUILD/libjsui.so"
    cp "$BUILD/libjsui.so" "$OUT/shared/lib/"
    need "$BUILD/tools/jsui-viewer/jsui-viewer"
    cp "$BUILD/tools/jsui-viewer/jsui-viewer" "$OUT/viewer/"
    cp "$BUILD"/godot/libjsui_godot.linux.*.so "$OUT/godot/"
    [ ! -f "$BUILD/unity/libjsui_unity.so" ] || cp "$BUILD/unity/libjsui_unity.so" "$OUT/unity/"
    strip --strip-unneeded "$OUT/shared/lib/libjsui.so" "$OUT"/godot/*.so "$OUT/viewer/jsui-viewer"
    [ ! -f "$OUT/unity/libjsui_unity.so" ] || strip --strip-unneeded "$OUT/unity/libjsui_unity.so"
    ;;
  windows-x64)
    # Visual Studio generator: outputs are under <dir>/Release.
    win() { for f in "$BUILD/Release/$1" "$BUILD/$1" "$BUILD/$2/Release/$1" "$BUILD/$2/$1"; do
              [ -f "$f" ] && { echo "$f"; return; }; done
            echo "stage-native: missing $1 in $BUILD" >&2; exit 1; }
    mkdir -p "$OUT/shared/bin"
    cp "$(win jsui.dll .)" "$OUT/shared/bin/"
    cp "$(win jsui.lib .)" "$OUT/shared/lib/"
    viewer="$(win jsui-viewer.exe tools/jsui-viewer)"
    cp "$viewer" "$OUT/viewer/"
    cp "$BUILD"/godot/libjsui_godot.windows.*.dll "$OUT/godot/"
    # The prebuilt Skia's ICU reads its data from the module's directory.
    icu="$(dirname "$viewer")/icudtl.dat"
    if [ -f "$icu" ]; then
      cp "$icu" "$OUT/shared/bin/"
      cp "$icu" "$OUT/viewer/"
      cp "$icu" "$OUT/godot/"
    else
      echo "stage-native: no icudtl.dat next to the viewer" >&2; exit 1
    fi
    ;;
  macos-universal)
    [ -n "$BUILD2" ] || { echo "stage-native: macos-universal needs the arm64 and the x86_64 build" >&2; exit 1; }
    need "$BUILD/libjsui.dylib"; need "$BUILD2/libjsui.dylib"
    merge "$OUT/shared/lib/libjsui.dylib" "$BUILD/libjsui.dylib" "$BUILD2/libjsui.dylib"
    merge "$OUT/viewer/jsui-viewer" "$BUILD/tools/jsui-viewer/jsui-viewer" "$BUILD2/tools/jsui-viewer/jsui-viewer"
    g=libjsui_godot.macos.universal.dylib
    merge "$OUT/godot/$g" "$BUILD/godot/$g" "$BUILD2/godot/$g"
    if [ -d "$BUILD/unity/jsui_unity.bundle" ] && [ -d "$BUILD2/unity/jsui_unity.bundle" ]; then
      cp -R "$BUILD/unity/jsui_unity.bundle" "$OUT/unity/"
      b=jsui_unity.bundle/Contents/MacOS/jsui_unity
      merge "$OUT/unity/$b" "$BUILD/unity/$b" "$BUILD2/unity/$b"
    fi
    strip -x "$OUT/shared/lib/libjsui.dylib" "$OUT/godot/$g"
    strip "$OUT/viewer/jsui-viewer"
    # Stripping invalidates the linker's ad-hoc signature, which Apple
    # silicon requires to load anything: sign again (ad hoc; not notarized).
    for f in "$OUT/shared/lib/libjsui.dylib" "$OUT/godot/$g" "$OUT/viewer/jsui-viewer"; do
      codesign --force --sign - "$f"
    done
    [ ! -d "$OUT/unity/jsui_unity.bundle" ] || codesign --force --deep --sign - "$OUT/unity/jsui_unity.bundle"
    ;;
  *) echo "stage-native: unknown platform $PLATFORM" >&2; exit 1 ;;
esac

find "$OUT" -type f -exec ls -l {} + | awk '{print $5, $NF}'
