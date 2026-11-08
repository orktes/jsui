#!/bin/sh
# cd-verify.sh: run claude-desktop-gpu (CLAUDE_GPU_GL, default egl-native) and show what its GPU
# process loaded and which buffers the window uses. Run under with-lock.
O=$HOME/gpu-stack/logs/cd-verify; rm -rf $O; mkdir -p $O
export XDG_RUNTIME_DIR=/run/user/32011 WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus
export LD_PRELOAD=$HOME/jsui-wm/lib/jsui-no-bluetooth.so CLAUDE_GPU_GL=${CLAUDE_GPU_GL:-egl-native}
export CLAUDE_DESKTOP_FLAGS="$CD_EXTRA_FLAGS" WAYLAND_DEBUG=client
setsid $HOME/gpu-stack/bin/claude-desktop-gpu > $O/stderr.log 2>&1 < /dev/null &
sleep 20
G=$(ps -eo pid,args | grep -E "[c]laude-desktop .*--type=gpu-process" | awk "{print \$1}" | head -1); [ -n "$G" ] || G=$(ps -eo pid,args | grep -E "[c]laude-desktop .*profiles/claude" | grep -v -- --type= | awk "{print \$1}" | head -1)
echo "gpu-process $G maps:"; grep -oE "lib(gallium|vulkan_freedreno|EGL_mesa|vulkan|GLESv2|EGL)[^ ]*" /proc/$G/maps | sort -u
echo "gpu-process cpu: $(ps -o %cpu= -p $G)  browser cpu: $(ps -eo %cpu,args | grep -E "[c]laude-desktop .*profiles/claude" | grep -v -- --type= | awk "{print \$1}")"
for q in $(ps -eo pid,args | grep -E "[c]laude-desktop .*profiles/claude" | awk "{print \$1}"); do n=$(ls -l /proc/$q/fd 2>/dev/null | grep -c dmabuf); [ $n -gt 0 ] && echo "pid $q ($(tr "\\0" " " < /proc/$q/cmdline | grep -oE -- "--type=[a-z-]*" || echo browser)) dmabuf fds: $n"; done
echo "jsui-wl cpu: $(ps -o %cpu= -C jsui-wl)"
for p in $(ps -eo pid,args | grep -E "[c]laude-desktop .*gpu-stack/profiles/claude" | grep -v -- "--type=" | awk "{print \$1}"); do kill $p; done
sleep 4
grep -aoE "(zwp_linux_buffer_params_v1@[0-9]+\.create_immed|wl_shm_pool@[0-9]+\.create_buffer)\(new id wl_buffer@[0-9]+, [0-9]+, [0-9]+, [0-9]+" $O/stderr.log | sed "s/@[0-9]*//g" | sort | uniq -c
grep -aoE "zwp_linux_buffer_params_v1@[0-9]+\.add\([^)]*\)" $O/stderr.log | sed "s/@[0-9]*//; s/fd [0-9]*/fd/" | sort | uniq -c | head -5
