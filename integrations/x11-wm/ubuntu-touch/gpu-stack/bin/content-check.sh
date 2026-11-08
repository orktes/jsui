#!/bin/sh
# content-check.sh: run each GPU client briefly with MESA_WSI_DMABUF_CHECK so Turnip's WSI logs
# what is actually in the dmabufs it hands to the compositor (centre pixel, % non-zero).
# Run under with-lock. Logs: ~/gpu-stack/logs/content-check/
O=$HOME/gpu-stack/logs/content-check; rm -rf $O; mkdir -p $O
B=$HOME/gpu-stack/bin
export XDG_RUNTIME_DIR=/run/user/32011 WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus
export LD_PRELOAD=$HOME/jsui-wm/lib/jsui-no-bluetooth.so MESA_WSI_DMABUF_CHECK=30
shot() { python3 ~/ctl.py "raw shot $HOME/caps/cc-$1.ppm" >/dev/null; }
# jsui's own lock screen (a swipe, no PIN); shot first so a PIN pad would show up in the captures
python3 ~/ctl.py "drag 1000 1050 1000 480 0.4" >/dev/null; sleep 1; shot unlock
r() { grep -a "WSI-CHECK\|gpu-env:" $O/$1.log | head -${2:-4} | sed "s/^/$1: /"; }
(timeout 15 $B/gpu-env $B/vkcube-wayland --c 300 > $O/vkcube.log 2>&1 &); sleep 4; shot vkcube; sleep 6; r vkcube
timeout 15 $B/gpu-env $B/wlgl 4 1 > $O/wlgl.log 2>&1; r wlgl
# Firefox: the bench page (gradient cards) in a fresh bench profile
P=$HOME/gpu-stack/profiles/firefox-check; rm -rf $P; mkdir -p $P
printf '%s\n' 'user_pref("gfx.webrender.software", false);' 'user_pref("gfx.webrender.all", true);' \
  'user_pref("layers.acceleration.disabled", false);' 'user_pref("browser.shell.checkDefaultBrowser", false);' \
  'user_pref("browser.aboutwelcome.enabled", false);' 'user_pref("layout.css.devPixelsPerPx", "1.75");' > $P/user.js
MOZ_ENABLE_WAYLAND=1 setsid $B/gpu-env $HOME/firefox/firefox --no-remote --profile $P --name firefox-check \
  file://$HOME/gpu-stack/bench/scroll.html > $O/firefox.log 2>&1 < /dev/null &
sleep 20; shot firefox
for p in $(ps -eo pid,args | grep -E "[f]irefox(-bin)? .*firefox-check" | grep -v contentproc | awk '{print $1}'); do kill $p; done; sleep 3
r firefox 6
# Claude Desktop (test profile, ANGLE-GL; GPU sandbox flag approved by Jaakko for this launcher)
F=$HOME/gpu-stack/profiles/claude/claude_desktop_config.json
[ -f $F ] && sed -i 's/"isHardwareAcceleration\([A-Za-z]*\)": true/"isHardwareAcceleration\1": false/g' $F
CLAUDE_GPU_GL=egl CLAUDE_DESKTOP_FLAGS="$CD_EXTRA_FLAGS" setsid $B/claude-desktop-gpu > $O/claude.log 2>&1 < /dev/null &
sleep 25; shot claude
for p in $(ps -eo pid,args | grep -E "[c]laude-desktop .*gpu-stack/profiles/claude" | grep -v -- "--type=" | awk '{print $1}'); do kill $p; done; sleep 4
r claude 6
