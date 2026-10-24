#!/usr/bin/env bash
# The engine plugins attached to a GitHub Release, from the same per-platform
# CI artifacts as scripts/dist/assemble-release.sh (jsui-native-<platform>):
#
#   com.orktes.jsui-<version>.tgz
#       The Unity package (integrations/unity/package) with the native plugin
#       for every platform built (Plugins/macOS/jsui_unity.bundle,
#       Plugins/Linux/x86_64/libjsui_unity.so) and their .meta import
#       settings. Package Manager > + > Install package from tarball.
#       With <upm dir>, the same tree is also left there (the `upm` git branch).
#   jsui-unreal-<version>.zip
#       Jsui/, the Unreal plugin (integrations/unreal/Plugins/Jsui) with the
#       prebuilt JSUI shared library for macOS (universal), Win64 and Linux
#       (x86_64) in Source/ThirdParty/JsuiLibrary. Unzip into a project's
#       Plugins/; Unreal compiles the plugin's module.
#
#   scripts/dist/assemble-engines.sh <version> <artifacts dir> <out dir> [<upm dir>]
#
# Platforms whose artifact is missing are left out. Run it before
# assemble-release.sh, whose SHA256SUMS.txt covers these files too.
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
}

# Unity needs a .meta next to every file and folder of an immutable (tarball
# or git) package, and their GUIDs must not change between versions: derive
# them from the path inside the package.
md5() { if command -v md5sum >/dev/null; then md5sum | cut -c1-32; else command md5 -q; fi; }
guid() { printf 'com.orktes.jsui/%s' "$1" | md5; }

folder_meta() {  # folder_meta <package dir> <relative folder>
  cat > "$1/$2.meta" <<EOF
fileFormatVersion: 2
guid: $(guid "$2")
folderAsset: yes
DefaultImporter:
  externalObjects: {}
  userData:
  assetBundleName:
  assetBundleVariant:
EOF
}

text_meta() {  # text_meta <package dir> <relative file>
  cat > "$1/$2.meta" <<EOF
fileFormatVersion: 2
guid: $(guid "$2")
TextScriptImporter:
  externalObjects: {}
  userData:
  assetBundleName:
  assetBundleVariant:
EOF
}

# plugin_meta <package dir> <relative file> <editor OS> <CPU> <standalone target>:
# the native plugin loads in the Editor and in standalone players of one OS.
plugin_meta() {
  cat > "$1/$2.meta" <<EOF
fileFormatVersion: 2
guid: $(guid "$2")
PluginImporter:
  externalObjects: {}
  serializedVersion: 2
  iconMap: {}
  executionOrder: {}
  defineConstraints: []
  isPreloaded: 0
  isOverridable: 1
  isExplicitlyReferenced: 0
  validateReferences: 1
  platformData:
  - first:
      Any:
    second:
      enabled: 0
      settings: {}
  - first:
      Editor: Editor
    second:
      enabled: 1
      settings:
        CPU: $4
        DefaultValueInitialized: true
        OS: $3
  - first:
      Standalone: $5
    second:
      enabled: 1
      settings:
        CPU: $4
  userData:
  assetBundleName:
  assetBundleVariant:
EOF
}

# ------------------------------------------------------------------- Unity
upm="$WORK/package"
mkdir -p "$upm"
(cd "$ROOT/integrations/unity/package" && tar --exclude=.gitignore --exclude='Plugins/*' -cf - .) | tar -C "$upm" -xf -
node -e '
  const fs = require("fs"), f = process.argv[1], p = JSON.parse(fs.readFileSync(f, "utf8"));
  p.version = process.argv[2];
  p.documentationUrl = "https://github.com/orktes/jsui/tree/main/integrations/unity";
  p.licensesUrl = "https://github.com/orktes/jsui/blob/main/LICENSE";
  fs.writeFileSync(f, JSON.stringify(p, null, 2) + "\n");
' "$upm/package.json" "$VERSION"
unity=()
mkdir -p "$upm/Plugins"
if [ -d "$ART/jsui-native-macos-universal/unity/jsui_unity.bundle" ]; then
  mkdir -p "$upm/Plugins/macOS"
  cp -R "$ART/jsui-native-macos-universal/unity/jsui_unity.bundle" "$upm/Plugins/macOS/"
  # Artifacts lose the executable bit.
  chmod +x "$upm/Plugins/macOS/jsui_unity.bundle/Contents/MacOS/jsui_unity"
  folder_meta "$upm" Plugins/macOS
  # Unity imports a .bundle as one asset: the .meta sits next to the folder.
  plugin_meta "$upm" Plugins/macOS/jsui_unity.bundle OSX AnyCPU OSXUniversal
  unity+=(macos)
