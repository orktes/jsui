#!/bin/sh
# ff-trace.sh [SECONDS]: run firefox-gpu on dmabuf with Wayland widget + protocol logging,
# take a shot, stop it. Run under with-lock. Logs: ~/gpu-stack/logs/ff-trace/
O=$HOME/gpu-stack/logs/ff-trace; rm -rf $O; mkdir -p $O ~/caps
export XDG_RUNTIME_DIR=/run/user/32011 WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus
export LD_PRELOAD=$HOME/jsui-wm/lib/jsui-no-bluetooth.so
echo "dmabuf: ${GPU_STACK_DMABUF:-no (wl_shm)}"
export MOZ_LOG="WidgetWayland:5,Widget:4,WidgetPopup:0" MOZ_LOG_FILE=$O/moz WAYLAND_DEBUG=client
setsid $HOME/gpu-stack/bin/firefox-gpu https://en.wikipedia.org/wiki/Adreno > $O/stderr.log 2>&1 < /dev/null &
sleep ${1:-25}
python3 ~/ctl.py "raw shot $HOME/caps/ff-trace.ppm" >/dev/null
for p in $(ps -eo pid,args | grep -E "[f]irefox(-bin)? .*gpu-stack/profiles" | awk '{print $1}'); do kill $p; done
sleep 2
grep -aoE "(create_immed|wl_shm_pool@[0-9]+.create_buffer)\\(new id wl_buffer@[0-9]+, [0-9]+, [0-9]+, [0-9]+" $O/stderr.log | sed "s/@[0-9]*//g" | sort | uniq -c
: grep -aoE "create_immed\(new id wl_buffer@[0-9]+, [0-9]+, [0-9]+" $O/stderr.log | awk '{print $5,$6}' | sort | uniq -c
