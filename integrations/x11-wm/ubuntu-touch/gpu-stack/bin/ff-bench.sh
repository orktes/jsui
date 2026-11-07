#!/bin/sh
# ff-bench.sh gpu|sw [SECONDS]: auto-scroll bench/scroll.html in Firefox, GPU WebRender on
# ~/gpu-stack (gpu) or software WebRender on the stock stack (sw, like ~/.local/bin/firefox).
# Prints per-second rAF fps and CPU. Run under with-lock.
M=${1:-gpu}; T=${2:-20}; O=$HOME/gpu-stack/logs/ff-bench-$M; rm -rf $O; mkdir -p $O
P=$HOME/gpu-stack/profiles/firefox-bench-$M; mkdir -p $P
export XDG_RUNTIME_DIR=/run/user/32011 WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus
export LD_PRELOAD=$HOME/jsui-wm/lib/jsui-no-bluetooth.so MOZ_ENABLE_WAYLAND=1
{
  echo 'user_pref("devtools.console.stdout.content", true);'
  echo 'user_pref("browser.shell.checkDefaultBrowser", false);'
  echo 'user_pref("browser.aboutwelcome.enabled", false);'
  echo 'user_pref("layout.css.devPixelsPerPx", "1.75");'
  echo 'user_pref("general.smoothScroll", false);'
  if [ $M = gpu ]; then
    echo 'user_pref("gfx.webrender.software", false);'; echo 'user_pref("gfx.webrender.all", true);'
    echo 'user_pref("layers.acceleration.disabled", false);'; echo 'user_pref("layers.acceleration.force-enabled", true);'
  else
    echo 'user_pref("gfx.webrender.software", true);'; echo 'user_pref("layers.acceleration.disabled", true);'
  fi
} > $P/user.js
RUN="$HOME/firefox/firefox"
if [ $M = gpu ]; then
  $HOME/gpu-stack/bin/wlglobals | grep -q zwp_linux_dmabuf_v1 && export GPU_STACK_DMABUF=1
  RUN="$HOME/gpu-stack/bin/gpu-env $RUN"
fi
setsid $RUN --no-remote --profile $P --name firefox-bench file://$HOME/gpu-stack/bench/scroll.html > $O/stdout.log 2>&1 < /dev/null &
sleep 8
W=$(pgrep -x jsui-wl); cpu() { for p in $(ps -eo pid,args | grep -E "[f]irefox(-bin)? .*firefox-bench-$M" | awk '{print $1}') $W; do awk '{print $14+$15}' /proc/$p/stat; done | paste -sd+ | bc 2>/dev/null || true; }
ff() { ps -eo pid,args | grep -E "[f]irefox(-bin)? .*firefox-bench-$M" | awk '{print $1}'; }
tick() { s=0; for p in $(ff); do s=$((s + $(awk '{print $14+$15}' /proc/$p/stat))); done; echo $s; }
a=$(tick); wa=$(awk '{print $14+$15}' /proc/$W/stat); sleep $T; b=$(tick); wb=$(awk '{print $14+$15}' /proc/$W/stat)
echo "$M: firefox CPU $(( (b - a) / T ))% (all processes, 100% = 1 core), jsui-wl CPU $(( (wb - wa) / T ))%, gpu_busy $(cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage) @ $(( $(cat /sys/class/kgsl/kgsl-3d0/gpuclk) / 1000000 )) MHz"
for p in $(ff); do kill $p 2>/dev/null; done; sleep 2
grep -a "BENCH" $O/stdout.log | sed 's/.*BENCH/BENCH/' | tail -n $((T)) | awk '{for(i=2;i<=NF;i++){split($i,kv,"=");s[kv[1]]+=kv[2];if(kv[2]>m[kv[1]])m[kv[1]]=kv[2]}n++} END {if(n) printf "'$M': %d samples, avg fps %.1f, avg p50 %.1f ms, avg p95 %.1f ms, worst frame %.1f ms\n", n, s["fps"]/n, s["p50"]/n, s["p95"]/n, m["max"]; else print "'$M': no BENCH output"}'
