#!/bin/sh
# Builds the example game, headless: the UI bundle, the JSUI shared library
# (staged into the plugin), then RunUAT BuildCookRun -> build/unreal/<platform>.
#
# usage: integrations/unreal/scripts/build-example.sh [UE_ROOT] [JSUI_SKIA_DIR]
#   UE_ROOT       default: /Users/Shared/Epic Games/UE_5.8 (env UE_ROOT)
#   JSUI_SKIA_DIR a Skia with Graphite (scripts/build-skia-desktop.sh), for the
#                 GPU path; default third_party/skia-desktop (env JSUI_SKIA_DIR)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
ue=${1:-${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}}
skia=${2:-${JSUI_SKIA_DIR:-$root/third_party/skia-desktop}}
build="$root/build-unreal"
project="$root/integrations/unreal/project"
out="$root/build/unreal"

# 1. The UI bundle, copied into the project (Content/Jsui is staged as a
#    non-UFS directory, DefaultGame.ini).
bundle="$root/integrations/shared/ui/dist/bundle.js"
if [ ! -f "$bundle" ]; then
  (cd "$root" && npx jsui build integrations/shared/ui/app.jsx -o integrations/shared/ui/dist/bundle.js)
fi
mkdir -p "$project/Content/Jsui"
cp "$bundle" "$project/Content/Jsui/bundle.js"

# 2. libjsui (the C API as a shared library) -> Plugins/Jsui/Source/ThirdParty.
#    macOS 14: the oldest macOS UE 5.8 games run on.
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DJSUI_BUILD_SHARED=ON \
  -DJSUI_BUILD_TOOLS=OFF -DJSUI_BUILD_TESTS=OFF -DJSUI_BUILD_VIEWER=OFF -DJSUI_SKIA_DIR="$skia" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
cmake --build "$build" --target jsui_shared
"$here/stage-lib.sh" "$build"

# 3. Build, cook, stage, package.
case "$(uname -s)" in
  Darwin) platform=Mac ;;
  Linux) platform=Linux ;;
  *) platform=Win64 ;;
esac
"$ue/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun \
  -project="$project/JsuiExample.uproject" -platform="$platform" -clientconfig=Development \
  -build -cook -stage -pak -iostore -package -archive -archivedirectory="$out" \
  -unattended -utf8output -nop4 -nodebuginfo
echo "built: $out"
