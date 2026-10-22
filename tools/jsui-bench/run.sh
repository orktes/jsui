#!/usr/bin/env bash
# Builds the jsui-bench scenarios (production React) and runs the suite.
#
#   tools/jsui-bench/run.sh [build-dir] [jsui-bench args...]
#   tools/jsui-bench/run.sh build --json after.json --compare before.json
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
node "$jsui" build "$repo/examples/showcase/interactions.jsx" -o "$out/interactions.js" >/dev/null
for s in list menu inventory; do
  node "$jsui" build "$repo/tools/jsui-bench/scenarios/$s.jsx" -o "$out/$s.js" >/dev/null
done
exec "$build/jsui-bench" --suite "$out" "$@"
