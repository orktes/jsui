#!/usr/bin/env bash
# Checks a staged shared-library package (scripts/dist/stage-native.sh
# <out>/shared) the way a host would use it: builds tests/shared/consumer.c
# against it with find_package(jsui) and runs it (on macOS once per
# architecture the machine can run), then checks that the library exports
# the C API and nothing else and, on Linux, does not need libstdc++.
#
#   scripts/dist/check-shared.sh <package dir> [<scratch build dir>]
set -euo pipefail

# Native paths for CMake (Git Bash on Windows: D:/a/..., not /d/a/...).
abspath() { (cd "$1" && { pwd -W 2>/dev/null || pwd; }); }
ROOT="$(abspath "$(dirname "${BASH_SOURCE[0]}")/../..")"
PKG="$(abspath "$1")"
B="${2:-$ROOT/build-consumer}"

cmake -S "$ROOT/tests/shared" -B "$B" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PKG" ${CONSUMER_CMAKE_ARGS:-}
cmake --build "$B" --config Release

case "$(uname -s)" in
  Linux)
    "$B/consumer"
    lib="$PKG/lib/libjsui.so"
    extra="$(nm -D --defined-only "$lib" | awk '{print $NF}' | grep -v '^jsui_' || true)"
    [ -z "$extra" ] || { echo "libjsui.so exports more than the C API:"; echo "$extra" | head -20; exit 1; }
    needed="$(objdump -p "$lib" | awk '/NEEDED/ {print $2}')"
    echo "NEEDED: $(echo $needed)"
    if echo "$needed" | grep -q 'libstdc++\|libgcc_s'; then echo "libjsui.so must not need libstdc++ / libgcc_s"; exit 1; fi
    echo "glibc: $(objdump -T "$lib" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
    ;;
  Darwin)
    lib="$PKG/lib/libjsui.dylib"
    for arch in $(lipo -archs "$B/consumer"); do
      if arch "-$arch" /usr/bin/true 2>/dev/null; then
        arch "-$arch" "$B/consumer"
      else
        echo "skipping the $arch run (this Mac cannot run it)"
      fi
    done
    extra="$(nm -gU "$lib" | awk '{print $NF}' | grep -v '^_jsui_' || true)"
    [ -z "$extra" ] || { echo "libjsui.dylib exports more than the C API:"; echo "$extra" | sort -u | head -20; exit 1; }
    otool -L "$lib"
    ;;
  MINGW*|MSYS*|CYGWIN*)
    exe="$(find "$B" -name consumer.exe | head -1)"
    "$exe"
    ;;
esac
echo "check-shared: ok ($(grep -c '^JSUI_API' "$PKG/include/jsui/jsui.h") functions in jsui.h)"
