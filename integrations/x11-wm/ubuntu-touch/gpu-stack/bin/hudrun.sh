#!/bin/sh
# hudrun.sh SECONDS CMD...: run a GL client on jsui-wl through ~/gpu-stack and print
# median fps / cpu from the Gallium HUD dump, plus jsui-wl CPU and GPU busy samples.
T=$1; shift
export XDG_RUNTIME_DIR=/run/user/32011 WAYLAND_DISPLAY=wayland-0
. ~/gpu-stack/env.sh
export MESA_VK_WSI_DEBUG=${MESA_VK_WSI_DEBUG-sw}
H=$HOME/gpu-stack/logs/hud; rm -rf $H; mkdir -p $H
GALLIUM_HUD=fps,cpu GALLIUM_HUD_DUMP_DIR=$H GALLIUM_HUD_PERIOD=1 timeout -s INT $T "$@" > $HOME/gpu-stack/logs/hudrun.log 2>&1 &
sleep $((T / 2)); W=$(pgrep -x jsui-wl)
a=$(awk '{print $14+$15}' /proc/$W/stat); sleep 2; b=$(awk '{print $14+$15}' /proc/$W/stat)
echo "jsui-wl cpu: $(( (b - a) / 2 ))% gpu_busy: $(cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage) gpuclk: $(cat /sys/class/kgsl/kgsl-3d0/gpuclk)"
wait
for f in fps cpu; do [ -f $H/$f ] && echo "$f: $(sort -n $H/$f | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}')"; done
grep -v "plugin" $HOME/gpu-stack/logs/hudrun.log | head -5
