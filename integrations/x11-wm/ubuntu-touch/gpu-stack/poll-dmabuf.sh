#!/bin/bash
# Mac side: wait until the tablet's deploy.lock is gone and jsui-wl advertises
# zwp_linux_dmabuf_v1, then run ~/gpu-stack/bin/dmabuf-check.sh and pull its results.
D=${GPU_STACK_RESULTS:-$(dirname "$0")/dmabuf-results}; mkdir -p $D
for i in $(seq 1 ${1:-110}); do
  st=$(adb shell '[ -e ~/caps/deploy.lock ] && echo locked || (XDG_RUNTIME_DIR=/run/user/32011 WAYLAND_DISPLAY=wayland-0 ~/gpu-stack/bin/wlglobals | grep -q zwp_linux_dmabuf_v1 && echo ready || echo nodmabuf)' 2>/dev/null | tr -d '\r')
  echo "$(date +%H:%M:%S) $st"
  if [ "$st" = ready ]; then
    adb shell '~/gpu-stack/bin/dmabuf-check.sh' 2>&1 | tee $D/summary.txt
    for f in $(adb shell 'ls ~/caps/dmabuf-*.ppm 2>/dev/null' | tr -d '\r'); do
      n=$(basename $f .ppm); adb pull $f $D/$n.ppm >/dev/null && adb shell "rm -f $f" && sips -s format png -Z 1400 $D/$n.ppm --out $D/$n.png >/dev/null && rm $D/$n.ppm
    done
    adb pull /home/phablet/gpu-stack/logs/dmabuf-check $D/logs >/dev/null 2>&1
    exit 0
  fi
  sleep 60
done
echo "gave up"
