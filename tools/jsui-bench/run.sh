#!/usr/bin/env bash
# Builds the jsui-bench scenarios (production React) and runs the suite.
#
#   tools/jsui-bench/run.sh [build-dir] [jsui-bench args...]
#   tools/jsui-bench/run.sh build --json after.json --compare before.json
#   JSUI_BUILD_FLAGS=--react-compiler tools/jsui-bench/run.sh   (extra `jsui build` flags)
#
# Needs `npm ci` at the repo root and a native build with the jsui-bench
# target (cmake --build build --target jsui-bench). Works on Linux and macOS.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$repo/build}"
[ $# -gt 0 ] && shift
out="$build/bench-bundles"
mkdir -p "$out"
jsui="$repo/packages/cli/bin/jsui.js"
read -r -a flags <<< "${JSUI_BUILD_FLAGS:-}"
node "$jsui" build "$repo/examples/showcase/interactions.jsx" -o "$out/interactions.js" ${flags[@]+"${flags[@]}"} >/dev/null
for s in list menu inventory surfaces markers; do
  node "$jsui" build "$repo/tools/jsui-bench/scenarios/$s.jsx" -o "$out/$s.js" ${flags[@]+"${flags[@]}"} >/dev/null
done
exec "$build/jsui-bench" --suite "$out" "$@"
