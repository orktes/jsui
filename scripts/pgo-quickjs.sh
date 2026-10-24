#!/usr/bin/env bash
# Profile-guided optimization of QuickJS, both stages in one build directory.
#
#   scripts/pgo-quickjs.sh BUILD_DIR [cmake configure args...]
#   cmake --build BUILD_DIR ...        # then build the targets as usual
#
# 1. configures BUILD_DIR with the given args and -DJSUI_QUICKJS_PGO=generate
#    (only QuickJS is instrumented) and builds jsui-bench and jsui-render;
# 2. runs the training set: jsui-bench scenarios (markers 100 buffer + json,
#    inventory, surfaces-props, interactions-wheel, list) and
#    packages/react/bench/commit-bench.jsx through jsui-render;
# 3. merges the profile (clang: llvm-profdata into BUILD_DIR/pgo/quickjs.profdata;
#    GCC: the .gcda files stay in BUILD_DIR/pgo/raw);
# 4. reconfigures BUILD_DIR with -DJSUI_QUICKJS_PGO=use and the profile.
#
# Switching modes only recompiles QuickJS's four C files and relinks, so the
# real build afterwards reuses every other object of step 1.
#
# Any failure after step 1 (no node / npm, a scenario failing, no
# llvm-profdata) is a warning: BUILD_DIR is then reconfigured without PGO and
# the script exits 0, so a release never fails over PGO. PGO_STRICT=1 makes
# those failures errors (for checking the script itself).
#
# Environment:
#   PGO_JOBS=N          build parallelism (default: all cores)
#   PGO_STRICT=1        fail instead of falling back to a build without PGO
#   PGO_FRAMES=N        measured frames per training scenario (default 240)
#   PGO_PROFILE_OUT=F   also copy the merged .profdata (clang) to F
#   LLVM_PROFDATA=PATH  the llvm-profdata to use (default: next to the
#                       compiler, llvm-profdata-<major>, xcrun's on macOS)
set -euo pipefail

