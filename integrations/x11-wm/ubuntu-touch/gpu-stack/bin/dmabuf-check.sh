#!/bin/sh
# dmabuf-check.sh: once jsui-wl advertises zwp_linux_dmabuf_v1, run the GPU clients on the
# real dmabuf path (no MESA_VK_WSI_DEBUG=sw) and log fps, buffer params and protocol errors.
# Exit 2 if the compositor has no linux-dmabuf yet. Output: ~/gpu-stack/logs/dmabuf-check/
export XDG_RUNTIME_DIR=/run/user/32011 WAYLAND_DISPLAY=wayland-0
B=$HOME/gpu-stack/bin; O=$HOME/gpu-stack/logs/dmabuf-check
G=$($B/wlglobals | grep zwp_linux_dmabuf_v1)
[ -n "$G" ] || { echo "no zwp_linux_dmabuf_v1 yet"; exit 2; }
L=$HOME/caps/deploy.lock
# The shell thread deploys jsui-wl under this lock; wait for it, then hold it while testing.
n=0; while [ -e $L ]; do [ $n -ge 120 ] && { echo "deploy.lock held: $(cat $L)"; exit 3; }; sleep 10; n=$((n + 1)); done
echo "gpu-stack (Turnip/Zink thread)" > $L; trap 'rm -f $L' EXIT INT TERM
rm -rf $O; mkdir -p $O ~/caps
echo "compositor: $G" | tee $O/summary.txt
export GPU_STACK_DMABUF=1 MESA_VK_WSI_DEBUG=
shot() { python3 ~/ctl.py "raw shot $HOME/caps/dmabuf-$1.ppm" >/dev/null; }
# wlgl: Zink EGL, logs render vs swap time; WAYLAND_DEBUG shows the dmabuf params it creates
WAYLAND_DEBUG=client timeout 20 $B/gpu-env $B/wlgl 8 1 > $O/wlgl1.txt 2> $O/wlgl1-wl.log &
sleep 5; shot wlgl; wait
grep -E "fps|renderer" $O/wlgl1.txt | tee -a $O/summary.txt
grep -m3 -E "zwp_linux_buffer_params_v1@[0-9]+\.(add|create|create_immed)\(|format_table|main_device|tranche" $O/wlgl1-wl.log | tee -a $O/summary.txt
grep -E "error|failed" $O/wlgl1-wl.log $O/wlgl1.txt | grep -v plugin | head -5 | tee -a $O/summary.txt
timeout 20 $B/gpu-env $B/wlgl 8 4 2>&1 | grep fps | sed 's/^/4 quads: /' | tee -a $O/summary.txt
WLGL_SWAP0=1 timeout 20 $B/gpu-env $B/wlgl 6 1 2>&1 | grep fps | sed 's/^/swapinterval 0: /' | tee -a $O/summary.txt
# vkcube (Turnip WSI directly)
s=$(date +%s%N); timeout 60 $B/gpu-env $B/vkcube-wayland --c 300 > $O/vkcube.txt 2>&1; rc=$?; e=$(date +%s%N)
echo "vkcube 300 frames rc=$rc: $(( 300000000000 / (e - s) )) fps" | tee -a $O/summary.txt
tail -3 $O/vkcube.txt >> $O/summary.txt
# eglgears with the Gallium HUD
$B/hudrun.sh 10 $B/eglgears_wayland 2>&1 | sed 's/^/eglgears: /' | tee -a $O/summary.txt
# Firefox (WebRender on Zink); leaves it running for a look, logs in $O/firefox.log
LD_PRELOAD=$HOME/jsui-wm/lib/jsui-no-bluetooth.so DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus \
  setsid nohup $B/firefox-gpu https://en.wikipedia.org/wiki/Special:Random > $O/firefox.log 2>&1 < /dev/null &
sleep 25; shot firefox
grep -iE "error|fail" $O/firefox.log | grep -v -E "Sandbox|texture_lod" | head -5 | sed 's/^/firefox: /' | tee -a $O/summary.txt
echo "shots: $(ls ~/caps/dmabuf-*.ppm 2>/dev/null | tr '\n' ' ')" | tee -a $O/summary.txt
