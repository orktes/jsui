#!/bin/sh
# cd-trace.sh [SECONDS] [egl|vulkan|egl-native]: run claude-desktop-gpu with GPU logging, then stop it.
# Run under with-lock. Pass the GPU-sandbox flag yourself via CD_EXTRA_FLAGS once it is approved:
# Chromium's GPU broker only allows /dev/dri, and Turnip needs /dev/kgsl-3d0 + /dev/ion.
O=$HOME/gpu-stack/logs/cd-trace-${2:-egl}; rm -rf $O; mkdir -p $O
export XDG_RUNTIME_DIR=/run/user/32011 WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus
export LD_PRELOAD=$HOME/jsui-wm/lib/jsui-no-bluetooth.so CLAUDE_GPU_GL=${2:-egl}
$HOME/gpu-stack/bin/wlglobals | grep -q zwp_linux_dmabuf_v1 && export GPU_STACK_DMABUF=1
export CLAUDE_DESKTOP_FLAGS="$CD_EXTRA_FLAGS --enable-logging=stderr --v=1"
setsid $HOME/gpu-stack/bin/claude-desktop-gpu > $O/stderr.log 2>&1 < /dev/null &
sleep ${1:-30}
python3 ~/ctl.py "raw shot $HOME/caps/cd-${2:-egl}.ppm" >/dev/null
top -bn1 -d1 | grep -E "claude-d|jsui-wl" | head -6 > $O/top.txt
# stop the browser process only; killing children first makes the app show its GPU-crash dialog
for p in $(ps -eo pid,args | grep -E "[c]laude-desktop .*gpu-stack/profiles/claude" | grep -v -- "--type=" | awk '{print $1}'); do kill $p; done
sleep 4
grep -aiE "GL_RENDERER|ANGLE.*(Vulkan|OpenGL)|zink|Turnip|Exiting GPU|GPU process|failed to choose|SwiftShader|software compositing" $O/stderr.log | cut -c1-220 | sort | uniq -c | sort -rn | head -25