fi
if [ -f "$ART/jsui-native-linux-x64/unity/libjsui_unity.so" ]; then
  mkdir -p "$upm/Plugins/Linux/x86_64"
  cp "$ART/jsui-native-linux-x64/unity/libjsui_unity.so" "$upm/Plugins/Linux/x86_64/"
  folder_meta "$upm" Plugins/Linux
  folder_meta "$upm" Plugins/Linux/x86_64
  plugin_meta "$upm" Plugins/Linux/x86_64/libjsui_unity.so Linux x86_64 Linux64
  unity+=(linux-x64)
fi
[ ${#unity[@]} -gt 0 ] || { echo "assemble-engines: no Unity plugin in $ART/jsui-native-*/unity" >&2; exit 1; }
cp "$ROOT/LICENSE" "$upm/LICENSE.md"
text_meta "$upm" LICENSE.md
# The UPM tarball layout (npm pack): everything under package/.
tar -C "$WORK" --owner=0 --group=0 --numeric-owner -czf "$OUT/com.orktes.jsui-$VERSION.tgz" package
if [ -n "${4:-}" ]; then
  rm -rf "$4"
  mkdir -p "$(dirname "$4")"
  cp -R "$upm" "$4"
fi

# ------------------------------------------------------------------ Unreal
ue="$WORK/unreal/Jsui"
mkdir -p "$ue"
(cd "$ROOT/integrations/unreal/Plugins/Jsui" && tar --exclude=Binaries --exclude=Intermediate \
  --exclude='Source/ThirdParty/JsuiLibrary/*' -cf - .) | tar -C "$ue" -xf -
lib="$ue/Source/ThirdParty/JsuiLibrary"
mkdir -p "$lib/include/jsui" "$lib/lib"
cp "$ROOT/include/jsui/jsui.h" "$lib/include/jsui/"
unreal=()
if [ -f "$ART/jsui-native-macos-universal/shared/lib/libjsui.dylib" ]; then
  cp "$ART/jsui-native-macos-universal/shared/lib/libjsui.dylib" "$lib/lib/"
  unreal+=(Mac)
fi
if [ -f "$ART/jsui-native-linux-x64/shared/lib/libjsui.so" ]; then
  cp "$ART/jsui-native-linux-x64/shared/lib/libjsui.so" "$lib/lib/"
  unreal+=(Linux)
fi
if [ -f "$ART/jsui-native-windows-x64/shared/bin/jsui.dll" ]; then
  mkdir -p "$lib/bin"
  cp "$ART/jsui-native-windows-x64/shared/bin/jsui.dll" "$ART/jsui-native-windows-x64/shared/bin/icudtl.dat" "$lib/bin/"
  cp "$ART/jsui-native-windows-x64/shared/lib/jsui.lib" "$lib/lib/"
  unreal+=(Win64)
fi
[ ${#unreal[@]} -gt 0 ] || { echo "assemble-engines: no shared library in $ART/jsui-native-*/shared" >&2; exit 1; }
# Only the platforms that have their library.
node -e '
  const fs = require("fs"), f = process.argv[1], p = JSON.parse(fs.readFileSync(f, "utf8"));
  p.VersionName = process.argv[2];
  for (const m of p.Modules) m.PlatformAllowList = process.argv.slice(3);
  fs.writeFileSync(f, JSON.stringify(p, null, "\t") + "\n");
' "$ue/Jsui.uplugin" "$VERSION" "${unreal[@]}"
notices "$ue"
sed "s/@VERSION@/$VERSION/g" "$ROOT/scripts/dist/README.unreal.md" > "$ue/README.md"
(cd "$WORK/unreal" && rm -f "$OUT/jsui-unreal-$VERSION.zip" && zip -qr -X "$OUT/jsui-unreal-$VERSION.zip" Jsui)

echo "Unity: ${unity[*]}; Unreal: ${unreal[*]}"
ls -l "$OUT/com.orktes.jsui-$VERSION.tgz" "$OUT/jsui-unreal-$VERSION.zip"
