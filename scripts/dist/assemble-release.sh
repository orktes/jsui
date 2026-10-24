#!/usr/bin/env bash
# Turns the per-platform CI artifacts (jsui-native-<platform>, each staged by
# scripts/dist/stage-native.sh) into the files attached to a GitHub Release:
#
#   jsui-<version>-<platform>.tar.gz (.zip on Windows)
#       include/ lib/ bin/ (the shared library, its CMake package, jsui-viewer)
#   jsui-<version>-ios-xcframework.zip
#       JSUI.xcframework (static library + jsui.h, device and simulator arm64)
#   jsui-godot-<version>.zip
#       addons/jsui/ with jsui.gdextension and every platform's library
#   SHA256SUMS.txt
#
#   scripts/dist/assemble-release.sh <version> <artifacts dir> <out dir>
#
# <artifacts dir> holds one directory per platform (jsui-native-linux-x64,
# ..., jsui-ios-xcframework). Platforms whose artifact is missing are left out of the archives.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="$1"
ART="$(cd "$2" && pwd)"
mkdir -p "$3"
OUT="$(cd "$3" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

notices() {
  cp "$ROOT/LICENSE" "$1/LICENSE"
  cp "$ROOT/docs/THIRD_PARTY_NOTICES.md" "$1/THIRD_PARTY_NOTICES.md"
  cp "$ROOT/assets/fonts/Inter-LICENSE.txt" "$1/Inter-LICENSE.txt"
}

zipdir() {  # zipdir <archive> <dir> <entry>: <entry> (inside <dir>) as the archive's top level
  (cd "$2" && rm -f "$1" && if command -v zip >/dev/null; then zip -qr -X "$1" "$3"; else 7z a -tzip -bd "$1" "$3" >/dev/null; fi)
}

godot="$WORK/godot/addons/jsui"
mkdir -p "$godot/bin"
platforms=()
for dir in "$ART"/jsui-native-*; do
  [ -d "$dir" ] || continue
  platform="${dir##*/jsui-native-}"
  platforms+=("$platform")
  name="jsui-$VERSION-$platform"
  pkg="$WORK/$name"
  mkdir -p "$pkg/bin"
  cp -R "$dir/shared/." "$pkg/"
  cp -R "$dir/viewer/." "$pkg/bin/"
  sed "s/@VERSION@/$VERSION/g; s/@PLATFORM@/$platform/g" "$ROOT/scripts/dist/README.package.md" > "$pkg/README.md"
  notices "$pkg"
  # Artifacts lose the executable bit.
  chmod +x "$pkg"/bin/jsui-viewer* 2>/dev/null || true
  case "$platform" in
    windows-*) zipdir "$OUT/$name.zip" "$WORK" "$name" ;;
    *) tar -C "$WORK" -czf "$OUT/$name.tar.gz" "$name" ;;
  esac
  cp -R "$dir/godot/." "$godot/bin/"
done
[ ${#platforms[@]} -gt 0 ] || { echo "assemble-release: no jsui-native-* artifacts in $ART" >&2; exit 1; }

# iOS: JSUI.xcframework (static, device + simulator arm64), if it was built.
if [ -d "$ART/jsui-ios-xcframework/JSUI.xcframework" ]; then
  ios="$WORK/jsui-$VERSION-ios"
  mkdir -p "$ios"
  cp -R "$ART/jsui-ios-xcframework/JSUI.xcframework" "$ios/"
  sed "s/@VERSION@/$VERSION/g" "$ROOT/scripts/dist/README.ios.md" > "$ios/README.md"
  notices "$ios"
  zipdir "$OUT/jsui-$VERSION-ios-xcframework.zip" "$WORK" "jsui-$VERSION-ios"
  platforms+=(ios)
fi

# The Godot addon: drop addons/jsui into a project.
sed 's|res://bin/|res://addons/jsui/bin/|' "$ROOT/integrations/godot/demo/jsui.gdextension" \
  | grep -v '^; Built by CMake\|^; library serves' > "$godot/jsui.gdextension"
grep -q 'res://addons/jsui/bin/libjsui_godot' "$godot/jsui.gdextension"
sed "s/@VERSION@/$VERSION/g" "$ROOT/scripts/dist/README.godot.md" > "$godot/README.md"
notices "$godot"
zipdir "$OUT/jsui-godot-$VERSION.zip" "$WORK/godot" addons

sum() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
(cd "$OUT" && shopt -s nullglob && files=(*.tar.gz *.tgz *.zip) && sum "${files[@]}" > SHA256SUMS.txt)
echo "Platforms: ${platforms[*]}"
ls -l "$OUT"
