#!/usr/bin/env bash
# Records the pipeline list the Godot demo ships, demo/jsui-pipelines.vulkan.bin.
# JsuiView precompiles res://<its pipeline_cache_path under user://> at
# startup, besides its own cache file, so even a player's first session does
# not compile the UI's GPU pipelines inside its first frames.
#
#   xvfb-run -a -s "-screen 0 1920x1080x24" integrations/godot/record-pipelines.sh
#
# Runs smoke-test.sh on Vulkan (GODOT_RENDERING=vulkan; Mesa's lavapipe is
# enough) RUNS times (default 3) with the input self-test and the tour
# (TOUR=1), all into one new cache file outside user:// (which leaves the
# shipped list out: the file holds what the runs compiled), then copies it
# into the demo. Several runs because which pipelines a frame needs varies a
# little with timing (draws batched into one render pass share its
# attachments). The keys are fitted to each device that loads them (its
# depth/stencil formats), so a list recorded on lavapipe serves other Vulkan
# GPUs too; a GPU whose Skia draws some things another way leaves those keys
# unused. Record again after updating Skia: a list of another Skia build is
# ignored, with a warning.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
runs="${RUNS:-3}"
work="$repo/build/record-pipelines"
rm -rf "$work"
mkdir -p "$work"
for ((i = 1; i <= runs; i++)); do
  echo "== recording, run $i of $runs"
  GODOT_RENDERING=vulkan TOUR=1 PIPELINE_CACHE="$work/pipelines.bin" "$here/smoke-test.sh" "$work/run$i.png"
done
list="$work/pipelines.vulkan.bin"
if [[ ! -s "$list" ]]; then
  echo "no pipelines recorded ($list)" >&2
  exit 1
fi
cp "$list" "$here/demo/jsui-pipelines.vulkan.bin"
echo "ok: $here/demo/jsui-pipelines.vulkan.bin ($(wc -c < "$list" | tr -d ' ') bytes)"
