#!/usr/bin/env python3
"""Window state matrix for jsui-wl: puts apps through every window state the
shell has (maximized, side by side with the divider moved, both turns of the
screen, the on-screen keyboard shown / hidden / dragged away, the switcher,
fullscreen, minimize and restore, launches in each state) and after every
transition checks, from jsui-wl's own state (control socket `windows`), that
each shown window fills its room 1:1: its size and place are its room's, it
was told that size, it isn't scaled, and its frame covers it (no stale smaller
frame, no shadow margin drawn). It records how long each transition took to
settle (a frame of the right size shown); over SLOW_MS is reported as slow,
never settling within SETTLE_S as a failure, with a screenshot.

    state_matrix.py [--apps ff,claude,foot,calc] [--out DIR] [--only NAME,...]

Run on the device, as the session's user, against a jsui-wl started with
--control (and --anchors for the gesture steps). Apps are started the way the
shell starts them (ut-launch2.py-style: jsui-wl's environment). App specs
(APPS) name the test profiles: never a person's own browser profile.
Exit status: the number of failed checks.
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recscan  # noqa: E402

RUN = os.environ.get("XDG_RUNTIME_DIR", "/run/user/32011")
CTL = os.path.join(RUN, "jsui-wl.ctl")
ANCHORS = os.path.join(RUN, "jsui-anchors.json")
HOME = os.path.expanduser("~")
SLOW_MS = 300
SETTLE_S = 3.0
REC_FRAMES = 36  # frames recorded per step (~0.6-1 s)

# name: (Exec, extra env, process pattern for pkill, needs time to start)
APPS = {
    "ff": (f"sh -c '. {HOME}/gpu-stack/env.sh; MOZ_ENABLE_WAYLAND=1 exec {HOME}/firefox/firefox --no-remote "
           f"-profile {HOME}/caps/ffb-gpu about:support'", "", "[c]aps/ffb-gpu", 25),
    "claude": (f"{HOME}/.local/bin/claude-desktop", f"CLAUDE_DESKTOP_FLAGS=--user-data-dir={HOME}/caps/cdtest",
               "[c]aps/cdtest", 20),
    "foot": (f"{HOME}/jsui-wm/foot", "", "[j]sui-wm/foot", 4),
    "calc": ("CALC", "", "[c]alculator", 6),
}


class Ctl:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(CTL)
        self.f = self.s.makefile("rwb")

    def __call__(self, line):
        self.f.write((line + "\n").encode())
        self.f.flush()
        return self.f.readline().decode().strip()

    def windows(self):
        return json.loads(self("windows"))


ctl = None


def anchors():
    try:
        with open(ANCHORS) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def tap(x, y):
    ctl(f"touch 1 0 {x} {y}")
    time.sleep(0.05)
    ctl(f"touch 1 2 {x} {y}")


def drag(x0, y0, x1, y1, dur=0.4, n=24):
    ctl(f"touch 2 0 {x0} {y0}")
    for i in range(1, n + 1):
        time.sleep(dur / n)
        ctl(f"touch 2 1 {x0 + (x1 - x0) * i / n:.1f} {y0 + (y1 - y0) * i / n:.1f}")
    ctl(f"touch 2 2 {x1} {y1}")


def screen():
    return ctl.windows()["screen"]


def jsui_env():
    pid = subprocess.check_output(["pgrep", "-x", "jsui-wl"]).split()[0].decode()
    with open(f"/proc/{pid}/environ") as f:
        env = dict(e.split("=", 1) for e in f.read().split("\0") if "=" in e)
    with open(f"/proc/{pid}/cmdline") as f:
        args = f.read().split("\0")
    for i, a in enumerate(args):
        if a == "--app-env" and i + 1 < len(args):
            k, v = args[i + 1].split("=", 1)
            if v:
                env[k] = v
            else:
                env.pop(k, None)
    return env


def calc_exec():
    d = os.path.join(HOME, ".local/share/applications")
    for n in sorted(os.listdir(d)):
        if n.startswith("calculator.ubports") and n.endswith(".desktop"):
            return os.path.join(d, n)
    return None


def launch(name, tries=2):
    for _ in range(tries):
        wid = launch_once(name)
        if wid is not None:
            return wid
        print(f"    {name}: no window, trying again", flush=True)
        stop(name)
    return None


def stop(name):
    """Ends the app and waits for it to be gone (a single-instance app
    started again too soon hands its window to the dying one)."""
    pat = APPS[name][2]
    subprocess.run(["pkill", "-f", pat])
    for _ in range(100):
        if subprocess.run(["pgrep", "-f", pat], stdout=subprocess.DEVNULL).returncode != 0:
            break
        time.sleep(0.1)
    time.sleep(1)


def launch_once(name):
    ex, extra, _, wait = APPS[name]
    before = {w["id"] for w in ctl.windows()["windows"]}
    if ex == "CALC":
        desk = calc_exec()
        env = dict(os.environ)
        subprocess.run(["python3", os.path.join(HOME, "ut-launch2.py"), desk, "/dev/null"], env=env,
                       stdout=subprocess.DEVNULL)
    else:
        env = jsui_env()
        env.update(dict(x.split("=", 1) for x in extra.split() if "=" in x))
        subprocess.Popen(["sh", "-c", ex], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         stdin=subprocess.DEVNULL, start_new_session=True)
    t0 = time.time()
    while time.time() - t0 < wait + 30:
        new = [w for w in ctl.windows()["windows"] if w["id"] not in before and w["mapped"] and w["ready"]
               and not w["dialog"]]
        if new:
            time.sleep(wait if name in ("ff", "claude") and time.time() - t0 < wait else 1.0)
            return new[0]["id"]
        time.sleep(0.2)
    return None


def close_all():
    for name, (_, _, pat, _) in APPS.items():
        subprocess.run(["pkill", "-f", pat])
    time.sleep(3)


# ------------------------------------------------------------------ checks

def problems(st, w):
    """What is wrong with how window `w` is shown (empty: it fills its room)."""
    if not w["mapped"] or w["minimized"] or w["dialog"] or not w["ready"]:
        return []
    if not (w["maximized"] or w["snap"] or w["fullscreen"]):
        # Tablet mode: every app window is maximized, split or fullscreen.
        return ["floating (not maximized, split or fullscreen)"]
    out = []
    x, y, ww, hh = w["rect"]
    rx, ry, rw, rh = w["room"]
    minw, minh = w["min"]
    fixed = w["maxsz"][0] > 0 and w["maxsz"][0] == minw and w["maxsz"][1] == minh
    if fixed:
        return []
    # The room may be smaller than the app's minimum in one axis: it is then
    # bigger than its room there (scaled to fit, or panned over the keyboard).
    fits_w, fits_h = minw <= rw + 2, minh <= rh + 2
    if w["fit"] < 0.999 and fits_w and fits_h:
        out.append(f"scaled {w['fit']:.3f}")
    if fits_w and abs(ww - rw) > 2:
        out.append(f"width {ww} in a {rw} room")
    if fits_h and abs(hh - rh) > 2 and not (st["osk"] > 0 and hh > rh):
        out.append(f"height {hh} in a {rh} room")
    if fits_w and abs(x - rx) > 2:
        out.append(f"x {x} (room at {rx})")
    if fits_h and abs(y - ry) > 2 and not (st["osk"] > 0 and hh > rh):
        out.append(f"y {y} (room at {ry})")
    cw, ch = w["conf"]
    if cw and (fits_w and abs(cw - rw) > 2 or fits_h and abs(ch - rh) > 2):
        out.append(f"told {cw}x{ch} for a {rw}x{rh} room")
    # The frame: a GPU buffer must cover the window from where its geometry
    # starts (src is the part of the buffer the client shows).
    if w["gpu"] and w["src"][0]:
        cs = w["cs"]
        gx = round((w["geo"][0] - w["frameAt"][0]) * cs)
        gy = round((w["geo"][1] - w["frameAt"][1]) * cs)
        if w["src"][0] - gx < ww - 2 or w["src"][1] - gy < hh - 2:
            out.append(f"frame {w['src'][0]}x{w['src'][1]} (from {gx},{gy}) short of {ww}x{hh}")
    if w["crop"][2] > 1.001 or w["crop"][3] > 1.001:
        out.append(f"stretched/old frame crop {w['crop']}")
    if w["awaiting"]:
        out.append("awaiting its new size")
    if w.get("settling"):
        out.append("settling after a turn (hidden)")
    return out


class Report:
    def __init__(self, out):
        self.out = out
        self.rows = []
        self.fails = 0
        self.flicker = 0
        self.record = True
        self.n = 0

    def step(self, label, action, expect=None):
        """Does `action` while the screen is recorded, then settle(); the
        recording is scanned for flicker (recscan.py)."""
        rec = os.path.join(self.out, f"step{self.n + 1:03d}.rec") if self.out and self.record else ""
        if rec:
            ctl(f"record {REC_FRAMES} {rec}")
        action()
        status = self.settle(label, expect)
        if rec:
            for _ in range(60):  # the recording ends after REC_FRAMES presented frames (or none come)
                if os.path.exists(rec):
                    break
                time.sleep(0.05)
            if os.path.exists(rec):
                glitches, _ = recscan.scan(rec)
                if glitches:
                    self.flicker += 1
                    self.rows[-1]["flicker"] = [f"frame {i} +{t:.0f} ms: {w}" for i, t, w in glitches]
                    print(f"    flicker: {self.rows[-1]['flicker']}", flush=True)
                    keep = os.path.join(self.out, f"flicker{self.n:03d}.rec")
                    os.rename(rec, keep)
                    self.rows[-1]["rec"] = keep
                else:
                    os.remove(rec)
            else:
                ctl("record 0 x")  # cancel: nothing was presented
        return status

    def settle(self, label, expect=None, extra_wait=0.0):
        """Polls until every shown window passes (or SETTLE_S); records it.
        `expect(state)`: the step's own effect (the keyboard is up, the window
        fullscreen, ...); a step without it fails as "no effect"."""
        self.n += 1
        t0 = time.time()
        last = None
        good_since = None
        # The step's effects reach jsui-wl through the shell's next task (a
        # keyboard) or the client's next commit: a check right away would pass
        # on the old state. Settled = passing for 150 ms on end, counted from
        # when it first passed.
        time.sleep(0.12)
        while True:
            st = ctl.windows()
            bad = {w["id"]: (w["app"], p) for w in st["windows"] for p in [problems(st, w)] if p}
            if expect and not expect(st):
                bad["step"] = "no effect (yet)"
            now = time.time()
            if not bad and not st["quiet"]:
                if good_since is None:
                    good_since = now
                if now - good_since >= 0.15:
                    ms = (good_since - t0) * 1000
                    status = "ok" if ms <= SLOW_MS else "slow"
                    break
            else:
                good_since = None
            last = bad
            if time.time() - t0 > SETTLE_S:
                ms = SETTLE_S * 1000
                status = "FAIL"
                self.fails += 1
                break
            time.sleep(0.03)
        shot = ""
        if status == "FAIL" and self.out:
            shot = os.path.join(self.out, f"fail{self.n:03d}.ppm")
            ctl(f"shot {shot}")
        row = {"n": self.n, "step": label, "status": status, "ms": round(ms), "screen": st["screen"], "osk": st["osk"],
               "windows": [[w["id"], w["app"], w["rect"], w["room"], w["fit"]] for w in st["windows"] if w["mapped"]],
               "problems": last if status == "FAIL" else None, "shot": shot}
        self.rows.append(row)
        print(f"{self.n:3d} {status:4s} {round(ms):5d} ms  {label}" +
              (f"  {json.dumps(last)}" if status == "FAIL" else ""), flush=True)
        if extra_wait:
            time.sleep(extra_wait)
        return status


# --------------------------------------------------------------- the steps

def turned(deg):
    return lambda st: (st["screen"][0] > st["screen"][1]) == (deg == 90)


def kb(up):
    return lambda st: (st["osk"] > 0) == up


def win(wid, key, value):
    return lambda st: any(w["id"] == wid and w[key] == value for w in st["windows"])


def both(*fs):
    return lambda st: all(f(st) for f in fs)


def rotate(deg):
    ctl(f"rotate {deg}")
    time.sleep(0.15)


def kb_show():
    ctl('post wm:textInput {"show":true}')


def kb_field(wid):
    """The keyboard the way a person gets it: a tap on the app's text field
    (Firefox's address bar), so the app's own text input asks for it (and asks
    again as it resizes), not the host message alone."""
    for w in ctl.windows()["windows"]:
        if w["id"] == wid:
            x, y, ww, hh = w["rect"]
            tap(x + ww * 0.45, y + 112)
            return


def kb_hide():
    ctl('post wm:textInput {"show":false}')


def kb_drag_away():
    a = anchors()
    keys = [v for k, v in a.items() if k.startswith("osk.")]
    if not keys:
        kb_hide()
        return
    top = min(v[1] for v in keys)
    w, h = screen()
    drag(w / 2, top - 30, w / 2, h - 5, 0.25)


def switcher_roundtrip(wid):
    w, h = screen()
    drag(w / 2, h - 2, w / 2, h * 0.55, 0.5)
    time.sleep(1.2)
    a = anchors()
    card = a.get(f"switcher.{wid}")
    if card:
        tap(card[0] + card[2] / 2, card[1] + card[3] / 2)
    else:
        drag(w / 2, h * 0.3, w / 2, h * 0.9, 0.3)  # nothing to pick: swipe it down
    time.sleep(0.2)


def go_home_and_back(wid):
    w, h = screen()
    drag(w / 2, h - 2, w / 2, h * 0.35, 0.3)  # a long fast swipe: home
    time.sleep(1.0)
    ctl(f'wm wm:focus {{"id":{wid}}}')
    # wm:restore of a shown maximized window un-maximizes it: only if it went.
    if any(w["id"] == wid and w["minimized"] for w in ctl.windows()["windows"]):
        ctl(f'wm wm:restore {{"id":{wid}}}')


def divider_drag(frac):
    a = anchors()
    d = a.get("split.divider")
    if not d:
        return False
    w, h = screen()
    cx, cy = d[0] + d[2] / 2, d[1] + d[3] / 2
    if h > w:
        drag(cx, cy, cx, h * frac, 0.5)
    else:
        drag(cx, cy, w * frac, cy, 0.5)
    return True


def run(apps, rep, only):
    def want(name):
        return not only or name in only

    rotate(90)
    w, h = screen()
    drag(w / 2, h * 0.88, w / 2, h * 0.4)  # unlock (no-op when unlocked)
    time.sleep(1.5)
    ids = {}
    for name in apps:
        if not want("single"):
            break
        for deg in (90, 0):
            rotate(deg)
            rep.settle(f"{name}: rotate {deg} before launch", turned(deg))
            wid = launch(name)
            if wid is None:
                rep.rows.append({"step": f"{name}: launch", "status": "FAIL", "problems": "no window"})
                rep.fails += 1
                print(f"    FAIL {name}: no window", flush=True)
                break
            ids[name] = wid
            rep.settle(f"{name}: launched at {deg}")
            other = 0 if deg == 90 else 90
            rep.step(f"{name}: turned to {other}", lambda: rotate(other), turned(other))
            rep.step(f"{name}: turned back to {deg}", lambda: rotate(deg), turned(deg))
            rep.step(f"{name}: keyboard up at {deg}", lambda: kb_show(), kb(True))
            rep.step(f"{name}: keyboard up, turned to {other}", lambda: rotate(other), both(turned(other), kb(True)))
            rep.step(f"{name}: keyboard up, turned back", lambda: rotate(deg), both(turned(deg), kb(True)))
            rep.step(f"{name}: keyboard down", lambda: kb_hide(), kb(False))
            rep.step(f"{name}: keyboard up again", lambda: kb_show(), kb(True))
            rep.step(f"{name}: keyboard dragged away", lambda: kb_drag_away(), kb(False))
            rep.step(f"{name}: switcher and back", lambda: switcher_roundtrip(wid))
            rep.step(f"{name}: home and back", lambda: go_home_and_back(wid))
            rep.step(f"{name}: fullscreen", lambda: ctl(f'wm wm:fullscreen {{"id":{wid},"on":true}}'), win(wid, "fullscreen", True))
            rep.step(f"{name}: fullscreen, turned", lambda: rotate(other), both(turned(other), win(wid, "fullscreen", True)))
            rep.step(f"{name}: fullscreen, turned back", lambda: rotate(deg), both(turned(deg), win(wid, "fullscreen", True)))
            rep.step(f"{name}: fullscreen off", lambda: ctl(f'wm wm:fullscreen {{"id":{wid},"on":false}}'), win(wid, "fullscreen", False))
            ctl(f'wm wm:minimize {{"id":{wid}}}'); time.sleep(0.5)
            rep.step(f"{name}: minimized and restored", lambda: ctl(f'wm wm:restore {{"id":{wid}}}'), win(wid, "minimized", False))
            if name == "ff":
                # Regression (2026-11-09): turned with the keyboard up for a
                # focused field, Firefox was shown small in the middle.
                rep.step(f"{name}: field focused, keyboard up", lambda: kb_field(wid), kb(True))
                rep.step(f"{name}: field focused, turned to {other}", lambda: rotate(other), both(turned(other), kb(True)))
                rep.step(f"{name}: field focused, turned back", lambda: rotate(deg), both(turned(deg), kb(True)))
                rep.step(f"{name}: field focused, keyboard down", lambda: kb_hide(), kb(False))
            # Closed after each pass: four GPU apps at once drove the Tab A7
            # into swap and its windows behind took seconds to follow.
            stop(name)
            ids.pop(name, None)
    if len(apps) < 2 or not want("pairs"):
        return
    for i in range(len(apps)):
        a, b = apps[i], apps[(i + 1) % len(apps)]
        if a == b:
            continue
        for name in list(ids):
            if name not in (a, b):
                stop(name)
                ids.pop(name)
        for name in (a, b):
            if name not in ids:
                rotate(90)
                ids[name] = launch(name)
        for deg in (90, 0):
            rotate(deg)
            rep.step(f"{a}|{b}: side by side at {deg}", lambda: ctl(f'wm wm:split {{"left":{ids[a]},"right":{ids[b]}}}'),
                     both(turned(deg), win(ids[a], "snap", 1), win(ids[b], "snap", 2)))
            if anchors().get("split.divider"):
                for frac in (0.3, 0.7, 0.5):
                    rep.step(f"{a}|{b}: divider to {round(frac * 100)} %", lambda: divider_drag(frac))
            other = 0 if deg == 90 else 90
            rep.step(f"{a}|{b}: side by side, turned to {other}", lambda: rotate(other), both(turned(other), win(ids[a], "snap", 1)))
            rep.step(f"{a}|{b}: side by side, turned back", lambda: rotate(deg), both(turned(deg), win(ids[a], "snap", 1)))
            ctl(f'wm wm:focus {{"id":{ids[b]}}}')
            rep.step(f"{a}|{b}: keyboard up", lambda: kb_show(), kb(True))
            rep.step(f"{a}|{b}: keyboard down", lambda: kb_hide(), kb(False))
            rep.step(f"{a}|{b}: sides swapped", lambda: ctl(f'wm wm:split {{"left":{ids[b]},"right":{ids[a]}}}'), both(win(ids[b], "snap", 1), win(ids[a], "snap", 2)))
            rep.step(f"{a}|{b}: switcher and back", lambda: switcher_roundtrip(ids[a]))
            rep.step(f"{a}|{b}: {a} maximized out of split", lambda: ctl(f'wm wm:maximize {{"id":{ids[a]},"on":true}}'), win(ids[a], "maximized", True))
            rep.step(f"{a}|{b}: {b} maximized", lambda: ctl(f'wm wm:maximize {{"id":{ids[b]},"on":true}}'), win(ids[b], "maximized", True))
            # Regression (2026-11-09): split with the keyboard up (portrait:
            # one on top), the window was tiny with a gap above the keyboard.
            rep.step(f"{a}|{b}: keyboard up, maximized", lambda: kb_show(), kb(True))
            rep.step(f"{a}|{b}: keyboard up, side by side", lambda: ctl(f'wm wm:split {{"left":{ids[a]},"right":{ids[b]}}}'),
                     both(kb(True), win(ids[a], "snap", 1), win(ids[b], "snap", 2)))
            rep.step(f"{a}|{b}: keyboard up in split, turned", lambda: rotate(0 if deg == 90 else 90), kb(True))
            rep.step(f"{a}|{b}: keyboard up in split, turned back", lambda: rotate(deg), kb(True))
            rep.step(f"{a}|{b}: keyboard down in split", lambda: kb_hide(), kb(False))
            ctl(f'wm wm:maximize {{"id":{ids[a]},"on":true}}')
            ctl(f'wm wm:maximize {{"id":{ids[b]},"on":true}}')


def main():
    global ctl
    ap = argparse.ArgumentParser()
    ap.add_argument("--apps", default="ff,claude,foot,calc")
    ap.add_argument("--out", default=os.path.join(HOME, "caps", "matrix"))
    ap.add_argument("--only", default="")
    ap.add_argument("--no-record", action="store_true", help="skip the flicker recordings")
    o = ap.parse_args()
    os.makedirs(o.out, exist_ok=True)
    ctl = Ctl()
    rep = Report(o.out)
    rep.record = not o.no_record
    apps = [a for a in o.apps.split(",") if a in APPS]
    try:
        run(apps, rep, set(filter(None, o.only.split(","))))
    finally:
        close_all()
        kb_hide()
        rotate(90)
    with open(os.path.join(o.out, "report.json"), "w") as f:
        json.dump(rep.rows, f, indent=1)
    slow = sum(1 for r in rep.rows if r.get("status") == "slow")
    print(f"{rep.n} checks: {rep.fails} failed, {slow} slow (> {SLOW_MS} ms), {rep.flicker} with flicker")
    sys.exit(min(rep.fails + rep.flicker, 100))


if __name__ == "__main__":
    main()
