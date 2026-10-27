#!/usr/bin/env bash
# Builds the JSUI iOS example app (integrations/ios) for the simulator or a
# device, and optionally runs it in the simulator:
#
#   scripts/build-ios.sh                     # simulator build (build-ios-sim/)
#   RUN=1 scripts/build-ios.sh               # + boot a simulator, install, launch, screenshot
#   SDK=iphoneos scripts/build-ios.sh        # device build (build-ios/, ad-hoc signed)
#
# Environment:
#   SDK        iphonesimulator (default) or iphoneos
#   ARCH       arm64 (default) or x86_64 (Intel simulator)
#   APP        UI entry point (default integrations/godot/demo/ui/app.jsx)
#   STATE      shared state set before the bundle loads, as one JSON object
#              ({"key": value, ...}; shipped as ui/state.json)
#   DEVICE     simulator name for RUN=1 (default: first available iPhone)
#   SHOT       screenshot path for RUN=1 (default build-ios-sim/screenshot.png)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${SDK:-iphonesimulator}"
ARCH="${ARCH:-arm64}"
APP="${APP:-$ROOT/integrations/godot/demo/ui/app.jsx}"
if [ "$SDK" = "iphonesimulator" ]; then BUILD="${BUILD:-$ROOT/build-ios-sim}"; else BUILD="${BUILD:-$ROOT/build-ios}"; fi
CPU="$ARCH"; [ "$ARCH" = "x86_64" ] && CPU=x64

SLICES="$SDK-$CPU" "$ROOT/scripts/build-skia-ios.sh"

# UI bundle
[ -d "$ROOT/node_modules" ] || (cd "$ROOT" && npm ci)
UI="$ROOT/integrations/ios/build-ui"
rm -rf "$UI"
(cd "$ROOT" && node packages/cli/bin/jsui.js build "$APP" -o "$UI/bundle.js" --minify)
[ -z "${STATE:-}" ] || printf '%s\n' "$STATE" > "$UI/state.json"

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT="$SDK" \
  -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_BUILD_TYPE=Release \
  -DJSUI_AUDIO=ON \
  -DJSUI_IOS_BUNDLE="$UI/bundle.js"
cmake --build "$BUILD"
APP_DIR="$BUILD/integrations/ios/JsuiDemo.app"
echo "Built $APP_DIR"

[ "${RUN:-0}" = "1" ] || exit 0
[ "$SDK" = "iphonesimulator" ] || { echo "RUN=1 needs SDK=iphonesimulator" >&2; exit 1; }
DEVICE="${DEVICE:-$(xcrun simctl list devices available | grep -m1 -oE 'iPhone [^(]+' | sed 's/ *$//')}"
echo "Simulator: $DEVICE"
xcrun simctl boot "$DEVICE" 2>/dev/null || true
xcrun simctl bootstatus "$DEVICE" -b >/dev/null
xcrun simctl install "$DEVICE" "$APP_DIR"
xcrun simctl terminate "$DEVICE" com.orktes.jsui.demo 2>/dev/null || true
xcrun simctl launch "$DEVICE" com.orktes.jsui.demo ${LAUNCH_ARGS:-}
sleep "${WAIT:-6}"
SHOT="${SHOT:-$BUILD/screenshot.png}"
xcrun simctl io "$DEVICE" screenshot "$SHOT"
echo "Screenshot: $SHOT"
