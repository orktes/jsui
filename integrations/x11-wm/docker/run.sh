#!/bin/sh
# Entry point of the jsui-wm container.
#   run.sh vnc     Xvfb + jsui-wm + x11vnc on :5900 (default)
#   run.sh tour    Xvfb + jsui-wm, then docker/tour.py; screenshots in /out
#   run.sh shell   Xvfb + jsui-wm only (attach with `docker exec ... xdotool`)
# Env: SCREEN (default 1920x1200x24), SCALE (default: from the screen width).
set -e
MODE=${1:-vnc}
SCREEN=${SCREEN:-1920x1200x24}
export DISPLAY=:1
# Apps at the shell's 150 %: Xft.dpi 144 in Xresources scales xterm and GTK text.
# -noreset below: without it Xvfb resets when xrdb (its only client so far)
# disconnects, and the resources are gone.
RUN=/tmp/jsui-wm
mkdir -p $RUN/fb /out
Xvfb :1 -screen 0 "$SCREEN" -fbdir $RUN/fb -noreset -nolisten tcp +extension Composite +extension DAMAGE >$RUN/xvfb.log 2>&1 &
for i in $(seq 1 50); do xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done
xrdb -nocpp -load /etc/jsui-wm/Xresources
SCALE_ARG=""
[ -n "$SCALE" ] && SCALE_ARG="--scale $SCALE"
cd /opt/jsui-wm
./jsui-wm --bundle /opt/jsui-wm/shell.js --anchors $RUN/anchors.json $SCALE_ARG >$RUN/wm.log 2>&1 &
WM=$!
sleep 2
case "$MODE" in
  tour)
    python3 /opt/jsui-wm/tour.py --anchors $RUN/anchors.json --fb $RUN/fb/Xvfb_screen0 --out /out
    status=$?
    cp $RUN/wm.log /out/wm.log
    kill $WM 2>/dev/null || true
    exit $status
    ;;
  vnc)
    echo "jsui-wm is up: connect a VNC viewer to localhost:5900"
    exec x11vnc -display :1 -forever -shared -nopw -quiet -noxdamage
    ;;
  *)
    wait $WM
    ;;
esac