if [ $# -lt 1 ] || [ "$1" = -h ] || [ "$1" = --help ]; then
  sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'
  exit 2
fi
repo="$(cd "$(dirname "$0")/.." && pwd)"
build="$1"
shift
mkdir -p "$build"
build="$(cd "$build" && pwd)"
jobs="${PGO_JOBS:-$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 4)}"
frames="${PGO_FRAMES:-240}"
pgo="$build/pgo"

log() { echo "pgo-quickjs: $*"; }
warn() {
  if [ -n "${GITHUB_ACTIONS:-}" ]; then echo "::warning::pgo-quickjs: $*"; else echo "pgo-quickjs: warning: $*" >&2; fi
}

# Stage 1: instrumented build of the training programs.
rm -rf "$pgo/raw" "$pgo/train"
cmake -S "$repo" -B "$build" "$@" -DJSUI_QUICKJS_PGO=generate -DJSUI_QUICKJS_PROFILE="$pgo/raw"

no_pgo() {
  if [ "${PGO_STRICT:-0}" = 1 ]; then
    echo "pgo-quickjs: error: $*" >&2
    exit 1
  fi
  warn "$*; building QuickJS without PGO"
  cmake -S "$repo" -B "$build" -DJSUI_QUICKJS_PGO= -DJSUI_QUICKJS_PROFILE= >/dev/null
  exit 0
}

[ -f "$pgo/toolchain.env" ] || no_pgo "the compiler does not support PGO here (see the configure warning)"
# shellcheck disable=SC1091
. "$pgo/toolchain.env"
log "instrumented build ($PGO_CC_ID $PGO_CC_VERSION)"
cmake --build "$build" -j "$jobs" --target jsui-bench jsui-render || no_pgo "the instrumented build failed"
[ -x "$build/jsui-bench" ] && [ -x "$build/jsui-render" ] || no_pgo "jsui-bench / jsui-render were not built (JSUI_BUILD_TOOLS=OFF?)"

# Stage 2: training bundles (production React, like tools/jsui-bench/run.sh).
command -v node >/dev/null || no_pgo "node is not installed (the training bundles need the jsui CLI)"
if [ ! -d "$repo/node_modules/esbuild" ]; then
  log "npm ci (the jsui CLI's dependencies)"
  (cd "$repo" && npm ci --no-audit --no-fund >/dev/null) || no_pgo "npm ci failed"
fi
out="$pgo/train"
mkdir -p "$out"
jsui="$repo/packages/cli/bin/jsui.js"
node "$jsui" build "$repo/examples/showcase/interactions.jsx" -o "$out/interactions.js" >/dev/null || no_pgo "bundling failed"
for s in list inventory surfaces markers; do
  node "$jsui" build "$repo/tools/jsui-bench/scenarios/$s.jsx" -o "$out/$s.js" >/dev/null || no_pgo "bundling $s failed"
done
node "$jsui" build "$repo/packages/react/bench/commit-bench.jsx" -o "$out/commit-bench.js" >/dev/null || no_pgo "bundling commit-bench failed"

# The jsui-bench --suite scenarios of the same names (tools/jsui-bench/main.cpp).
bench() {
  local name="$1"
  shift
  log "training: $name"
  "$build/jsui-bench" --frames "$frames" --warmup 60 --name "$name" "$@" >"$out/$name.txt" 2>&1 ||
    { cat "$out/$name.txt" >&2; no_pgo "training scenario $name failed"; }
}
bench markers-buffer-100 "$out/markers.js" --markers 100,buffer --paint none
bench markers-json-100 "$out/markers.js" --markers 100,json --paint none
bench inventory "$out/inventory.js" --patch-state inventory,500
bench surfaces-props "$out/surfaces.js" --surfaces 30,changing
bench interactions-wheel "$out/interactions.js" --wheel 1080,396,12,60
bench list "$out/list.js"
log "training: commit-bench (jsui-render)"
"$build/jsui-render" "$out/commit-bench.js" -o "$out/commit-bench.png" --wait-ms 0 >"$out/commit-bench.txt" 2>&1 ||
  { cat "$out/commit-bench.txt" >&2; no_pgo "training with commit-bench failed"; }

# Stage 3: the profile.
case "$PGO_KIND" in
  clang)
    shopt -s nullglob
    raw=("$PGO_RAW_DIR"/*.profraw)
    shopt -u nullglob
    [ ${#raw[@]} -gt 0 ] || no_pgo "the training wrote no .profraw files to $PGO_RAW_DIR"
    profdata="${LLVM_PROFDATA:-}"
    if [ -z "$profdata" ]; then
      major="${PGO_CC_VERSION%%.*}"
      ccdir="$(dirname "$(readlink -f "$PGO_CC" 2>/dev/null || echo "$PGO_CC")")"
      if [ "$PGO_CC_ID" = AppleClang ]; then
        profdata="$(xcrun -f llvm-profdata 2>/dev/null || true)"
      fi
      for c in "$ccdir/llvm-profdata" "llvm-profdata-$major" llvm-profdata; do
        [ -n "$profdata" ] && break
        if [ -x "$c" ] || command -v "$c" >/dev/null 2>&1; then profdata="$c"; fi
      done
    fi
    [ -n "$profdata" ] || no_pgo "llvm-profdata not found (set LLVM_PROFDATA)"
    log "merging ${#raw[@]} raw profiles with $profdata"
    "$profdata" merge -o "$pgo/quickjs.profdata" "${raw[@]}" || no_pgo "llvm-profdata merge failed"
    profile="$pgo/quickjs.profdata"
    if [ -n "${PGO_PROFILE_OUT:-}" ]; then
      mkdir -p "$(dirname "$PGO_PROFILE_OUT")"
      cp "$profile" "$PGO_PROFILE_OUT"
    fi
    ;;
  gcc)
    n=$(find "$PGO_RAW_DIR" -name '*.gcda' | wc -l)
    [ "$n" -gt 0 ] || no_pgo "the training wrote no .gcda files to $PGO_RAW_DIR"
    log "$n .gcda files in $PGO_RAW_DIR"
    profile="$PGO_RAW_DIR"
    ;;
  *) no_pgo "unknown compiler kind '$PGO_KIND'" ;;
esac

# Stage 4: the optimized configuration (QuickJS recompiles on the next build).
cmake -S "$repo" -B "$build" -DJSUI_QUICKJS_PGO=use -DJSUI_QUICKJS_PROFILE="$profile" | tee "$pgo/configure-use.log"
grep -q 'QuickJS PGO: optimizing with' "$pgo/configure-use.log" || no_pgo "the profile was not accepted (see the warning above)"
log "BUILD_DIR configured with $profile; build the targets with cmake --build $build"
