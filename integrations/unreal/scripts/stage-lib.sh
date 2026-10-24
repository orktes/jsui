#!/bin/sh
# Copies a local CMake build of the JSUI shared library (jsui_shared,
# -DJSUI_BUILD_SHARED=ON) into the plugin's ThirdParty/JsuiLibrary, in the
# layout of the release artifacts the plugin zip is assembled from:
#
#   include/jsui/jsui.h
#   lib/libjsui.dylib | lib/libjsui.so | lib/jsui.lib
#   bin/jsui.dll + bin/icudtl.dat (Windows)
#
# usage: integrations/unreal/scripts/stage-lib.sh BUILD_DIR
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
build=${1:?usage: stage-lib.sh BUILD_DIR (a CMake build with -DJSUI_BUILD_SHARED=ON)}
build=$(cd "$build" && pwd)
dest="$here/../Plugins/Jsui/Source/ThirdParty/JsuiLibrary"

mkdir -p "$dest/include/jsui" "$dest/lib"
cp "$root/include/jsui/jsui.h" "$dest/include/jsui/"
found=0
for f in libjsui.dylib libjsui.so; do
  if [ -f "$build/$f" ]; then
    cp "$build/$f" "$dest/lib/"
    echo "staged lib/$f"
    found=1
  fi
done
for d in "$build" "$build/Release" "$build/RelWithDebInfo"; do
  if [ -f "$d/jsui.dll" ]; then
    mkdir -p "$dest/bin"
    cp "$d/jsui.dll" "$dest/bin/"
    cp "$d/jsui.lib" "$dest/lib/"
    [ -f "$d/icudtl.dat" ] && cp "$d/icudtl.dat" "$dest/bin/"
    echo "staged bin/jsui.dll, lib/jsui.lib"
    found=1
    break
  fi
done
if [ "$found" = 0 ]; then
  echo "no libjsui.dylib / libjsui.so / jsui.dll in $build (build the jsui_shared target)" >&2
  exit 1
fi
