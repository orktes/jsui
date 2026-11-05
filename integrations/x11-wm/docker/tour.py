#!/usr/bin/env python3
"""Drives jsui-wm on an Xvfb display like a finger would (xdotool) and saves a
screenshot after each step. Taps shell elements by name through the anchors
file the WM writes (--anchors), so it keeps working when the layout changes.

  tour.py --anchors /tmp/anchors.json --fb /tmp/fb/Xvfb_screen0 --out shots/
"""
import argparse, json, os, re, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument('--anchors', default='/tmp/jsui-wm/anchors.json')
ap.add_argument('--fb', default='/tmp/jsui-wm/fb/Xvfb_screen0', help='Xvfb -fbdir screen file')
ap.add_argument('--out', default='shots')
ap.add_argument('--only', default='', help='comma-separated step names to keep')
args = ap.parse_args()
os.makedirs(args.out, exist_ok=True)
failures = []


def sh(*cmd):
    return subprocess.run(cmd, check=False, capture_output=True, text=True).stdout


def anchors():
    try:
        with open(args.anchors) as f:
            return json.load(f)
    except Exception:
        return {}


def wait_anchor(pattern, timeout=8.0):
    """First anchor whose name matches `pattern` (regex), waiting for it."""
    end = time.time() + timeout
    rx = re.compile(pattern)
    while time.time() < end:
        found = [(n, r) for n, r in anchors().items() if rx.fullmatch(n)]
        # Prefer one that is on screen (cards scrolled out of view keep anchors).
        found.sort(key=lambda nr: not (0 <= nr[1][0] + nr[1][2] // 2 < 1920))
        if found:
            return found[0]
        time.sleep(0.1)
    failures.append(f'no anchor {pattern}')
    print(f'  !! no anchor matching {pattern}', file=sys.stderr)
    return None, None


def center(r):
    return r[0] + r[2] // 2, r[1] + r[3] // 2


def tap(pattern, settle=0.6):
    name, r = wait_anchor(pattern)
    if not r:
        return False
    x, y = center(r)
    print(f'  tap {name} at {x},{y}')
    sh('xdotool', 'mousemove', str(x), str(y), 'click', '1')
    time.sleep(settle)
    return True


def tap_xy(x, y, settle=0.6):
    sh('xdotool', 'mousemove', str(x), str(y), 'click', '1')
    time.sleep(settle)


def drag(x0, y0, x1, y1, steps=24, settle=0.8, hold=0.0):
    """A finger drag at ~60 Hz, in one xdotool run so the timing is real."""
    cmd = ['xdotool', 'mousemove', str(x0), str(y0), 'mousedown', '1', 'sleep', '0.03']
    for i in range(1, steps + 1):
        cmd += ['mousemove', str(x0 + (x1 - x0) * i // steps), str(y0 + (y1 - y0) * i // steps), 'sleep', '0.016']
    if hold:
        cmd += ['sleep', str(hold)]
    cmd += ['mouseup', '1']
    sh(*cmd)
    time.sleep(settle)


def drag_anchor(pattern, dx, dy, **kw):
    name, r = wait_anchor(pattern)
    if not r:
        return
    x, y = r[0] + min(r[2] // 2, 260), r[1] + r[3] // 2
    print(f'  drag {name} by {dx},{dy}')
    drag(x, y, x + dx, y + dy, **kw)


def type_text(text):
    sh('xdotool', 'type', '--delay', '30', text)


def key(k):
    sh('xdotool', 'key', k)


def shot(name):
    if args.only and name not in args.only.split(','):
        return
    time.sleep(0.4)
    path = os.path.join(args.out, name + '.png')
    subprocess.run(['convert', 'xwd:' + args.fb, path], check=False)
    print(f'shot {path}')


def windows():
    return sorted({m.group(1) for n in anchors() for m in [re.match(r'win\.(\d+)\.title', n)] if m}, key=int)


def launch(app_pattern, query, settle=2.0):
    """Opens the launcher, searches for `query` and taps the app tile."""
    tap(r'topbar\.apps')
    type_text(query)
    time.sleep(0.5)
    if tap(rf'app\.({app_pattern})', settle=settle):
        return True
    key('Escape')
    return False


def dock_swipe(dx=0, dy=0):
    name, r = wait_anchor(r'dock\.overview')
    if r:
        x, y = r[0] - 180, r[1] + r[3] // 2
        drag(x, y, x + dx, y + dy, steps=12, settle=1.2)


def title_of(w):
    return wait_anchor(rf'win\.{w}\.title', 2)[1]


# ------------------------------------------------------------------ the tour
time.sleep(1.0)
shot('01-desktop')

tap(r'topbar\.apps', settle=0.8)
shot('02-launcher')
type_text('calc')
time.sleep(0.6)
shot('03-launcher-search')
key('Escape')
tap_xy(40, 1150)
time.sleep(0.4)

# Tablet mode (the default): apps open full screen.
launch(r'debian-xterm|xterm', 'xterm')
time.sleep(1.0)
shot('04-app-fullscreen')
launch('galculator', 'galc')
launch(r'org\.xfce\.mousepad|mousepad', 'mousepad', settle=2.5)

# The switcher: swipe up on the dock.
dock_swipe(dy=-300)
shot('05-switcher')
# Fling through the cards.
drag(1500, 600, 500, 600, steps=8, settle=1.2)
shot('06-switcher-scrolled')
drag(500, 600, 1500, 600, steps=8, settle=1.2)
# Bring the next card to the middle and split it with the current app.
drag(1300, 600, 900, 600, steps=10, settle=1.2)
for attempt in range(3):
    time.sleep(0.6)
    tap(r'switcher\.\d+\.split', settle=1.5)
    if wait_anchor(r'split\.divider', 2)[1]:
        failures[:] = [f for f in failures if 'split' not in f]
        break
shot('07-split-view')
# Drag the divider to give the left app more room.
name, r = wait_anchor(r'split\.divider')
if r:
    x, y = center(r)
    drag(x, y, x + 300, y, steps=16, settle=1.2)
shot('08-split-resized')

# Quick switch: swipe sideways on the dock jumps to the previous app.
dock_swipe(dx=-260)
shot('09-quick-switch')

# Quick settings; turn tablet mode off for floating windows.
tap(r'topbar\.status', settle=0.8)
shot('10-quick-settings')
tap(r'qs\.tablet', settle=0.5)
tap_xy(200, 1000)
launch('xclock', 'clock', settle=1.5)
launch('xeyes', 'eyes', settle=1.5)
shot('11-floating-windows')
ws = windows()
if ws:
    r = title_of(ws[-1])
    if r:
        drag(r[0] + 120, r[1] + r[3] // 2, 1916, 600, steps=24, settle=1.2, hold=0.3)
shot('12-snap-right')

# On-screen keyboard into the terminal.
for w in windows():
    r = title_of(w)
    if r and r[2] > 0:
        pass
tap(r'dock\.(debian-xterm|xterm)', settle=0.8)
tap(r'topbar\.keyboard', settle=0.8)
for k in ['l', 's', ' ', '-', 'l', 'a', ' ', '/', 'Enter']:
    tap(rf'osk\.{re.escape(k)}', settle=0.15)
time.sleep(0.8)
shot('13-osk-terminal')
tap(r'osk\.Hide', settle=0.6)

# Light style.
tap(r'topbar\.status', settle=0.6)
tap(r'qs\.dark', settle=0.8)
shot('14-light-quick-settings')
tap_xy(200, 1000)
time.sleep(0.4)
shot('15-light-desktop')
tap(r'topbar\.status', settle=0.6)
tap(r'qs\.dark', settle=0.4)
tap_xy(200, 1000)

# Calendar and notifications.
tap(r'topbar\.clock', settle=0.8)
shot('16-calendar')
tap_xy(200, 1000)

# Close everything from the switcher.
dock_swipe(dy=-300)
tap(r'overview\.closeAll', settle=2.0)
shot('17-closed-all')

print('failures:', failures)
sys.exit(1 if failures else 0)
