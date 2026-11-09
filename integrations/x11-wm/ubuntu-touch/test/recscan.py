#!/usr/bin/env python3
"""Finds flicker in a jsui-wl frame recording (control socket `record N PATH`:
"JSUIREC w h n" then per frame a double time in ms and w*h RGBA).

A glitch is a frame unlike both of its neighbours while they are like each
other: a one-frame excursion that comes back (a window drawn at the wrong
scale or place, a blank or black frame), which a slide or a resize that
moves steadily never is. Black: a frame with clearly more pure black than
the frames on either side (an unpainted buffer shown).

    recscan.py FILE.rec [--sheet OUT.png]   (prints glitches; exit status = their count)
"""
import struct
import sys


def frames(path):
    with open(path, "rb") as f:
        hdr = f.readline().split()
        w, h, n = int(hdr[1]), int(hdr[2]), int(hdr[3])
        out = []
        for _ in range(n):
            raw = f.read(8)
            if len(raw) < 8:
                break
            t = struct.unpack("d", raw)[0]
            px = f.read(w * h * 4)
            if len(px) < w * h * 4:
                break
            out.append((t, px))
    return w, h, out


def small(px, w, h, step=4):
    """Grey levels on a coarse grid, and the share of pure black pixels."""
    g = []
    black = 0
    n = 0
    for y in range(0, h, step):
        row = y * w * 4
        for x in range(0, w, step):
            i = row + x * 4
            r, gg, b = px[i], px[i + 1], px[i + 2]
            g.append((r * 3 + gg * 6 + b) // 10)
            if r < 10 and gg < 10 and b < 10:
                black += 1
            n += 1
    return g, black / max(1, n)


def diff(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / max(1, len(a))


def scan(path):
    w, h, fr = frames(path)
    sm = [small(px, w, h) for _, px in fr]
    found = []
    i = 1
    while i < len(sm) - 1:
        # Frames i..j (up to 3) unlike frame i-1 and frame j+1, which are
        # like each other: an excursion that comes back.
        hit = None
        for j in range(i, min(i + 3, len(sm) - 1)):
            a = diff(sm[i - 1][0], sm[i][0])
            b = diff(sm[j][0], sm[j + 1][0])
            c = diff(sm[i - 1][0], sm[j + 1][0])
            inner = max(diff(sm[i - 1][0], sm[k][0]) for k in range(i, j + 1))
            if a > 6 and b > 6 and inner > 6 and c < 0.5 * min(a, b):
                hit = (j, a, b, c)
                break
        if hit:
            j, a, b, c = hit
            found.append((i, fr[i][0] - fr[0][0], f"{j - i + 1}-frame excursion (diff {a:.0f}/{b:.0f}, around it {c:.0f})"))
            i = j + 1
            continue
        i += 1
    for i in range(1, len(sm) - 1):
        bl = sm[i][1]
        if bl > 0.02 and bl > 2 * max(sm[i - 1][1], sm[i + 1][1]) + 0.01:
            found.append((i, fr[i][0] - fr[0][0], f"black {bl * 100:.0f}% of the screen"))
    return found, len(fr)


if __name__ == "__main__":
    found, n = scan(sys.argv[1])
    for i, t, what in found:
        print(f"frame {i} (+{t:.0f} ms): {what}")
    print(f"{n} frames, {len(found)} glitches")
    sys.exit(min(len(found), 100))
