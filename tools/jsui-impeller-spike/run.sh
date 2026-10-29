#!/usr/bin/env bash
# Impeller spike: first-launch and later-launch runs of the same screens on
# Graphite and on Impeller (macOS, Metal). See README.md.
#
#   tools/jsui-impeller-spike/run.sh [build-dir] [--runs N] [--frames N] [--out DIR]
#
# Needs `npm ci` at the repo root and a build configured with
# -DJSUI_IMPELLER_SDK=<unzipped impeller_sdk.zip> (cmake --build <build-dir>
# --target jsui-impeller-spike). Takes the shared Mac bench lock
# (~/Development/.bench-lock) for the whole run.
#
# Setups, each a fresh process per run:
#   graphite-bare    no pipeline cache file, no common precompile
#   graphite-first   as shipped, first launch (empty cache dir)
#   graphite-second  as shipped, second launch (the cache file graphite-first wrote)
#   impeller-first   first launch
#   impeller-second  second launch
# "first" runs start with the OS Metal shader cache moved away (and the one
# the run made deleted afterwards): a user's first launch. The original cache
# is restored at the end, also on errors.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
build="$repo/build"
if [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; then build="$1"; shift; fi
runs=3
frames=240
out="$build/impeller-spike"
while [ $# -gt 0 ]; do
  case "$1" in
    --runs) runs="$2"; shift 2 ;;
    --frames) frames="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    *) echo "unknown flag $1" >&2; exit 2 ;;
  esac
done
bin="$build/jsui-impeller-spike"
[ -x "$bin" ] || { echo "build $bin first" >&2; exit 1; }
mkdir -p "$out/bundles"

screens=(main-menu inventory hud settings battle-pass)
bundles=()
for s in "${screens[@]}"; do
  node "$repo/packages/cli/bin/jsui.js" build "$repo/examples/screens/$s.jsx" -o "$out/bundles/$s.js" >/dev/null
  bundles+=("$out/bundles/$s.js")
done

lock="$HOME/Development/.bench-lock"
until mkdir "$lock" 2>/dev/null; do echo "waiting for $lock"; sleep 30; done
metal="$(getconf DARWIN_USER_CACHE_DIR)/com.apple.metal"
saved="$metal.impeller-spike-saved"
cleanup() {
  rm -rf "$metal"
  [ -e "$saved" ] && mv "$saved" "$metal"
  rmdir "$lock" 2>/dev/null || true
}
trap cleanup EXIT
[ -e "$metal" ] && mv "$metal" "$saved"
uptime

run() {  # name run-index flags...
  local name="$1" i="$2"
  shift 2
  echo "== $name #$i"
  "$bin" --frames "$frames" --json "$out/$name-$i.json" "$@" "${bundles[@]}"
}

for i in $(seq 1 "$runs"); do
  cache="$out/cache-$i"
  rm -rf "$cache" "$metal"
  run graphite-bare "$i" --cache-dir "$cache" --no-cache-file --no-precompile
  rm -rf "$cache" "$metal"
  run graphite-first "$i" --cache-dir "$cache" --png "$out/graphite-$i.png"
  run graphite-second "$i" --cache-dir "$cache"
  rm -rf "$cache" "$metal"
  run impeller-first "$i" --backend impeller --png "$out/impeller-$i.png"
  run impeller-second "$i" --backend impeller
done
# Warm, GPU heavier: 2560x1440 at 2x, a mouse sweep for hover states.
for b in graphite impeller; do
  "$bin" --backend "$b" --frames 600 --width 2560 --height 1440 --scale 2 --hover \
    --json "$out/$b-1440p.json" "${bundles[@]}"
done
uptime
node "$repo/tools/jsui-impeller-spike/report.mjs" "$out"
