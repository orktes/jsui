#!/bin/sh
# cd-bench.sh gpu|sw [SECONDS]: start claude-desktop-gpu's profile in GPU mode (ANGLE-GL on
# ~/gpu-stack) or software mode (--disable-gpu, like ~/.local/bin/claude-desktop), let it settle,
# then measure CPU of all its processes and jsui-wl. Run under with-lock.
M=${1:-gpu}; T=${2:-20}; O=$HOME/gpu-stack/logs/cd-bench-$M; rm -rf $O; mkdir -p $O
export XDG_RUNTIME_DIR=/run/user/32011 WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus
export LD_PRELOAD=$HOME/jsui-wm/lib/jsui-no-bluetooth.so
F=$HOME/gpu-stack/profiles/claude/claude_desktop_config.json
[ -f $F ] && sed -i 's/"isHardwareAcceleration\([A-Za-z]*\)": true/"isHardwareAcceleration\1": false/g' $F
if [ $M = gpu ]; then
  export CLAUDE_GPU_GL=egl CLAUDE_DESKTOP_FLAGS="$CD_EXTRA_FLAGS"
  setsid $HOME/gpu-stack/bin/claude-desktop-gpu > $O/stderr.log 2>&1 < /dev/null &
else
  D=$HOME/claude-desktop/usr/lib/claude-desktop
  setsid "$D/claude-desktop" --ozone-platform=wayland --disable-gpu --password-store=basic --enable-wayland-ime \
    --wayland-text-input-version=3 --user-data-dir=$HOME/gpu-stack/profiles/claude > $O/stderr.log 2>&1 < /dev/null &
fi
sleep 25
ps_() { ps -eo pid,args | grep -E "[c]laude-desktop .*gpu-stack/profiles/claude" | awk '{print $1}'; }
tick() { s=0; for p in $(ps_); do s=$((s + $(awk '{print $14+$15}' /proc/$p/stat 2>/dev/null || echo 0))); done; echo $s; }
W=$(pgrep -x jsui-wl); a=$(tick); wa=$(awk '{print $14+$15}' /proc/$W/stat); sleep $T; b=$(tick); wb=$(awk '{print $14+$15}' /proc/$W/stat)
rss=0; for p in $(ps_); do rss=$((rss + $(awk '/VmRSS/{print $2}' /proc/$p/status))); done
echo "$M: claude CPU $(( (b - a) / T ))% idle on sign-in page (all processes), jsui-wl $(( (wb - wa) / T ))%, RSS $((rss / 1024)) MB, gpu_busy $(cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage)"
for p in $(ps -eo pid,args | grep -E "[c]laude-desktop .*gpu-stack/profiles/claude" | grep -v -- "--type=" | awk '{print $1}'); do kill $p; done; sleep 4
grep -aE "Context was lost|Exiting GPU|CreateSharedImage" $O/stderr.log | wc -l | sed "s/^/$M: gpu errors: /"
