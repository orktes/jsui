#!/usr/bin/env bash
# Sets up a Windows x64 cross toolchain on Linux (pinned): LLVM's clang-cl,
# lld-link, llvm-lib / llvm-rc / llvm-mt / llvm-nm from the official LLVM
# release, and the MSVC CRT + Windows SDK headers and libraries from
# Microsoft's packages, laid out by xwin in the /winsysroot layout
# (VC/Tools/MSVC/<ver>, Windows Kits/10). Used by
# scripts/build-skia-desktop.sh (SKIA_TARGET_OS=win) and by
# cmake/toolchains/windows-x64-clang-cl.cmake.
#
#   scripts/setup-win-cross.sh [<dir>]   (default third_party/win-cross)
#
# Installs <dir>/llvm/bin/... and <dir>/winsysroot/..., and prints the
# environment the builds take (CLANG_CL, LLD_LINK, NM, XWIN_SYSROOT,
# WIN_CROSS_LLVM). Idempotent: a <dir>/.stamp matching this script is kept,
# so CI caches <dir> keyed on the script's hash.
#
# Accepts Microsoft's license for the CRT / SDK packages on the caller's
# behalf (xwin --accept-license): see https://visualstudio.microsoft.com/license-terms/
set -euo pipefail

LLVM_VERSION="23.1.4"
XWIN_VERSION="0.10.0"
MSVC_CRT_VERSION="14.44.17.14"
WIN_SDK_VERSION="10.0.26100"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="$(mkdir -p "${1:-$ROOT/third_party/win-cross}" && cd "${1:-$ROOT/third_party/win-cross}" && pwd)"
[ "$(uname -s)-$(uname -m)" = Linux-x86_64 ] || { echo "setup-win-cross.sh runs on x86_64 Linux" >&2; exit 1; }

STAMP_VALUE="$(sha256sum "$ROOT/scripts/setup-win-cross.sh" | cut -d' ' -f1)"
print_env() {
  echo "WIN_CROSS_LLVM=$DIR/llvm"
  echo "CLANG_CL=$DIR/llvm/bin/clang-cl"
  echo "LLD_LINK=$DIR/llvm/bin/lld-link"
  echo "NM=$DIR/llvm/bin/llvm-nm"
  echo "XWIN_SYSROOT=$DIR/winsysroot"
}
if [ -f "$DIR/.stamp" ] && [ "$(cat "$DIR/.stamp")" = "$STAMP_VALUE" ]; then
  echo "win-cross is up to date ($DIR)" >&2
  print_env
  exit 0
fi
rm -rf "$DIR/llvm" "$DIR/winsysroot" "$DIR/.stamp"

# ------------------------------------------------------------------- LLVM
# The release archive is ~2 GB: stream it and keep only the tools (clang-cl
# and lld-link are symlinks to clang / lld) and clang's own headers.
echo "LLVM $LLVM_VERSION" >&2
top="LLVM-$LLVM_VERSION-Linux-X64"
mkdir -p "$DIR/llvm"
curl -fsSL --retry 4 "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/$top.tar.xz" \
  | tar -xJ -C "$DIR/llvm" --strip-components=1 --wildcards \
      "$top/bin/clang" "$top/bin/clang-[0-9]*" "$top/bin/clang-cl" "$top/bin/lld" "$top/bin/lld-link" \
      "$top/bin/llvm-ar" "$top/bin/llvm-lib" "$top/bin/llvm-ranlib" "$top/bin/llvm-nm" \
      "$top/bin/llvm-rc" "$top/bin/llvm-mt" "$top/bin/llvm-objdump" \
      "$top/lib/clang/*/include/*"
"$DIR/llvm/bin/clang-cl" --version | head -1 >&2
"$DIR/llvm/bin/lld-link" --version >&2

# ------------------------------------------------------------ CRT + SDK
echo "xwin $XWIN_VERSION: MSVC CRT $MSVC_CRT_VERSION, Windows SDK $WIN_SDK_VERSION" >&2
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -fsSL --retry 4 "https://github.com/Jake-Shadle/xwin/releases/download/$XWIN_VERSION/xwin-$XWIN_VERSION-x86_64-unknown-linux-musl.tar.gz" \
  | tar -xz -C "$tmp" --strip-components=1
# Case symlinks stay on (the SDK's own headers include <Windows.h> as
# <windows.h> and so on); MS architecture names keep lib/x64 as gn and
# clang-cl's /winsysroot expect.
"$tmp/xwin" --accept-license --cache-dir "$tmp/cache" --arch x86_64 \
  --crt-version "$MSVC_CRT_VERSION" --sdk-version "$WIN_SDK_VERSION" \
  splat --use-winsysroot-style --preserve-ms-arch-notation --output "$DIR/winsysroot"
[ -d "$DIR/winsysroot/VC/Tools/MSVC/$MSVC_CRT_VERSION/lib/x64" ] \
  && [ -d "$DIR/winsysroot/Windows Kits/10/Lib/$WIN_SDK_VERSION/um/x64" ] \
  || { echo "unexpected xwin layout in $DIR/winsysroot" >&2; exit 1; }

echo "$STAMP_VALUE" > "$DIR/.stamp"
du -sh "$DIR/llvm" "$DIR/winsysroot" >&2
print_env
