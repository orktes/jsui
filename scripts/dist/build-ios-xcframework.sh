#!/usr/bin/env bash
# Builds JSUI.xcframework for iOS: one static library per slice (device
# arm64, simulator arm64) that holds JSUI, QuickJS-ng, Yoga and the iOS Skia
# (Ganesh on Metal), plus include/jsui/jsui.h. Builds the Skia slices first
# when they are missing (scripts/build-skia-ios.sh, long).
#
#   scripts/dist/build-ios-xcframework.sh <out dir>
#     -> <out dir>/JSUI.xcframework
#
# An app links it with the system frameworks Skia and JSUI use:
#   CoreFoundation CoreGraphics CoreText Foundation UIKit Metal, libc++, libz
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
mkdir -p "$1"
OUT="$(cd "$1" && pwd)"
IOS_MIN="${IOS_MIN:-15.0}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
SLICES="iphoneos-arm64 iphonesimulator-arm64"

SLICES="$SLICES" IOS_MIN="$IOS_MIN" "$ROOT/scripts/build-skia-ios.sh"

args=()
headers="$OUT/.headers"
rm -rf "$headers" "$OUT/JSUI.xcframework"
mkdir -p "$headers/jsui"
cp "$ROOT/include/jsui/jsui.h" "$headers/jsui/"
cat > "$headers/module.modulemap" <<'MAP'
module JSUI {
  header "jsui/jsui.h"
  export *
}
MAP

for slice in $SLICES; do
  sdk="${slice%-*}"
  build="$ROOT/build-dist-$slice"
  cmake -S "$ROOT" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$sdk" -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_MIN" -DJSUI_BUILD_IOS_APP=OFF ${CMAKE_EXTRA_ARGS:-}
  cmake --build "$build" -j "$JOBS" --target jsui
  # Everything the app would otherwise link one by one, in one archive.
  # (every static library of the build: jsui, QuickJS, Yoga, the WOFF2 / zlib decoders, ...)
  libs=()
  for l in "$build"/lib*.a; do libs+=("$l"); done
  for l in "$ROOT/third_party/skia-ios/out/$slice"/lib*.a; do libs+=("$l"); done
  mkdir -p "$build/dist"
  libtool -static -no_warning_for_no_symbols -o "$build/dist/libjsui.a" "${libs[@]}"
  ls -l "$build/dist/libjsui.a"
  args+=(-library "$build/dist/libjsui.a" -headers "$headers")
done

xcodebuild -create-xcframework "${args[@]}" -output "$OUT/JSUI.xcframework"
rm -rf "$headers"

# Link check: tests/shared/consumer.c against the simulator slice with only
# the documented frameworks (every symbol resolves, nothing else needed).
sim="$(find "$OUT/JSUI.xcframework" -maxdepth 1 -type d -name 'ios-arm64*simulator' | head -1)"
xcrun --sdk iphonesimulator clang -target "arm64-apple-ios$IOS_MIN-simulator" \
  -I "$sim/Headers" "$ROOT/tests/shared/consumer.c" "$sim/libjsui.a" \
  -framework CoreFoundation -framework CoreGraphics -framework CoreText -framework Foundation \
  -framework UIKit -framework Metal -lc++ -lz -o "$ROOT/build-dist-consumer-ios"
echo "link check: ok"
find "$OUT/JSUI.xcframework" -maxdepth 2
