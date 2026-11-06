#!/bin/sh
# Entry point of the jsui-wm container.
#   run.sh vnc     the desktop + a VNC server on :5900 (default)
#   run.sh tour    the desktop, then docker/tour.py; screenshots in /out
#   run.sh shell   the desktop only
# Env:
#   WM      x11 (default: Xvfb + jsui-wm) or wayland (headless wlroots: jsui-wl)
#   SCREEN  default 1920x1200x24
#   SCALE   default: from the screen width
set -e
MODE=${1:-vnc}
WM=${WM:-x11}
SCREEN=${SCREEN:-1920x1200x24}
SIZE=$(echo "$SCREEN" | cut -dx -f1-2)
RUN=/tmp/jsui-wm
mkdir -p $RUN/fb /out
SCALE_ARG=""
[ -n "$SCALE" ] && SCALE_ARG="--scale $SCALE"
cd /opt/jsui-wm

if [ "$WM" = wayland ]; then
  # wlroots without a screen or GPU: the headless backend, pixman rendering.
  export XDG_RUNTIME_DIR=$RUN/xdg WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
  mkdir -p $XDG_RUNTIME_DIR && chmod 700 $XDG_RUNTIME_DIR
  unset DISPLAY
  ./jsui-wl --bundle /opt/jsui-wm/shell.js --anchors $RUN/anchors.json --control $RUN/control \
    --size "$SIZE" $SCALE_ARG >$RUN/wm.log 2>&1 &
  PID=$!
  for i in $(seq 1 50); do [ -S $XDG_RUNTIME_DIR/wayland-0 ] && break; sleep 0.1; done
  export WAYLAND_DISPLAY=wayland-0
  DRIVE="--wayland $RUN/control"
  VNC="wayvnc 0.0.0.0 5900"
else
  export DISPLAY=:1
  # Apps at the shell's 150 %: Xft.dpi 144 in Xresources scales xterm and GTK
  # text. -noreset: without it Xvfb resets when xrdb (its only client so far)
  # disconnects, and the resources are gone.
  Xvfb :1 -screen 0 "$SCREEN" -fbdir $RUN/fb -noreset -nolisten tcp +extension Composite +extension DAMAGE >$RUN/xvfb.log 2>&1 &
  for i in $(seq 1 50); do xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done
  xrdb -nocpp -load /etc/jsui-wm/Xresources
  ./jsui-wm --bundle /opt/jsui-wm/shell.js --anchors $RUN/anchors.json $SCALE_ARG >$RUN/wm.log 2>&1 &
  PID=$!
  DRIVE="--fb $RUN/fb/Xvfb_screen0"
  VNC="x11vnc -display :1 -forever -shared -nopw -quiet -noxdamage"
fi
sleep 2

case "$MODE" in
  tour)
    python3 /opt/jsui-wm/tour.py --anchors $RUN/anchors.json $DRIVE --out /out && status=0 || status=$?
    cp $RUN/wm.log /out/wm.log
    kill $PID 2>/dev/null || true
    exit $status
    ;;
  vnc)
    echo "jsui-wm ($WM) is up: connect a VNC viewer to localhost:5900"
    exec $VNC
    ;;
  *)
    wait $PID
    ;;
esac
