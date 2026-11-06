#!/bin/sh
# jsui-wl --mir against mir-test-server (see Dockerfile): the session comes
# up as a fullscreen Mir window, a Mir touch on the Apps button opens the
# launcher, and an app opens maximized. Screenshots (jsui-wl's frame, which
# is what it hands Mir) go to /out.
set -e
D=${JSUI_WM_DIR:-/opt/jsui-wm}
OUT=${OUT:-/out}
mkdir -p "$OUT"
export XDG_RUNTIME_DIR=/tmp/xdg MIR_SERVER_ENABLE_MIRCLIENT=1
mkdir -p -m700 $XDG_RUNTIME_DIR
P=/usr/lib/$(gcc -dumpmachine 2>/dev/null || echo x86_64-linux-gnu)/mir1/server-platform
[ -d "$P" ] || P=$(dirname "$(ls /usr/lib/*/mir1/server-platform/graphics-dummy.so | head -1)")
"$D/mir-test-server" --platform-graphics-lib $P/graphics-dummy.so --platform-input-lib $P/input-stub.so \
  --file /tmp/mir_socket >/tmp/mir.log 2>&1 &
SERVER=$!
sleep 2
MIR_SERVER_NAME=session-0 "$D/jsui-wl" --mir-socket /tmp/mir_socket --bundle "$D/shell.js" \
  --control /tmp/jsui.ctl --anchors /tmp/anchors.json >/tmp/wl.log 2>&1 &
sleep 6
ctl() { python3 -c "
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect('/tmp/jsui.ctl'); s.sendall(sys.argv[1].encode() + b'\n'); time.sleep(1)" "$1"; }
anchor() { python3 -c "
import json, sys
r = json.load(open('/tmp/anchors.json'))[sys.argv[1]]; print(r[0] + r[2] // 2, r[1] + r[3] // 2)" "$1"; }
touch_at() { printf 'down %s\nup %s\n' "$1" "$1" > /tmp/touch.cmd; sleep 1.5; }
shot() { ctl "shot /tmp/$1.ppm"; convert /tmp/$1.ppm "$OUT/$1.png"; echo "shot $OUT/$1.png"; }

shot mir-01-desktop
touch_at "$(anchor topbar.apps)"
shot mir-02-launcher
grep -q '"app\.' /tmp/anchors.json || { echo "FAIL: the Mir touch didn't open the launcher"; cat /tmp/wl.log; exit 1; }
touch_at "$(anchor app.org.codeberg.dnkl.foot 2>/dev/null || anchor app.foot)"
sleep 2
shot mir-03-app
kill -USR1 $SERVER; sleep 0.5
grep "surface at" /tmp/mir.log | tail -1
cat /tmp/wl.log
grep -q "map " /tmp/wl.log || { echo "FAIL: no app window"; exit 1; }
echo "ok"
