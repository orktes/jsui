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
  echo 'user_pref("browser.preonboarding.enabled", false);'
  echo 'user_pref("termsofuse.bypassNotification", true);'
  echo 'user_pref("datareporting.policy.dataSubmissionPolicyBypassNotification", true);'
  echo 'user_pref("browser.startup.homepage_override.mstone", "ignore");'
  if [ -n "$BENCH_NO_TELEMETRY" ]; then
    for k in datareporting.healthreport.uploadEnabled datareporting.policy.dataSubmissionEnabled toolkit.telemetry.enabled \
             toolkit.telemetry.unified browser.newtabpage.activity-stream.feeds.telemetry browser.ping-centre.telemetry \
             app.shield.optoutstudies.enabled browser.discovery.enabled extensions.pocket.enabled; do echo "user_pref(\"$k\", false);"; done
  fi
  if [ $M = gpu ]; then
    echo 'user_pref("gfx.webrender.software", false);'; echo 'user_pref("gfx.webrender.all", true);'
    echo 'user_pref("layers.acceleration.disabled", false);'; echo 'user_pref("layers.acceleration.force-enabled", true);'
  else
    echo 'user_pref("gfx.webrender.software", true);'; echo 'user_pref("layers.acceleration.disabled", true);'
  fi
} > $P/user.js
RUN="$HOME/firefox/firefox"
if [ $M = gpu ]; then
  RUN="$HOME/gpu-stack/bin/gpu-env $RUN"
fi
WAYLAND_DEBUG=${BENCH_WL_DEBUG:-} setsid $RUN --no-remote --profile $P --name firefox-bench file://$HOME/gpu-stack/bench/scroll.html > $O/stdout.log 2>&1 < /dev/null &
sleep 8
W=$(pgrep -x jsui-wl); cpu() { for p in $(ps -eo pid,args | grep -E "[f]irefox(-bin)? .*firefox-bench-$M" | awk '{print $1}') $W; do awk '{print $14+$15}' /proc/$p/stat; done | paste -sd+ | bc 2>/dev/null || true; }
ff() { r=$(ps -eo pid,args | grep -E "[f]irefox(-bin)? .*firefox-bench-$M" | awk '{print $1}'); [ -n "$r" ] && { echo "$r"; for q in $r; do ps -o pid= --ppid $q; done; } | sort -u; }
tick() { s=0; for p in $(ff); do s=$((s + $(awk '{print $14+$15}' /proc/$p/stat))); done; echo $s; }
threads() { for p in $(ff); do for t in /proc/$p/task/*; do echo "$p ${t##*/} $(awk '{print $14+$15}' $t/stat 2>/dev/null) $(cat $t/comm 2>/dev/null | tr ' ' _) $(tr '\0' ' ' < /proc/$p/cmdline | grep -oE -- '-(isForBrowser|contentproc|sandboxingKind)' | head -1)"; done; done; }
python3 ~/ctl.py "drag 1000 1050 1000 480 0.4" >/dev/null 2>&1; sleep 1  # jsui lock screen: a swipe, no PIN
threads > $O/ta; a=$(tick); wa=$(awk '{print $14+$15}' /proc/$W/stat); sleep $((T / 2)); python3 ~/ctl.py "raw shot $HOME/caps/ffb-$M.ppm" >/dev/null; sleep $((T - T / 2)); b=$(tick); wb=$(awk '{print $14+$15}' /proc/$W/stat); threads > $O/tb
echo "$M: firefox CPU $(( (b - a) / T ))% (all processes, 100% = 1 core), jsui-wl CPU $(( (wb - wa) / T ))%, gpu_busy $(cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage) @ $(( $(cat /sys/class/kgsl/kgsl-3d0/gpuclk) / 1000000 )) MHz"
awk -v T=$T 'NR==FNR {a[$1" "$2]=$3; next} ($1" "$2) in a {d=$3-a[$1" "$2]; if (d > 0) printf "%5.1f%% %-18s pid %s %s\n", d/T, $4, $1, ($5 ? "content" : "parent")}' $O/ta $O/tb | sort -rn | head -12 > $O/threads.txt
sed "s/^/$M thread: /" $O/threads.txt
for p in $(ff); do kill $p 2>/dev/null; done; sleep 2
echo "$M: $(grep -a -m1 'gpu-env:' $O/stdout.log || echo 'stock stack (software WebRender)'), wl buffers: $(grep -aoE 'create_immed|wl_shm_pool@[0-9]+\.create_buffer' $O/stdout.log | sed 's/@[0-9]*//' | sort | uniq -c | tr '\n' ' ')"
grep -a "BENCH" $O/stdout.log | sed 's/.*BENCH/BENCH/' | tail -n $((T)) | awk '{for(i=2;i<=NF;i++){split($i,kv,"=");s[kv[1]]+=kv[2];if(kv[2]>m[kv[1]])m[kv[1]]=kv[2]}n++} END {if(n) printf "'$M': %d samples, avg fps %.1f, avg p50 %.1f ms, avg p95 %.1f ms, worst frame %.1f ms\n", n, s["fps"]/n, s["p50"]/n, s["p95"]/n, m["max"]; else print "'$M': no BENCH output"}'
