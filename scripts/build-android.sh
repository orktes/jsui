#!/usr/bin/env bash
# Builds the JSUI Android example app (integrations/android) into an APK and
# optionally runs it on a device / emulator:
#
#   scripts/build-android.sh               # build-android/JsuiDemo.apk (arm64-v8a)
#   RUN=1 scripts/build-android.sh         # + install, launch, screenshot (adb)
#
# Environment:
#   ANDROID_HOME   SDK (default ~/Library/Android/sdk, else ~/Android/Sdk); needs
#                  build-tools (aapt2, zipalign, apksigner) and platforms;android-$TARGET_API
#   ANDROID_NDK_HOME  NDK (default: the newest $ANDROID_HOME/ndk/<version>)
#   ABI            arm64-v8a (default) or x86_64
#   APP            UI entry point (default integrations/godot/demo/ui/app.jsx)
#   MIN_API        minSdkVersion (default 29), TARGET_API targetSdkVersion (default 35)
#   ANDROID_SERIAL device for RUN=1 (adb's own variable; default: the only one)
#   LAUNCH_ARGS    extra `am start` arguments, e.g. "--es jsui_renderer cpu"
#   WAIT           seconds before the screenshot (default 6)
#   SHOT           screenshot path for RUN=1 (default build-android/screenshot.png)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ABI="${ABI:-arm64-v8a}"
APP="${APP:-$ROOT/integrations/godot/demo/ui/app.jsx}"
BUILD="${BUILD:-$ROOT/build-android}"
MIN_API="${MIN_API:-29}"
TARGET_API="${TARGET_API:-35}"
case "$ABI" in
  arm64-v8a) CPU=arm64 ;;
  x86_64) CPU=x64 ;;
  *) echo "unsupported ABI $ABI (arm64-v8a or x86_64)" >&2; exit 1 ;;
esac

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
if [ -z "$SDK" ]; then
  for d in "$HOME/Library/Android/sdk" "$HOME/Android/Sdk"; do [ -d "$d" ] && SDK="$d" && break; done
fi
[ -d "$SDK" ] || { echo "Android SDK not found: set ANDROID_HOME" >&2; exit 1; }
export ANDROID_HOME="$SDK"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1 || true)}}"
[ -f "$NDK/source.properties" ] || { echo "Android NDK not found: set ANDROID_NDK_HOME" >&2; exit 1; }
export ANDROID_NDK_HOME="$NDK"
BT="$(ls -d "$SDK"/build-tools/* 2>/dev/null | sort -V | tail -1)"
JAR="$SDK/platforms/android-$TARGET_API/android.jar"
[ -x "$BT/aapt2" ] || { echo "no build-tools in $SDK (sdkmanager 'build-tools;35.0.0')" >&2; exit 1; }
[ -f "$JAR" ] || { echo "no $JAR (sdkmanager 'platforms;android-$TARGET_API')" >&2; exit 1; }

CPUS="$CPU" ANDROID_API="$MIN_API" "$ROOT/scripts/build-skia-android.sh"

# UI bundle
[ -d "$ROOT/node_modules" ] || (cd "$ROOT" && npm ci)
UI="$ROOT/integrations/android/build-ui"
rm -rf "$UI"
(cd "$ROOT" && node packages/cli/bin/jsui.js build "$APP" -o "$UI/bundle.js" --minify)

# Native library
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM="android-$MIN_API" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --target jsuidemo

# APK: manifest + assets/ui (aapt2), the library stored uncompressed and
# 16 KB aligned (loaded straight from the APK), debug-signed.
STAGE="$BUILD/apk"
rm -rf "$STAGE"
mkdir -p "$STAGE/assets" "$STAGE/lib/$ABI"
cp -R "$UI" "$STAGE/assets/ui"
HOST_TAG="$(ls "$NDK/toolchains/llvm/prebuilt" | head -1)"
"$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin/llvm-strip" --strip-unneeded \
  -o "$STAGE/lib/$ABI/libjsuidemo.so" "$BUILD/integrations/android/libjsuidemo.so"
"$BT/aapt2" link -o "$STAGE/unaligned.apk" -I "$JAR" \
  --manifest "$ROOT/integrations/android/AndroidManifest.xml" \
  --min-sdk-version "$MIN_API" --target-sdk-version "$TARGET_API" \
  -A "$STAGE/assets"
(cd "$STAGE" && zip -q -0 -r unaligned.apk "lib/$ABI/libjsuidemo.so")
"$BT/zipalign" -f -P 16 4 "$STAGE/unaligned.apk" "$STAGE/aligned.apk"
KS="${DEBUG_KEYSTORE:-$HOME/.android/debug.keystore}"
if [ ! -f "$KS" ]; then
  mkdir -p "$(dirname "$KS")"
  keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias androiddebugkey \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null
fi
APK="$BUILD/JsuiDemo.apk"
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out "$APK" "$STAGE/aligned.apk"
echo "Built $APK ($(du -h "$APK" | cut -f1))"

[ "${RUN:-0}" = "1" ] || exit 0
ADB="$SDK/platform-tools/adb"
"$ADB" wait-for-device
"$ADB" install -r "$APK"
"$ADB" shell am force-stop com.orktes.jsui.demo
"$ADB" logcat -c || true
# shellcheck disable=SC2086
"$ADB" shell am start -n com.orktes.jsui.demo/android.app.NativeActivity ${LAUNCH_ARGS:-}
sleep "${WAIT:-6}"
SHOT="${SHOT:-$BUILD/screenshot.png}"
"$ADB" exec-out screencap -p > "$SHOT"
echo "Screenshot: $SHOT"
"$ADB" logcat -d -s jsui:* | tail -20
