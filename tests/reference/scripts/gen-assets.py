#!/usr/bin/env python3
"""Generates the original art used by examples/ (icons, portraits, backdrops).

Everything is procedural (PIL + numpy) so the repository ships no third-party
art. Output is deterministic: rerunning produces identical files.

    python3 tests/reference/scripts/gen-assets.py
"""
import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
OUT = os.path.join(ROOT, "examples", "assets")
SS = 4  # supersampling factor for anti-aliased shapes

rng = np.random.default_rng(1234)


def hexc(h, a=255):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(len(a)))


def save(img, name, **kw):
    path = os.path.join(OUT, name)
    if name.endswith(".jpg"):
        img.convert("RGB").save(path, quality=88, optimize=True, progressive=False)
    else:
        img.save(path, optimize=True, **kw)
    print("wrote", os.path.relpath(path, ROOT), img.size)


def vgrad(w, h, stops):
    """Vertical gradient; stops = [(t, (r,g,b,a)), ...]."""
    t = np.linspace(0, 1, h)[:, None]
    out = np.zeros((h, w, 4), np.float32)
    for c in range(4):
        xs = [s[0] for s in stops]
        ys = [s[1][c] for s in stops]
        out[:, :, c] = np.interp(t, xs, ys)
    return out


def to_img(arr):
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGBA")


def value_noise(w, h, scale, octaves=5, seed=0):
    r = np.random.default_rng(seed)
    total = np.zeros((h, w), np.float32)
    amp, norm = 1.0, 0.0
    for o in range(octaves):
        gw, gh = int(w / scale * 2**o) + 2, int(h / scale * 2**o) + 2
        grid = r.random((gh, gw)).astype(np.float32)
        layer = np.array(Image.fromarray((grid * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC), np.float32) / 255
        total += layer * amp
        norm += amp
        amp *= 0.5
    return total / norm


# ---------------------------------------------------------------- icons
class Canvas:
    """Supersampled RGBA drawing surface, downsampled on finish()."""

    def __init__(self, size):
        self.size = size
        self.img = Image.new("RGBA", (size * SS, size * SS), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.img)

    def s(self, pts):
        return [(x * SS, y * SS) for x, y in pts]

    def poly(self, pts, fill, outline=None, width=0):
        self.d.polygon(self.s(pts), fill=fill, outline=outline, width=int(width * SS))

    def ellipse(self, box, fill, outline=None, width=0):
        x0, y0, x1, y1 = box
        self.d.ellipse([x0 * SS, y0 * SS, x1 * SS, y1 * SS], fill=fill, outline=outline, width=int(width * SS))

    def rrect(self, box, r, fill, outline=None, width=0):
        x0, y0, x1, y1 = box
        self.d.rounded_rectangle([x0 * SS, y0 * SS, x1 * SS, y1 * SS], r * SS, fill=fill, outline=outline, width=int(width * SS))

    def line(self, pts, fill, width):
        self.d.line(self.s(pts), fill=fill, width=int(width * SS), joint="curve")

    def glow(self, color, radius, strength=1.0):
        """Adds a soft outer glow around everything drawn so far."""
        alpha = self.img.split()[3]
        blur = alpha.filter(ImageFilter.GaussianBlur(radius * SS))
        blur = blur.point(lambda v: min(255, int(v * strength)))
        g = Image.new("RGBA", self.img.size, color[:3] + (0,))
        g.putalpha(blur)
        self.img = Image.alpha_composite(g, self.img)
        self.d = ImageDraw.Draw(self.img)

    def finish(self):
        return self.img.resize((self.size, self.size), Image.LANCZOS)


def shade(c, f):
    return tuple(max(0, min(255, int(v * f))) for v in c[:3]) + (c[3] if len(c) > 3 else 255,)


def icon_sword():
    c = Canvas(128)
    steel, edge = hexc("#cfd8e3"), hexc("#f4f8fc")
    # blade (diagonal, tip top-right)
    c.poly([(98, 14), (114, 14), (114, 30), (52, 92), (36, 76)], steel)
    c.poly([(98, 14), (114, 14), (44, 84), (36, 76)], edge)
    c.line([(110, 18), (46, 82)], shade(steel, 0.75), 1.5)
    # guard
    c.poly([(26, 70), (34, 62), (66, 94), (58, 102)], hexc("#d9a441"))
    c.poly([(28, 70), (34, 64), (40, 70), (34, 76)], hexc("#f3cf72"))
    # grip + pommel
    c.poly([(40, 86), (48, 94), (28, 114), (20, 106)], hexc("#5b3a24"))
    for i in range(4):
        t = 0.2 + i * 0.2
        x, y = 44 - 20 * t, 90 + 20 * t
        c.line([(x - 4, y - 4), (x + 4, y + 4)], hexc("#3b2416"), 1.5)
    c.ellipse((12, 104, 28, 120), hexc("#d9a441"))
    c.ellipse((16, 107, 22, 113), hexc("#f6dd93"))
    c.glow(hexc("#9fd4ff"), 3, 0.6)
    return c.finish()


def icon_shield():
    c = Canvas(128)
    outline = [(64, 10), (108, 24), (104, 70), (64, 118), (24, 70), (20, 24)]
    c.poly(outline, hexc("#c08a2e"))
    inner = [(64, 18), (100, 30), (96, 68), (64, 108), (32, 68), (28, 30)]
    c.poly(inner, hexc("#2b4f8f"))
    c.poly([(64, 18), (100, 30), (96, 68), (64, 108)], hexc("#22407a"))
    # cross emblem
    c.poly([(58, 32), (70, 32), (70, 52), (88, 52), (88, 62), (70, 62), (70, 92), (58, 92), (58, 62), (40, 62), (40, 52), (58, 52)], hexc("#f0c75e"))
    c.poly([(58, 32), (64, 32), (64, 92), (58, 92)], hexc("#ffe39a"))
    for x, y in [(64, 14), (102, 27), (26, 27)]:
        c.ellipse((x - 3, y - 3, x + 3, y + 3), hexc("#ffe7a8"))
    return c.finish()


def icon_potion(liquid, glow):
    c = Canvas(128)
    # flask body
    c.ellipse((26, 46, 102, 122), hexc("#cfe3f0", 90), outline=hexc("#e8f4ff", 200), width=3)
    # liquid (lower part of circle)
    liq = Image.new("RGBA", c.img.size, (0, 0, 0, 0))
    ld = ImageDraw.Draw(liq)
    ld.ellipse([30 * SS, 50 * SS, 98 * SS, 118 * SS], fill=liquid)
    mask = Image.new("L", c.img.size, 0)
    ImageDraw.Draw(mask).rectangle([0, 72 * SS, 128 * SS, 128 * SS], fill=255)
    liq.putalpha(Image.composite(liq.split()[3], Image.new("L", c.img.size, 0), mask))
    c.img = Image.alpha_composite(c.img, liq)
    c.d = ImageDraw.Draw(c.img)
    c.ellipse((34, 70, 94, 78), shade(liquid, 1.35))
    # neck + cork
    c.rrect((52, 22, 76, 50), 4, hexc("#cfe3f0", 110), outline=hexc("#e8f4ff", 200), width=3)
    c.rrect((50, 10, 78, 26), 5, hexc("#8a5a35"))
    c.rrect((52, 12, 76, 16), 2, hexc("#b07c4f"))
    # highlights + bubbles
    c.ellipse((40, 60, 52, 84), (255, 255, 255, 120))
    for x, y, r in [(70, 92, 4), (58, 102, 3), (80, 84, 2.5)]:
        c.ellipse((x - r, y - r, x + r, y + r), (255, 255, 255, 150))
    c.glow(glow, 5, 0.9)
    return c.finish()


def icon_gem():
    c = Canvas(128)
    top = [(34, 40), (94, 40), (114, 58), (64, 118), (14, 58)]
    c.poly(top, hexc("#7b3fe4"))
    c.poly([(34, 40), (94, 40), (82, 58), (46, 58)], hexc("#c39bff"))
    c.poly([(14, 58), (34, 40), (46, 58)], hexc("#a777ff"))
    c.poly([(114, 58), (94, 40), (82, 58)], hexc("#9459f2"))
    c.poly([(14, 58), (46, 58), (64, 118)], hexc("#6a2fd0"))
    c.poly([(46, 58), (82, 58), (64, 118)], hexc("#8c4ff0"))
    c.poly([(82, 58), (114, 58), (64, 118)], hexc("#5420b0"))
    c.poly([(44, 44), (60, 44), (54, 54), (42, 54)], (255, 255, 255, 170))
    c.glow(hexc("#b37cff"), 6, 1.0)
    return c.finish()


def icon_bow():
    c = Canvas(128)
    # limb as thick arc
    c.d.arc([20 * SS, 10 * SS, 100 * SS, 118 * SS], start=-70, end=70, fill=hexc("#7a4a26"), width=10 * SS)
    c.d.arc([22 * SS, 12 * SS, 98 * SS, 116 * SS], start=-66, end=66, fill=hexc("#a8693a"), width=4 * SS)
    # string
    top = (60 + 40 * math.cos(math.radians(-70)), 64 + 54 * math.sin(math.radians(-70)))
    bot = (60 + 40 * math.cos(math.radians(70)), 64 + 54 * math.sin(math.radians(70)))
    c.line([top, (40, 64), bot], hexc("#e9e2cf"), 1.6)
    # arrow
    c.line([(18, 64), (108, 64)], hexc("#c9a46a"), 3)
    c.poly([(104, 56), (122, 64), (104, 72)], hexc("#d3dbe4"))
    c.poly([(14, 56), (26, 64), (14, 72), (20, 64)], hexc("#3fb6a8"))
    c.rrect((56, 58, 68, 70), 3, hexc("#d9a441"))
    return c.finish()


def icon_helmet():
    c = Canvas(128)
    c.d.chord([22 * SS, 18 * SS, 106 * SS, 110 * SS], 180, 360, fill=hexc("#8d99a8"))
    c.rrect((22, 60, 106, 104), 10, hexc("#8d99a8"))
    c.d.chord([28 * SS, 24 * SS, 100 * SS, 100 * SS], 200, 300, fill=hexc("#b8c3cf"))
    # visor slit
    c.rrect((34, 62, 94, 72), 4, hexc("#1b2230"))
    c.rrect((60, 72, 68, 100), 3, hexc("#1b2230"))
    # crest
    c.poly([(58, 6), (70, 6), (72, 40), (56, 40)], hexc("#c0392b"))
    c.poly([(58, 6), (64, 6), (64, 40), (56, 40)], hexc("#e0574a"))
    for x in (30, 98):
        c.ellipse((x - 4, 84, x + 4, 92), hexc("#d9a441"))
    return c.finish()


def icon_ring():
    c = Canvas(128)
    c.ellipse((22, 40, 106, 120), hexc("#d9a441"))
    c.ellipse((34, 52, 94, 108), (0, 0, 0, 0))
    # punch the hole
    mask = Image.new("L", c.img.size, 255)
    ImageDraw.Draw(mask).ellipse([36 * SS, 54 * SS, 92 * SS, 106 * SS], fill=0)
    c.img.putalpha(Image.composite(c.img.split()[3], Image.new("L", c.img.size, 0), mask))
    c.d = ImageDraw.Draw(c.img)
    c.d.arc([24 * SS, 42 * SS, 104 * SS, 118 * SS], 200, 300, fill=hexc("#ffe39a"), width=4 * SS)
    # gem setting
    c.poly([(48, 30), (80, 30), (90, 44), (64, 62), (38, 44)], hexc("#1fbf8f"))
    c.poly([(48, 30), (80, 30), (72, 40), (56, 40)], hexc("#7ff0c8"))
    c.glow(hexc("#3fe0b0"), 4, 0.7)
    return c.finish()


def icon_scroll():
    c = Canvas(128)
    c.rrect((26, 24, 102, 104), 6, hexc("#e8d6ae"))
    for i, y in enumerate(range(40, 92, 10)):
        w = 56 if i % 2 == 0 else 44
        c.rrect((36, y, 36 + w, y + 3), 1.5, hexc("#9a7b4f"))
    for y in (18, 98):
        c.rrect((18, y, 110, y + 12), 6, hexc("#c9a46a"))
        c.rrect((18, y + 2, 110, y + 5), 2, hexc("#e6c88f"))
    c.ellipse((74, 78, 98, 102), hexc("#b8322a"))
    c.ellipse((80, 84, 92, 96), hexc("#d9534a"))
    return c.finish()


def icon_boots():
    c = Canvas(128)
    c.poly([(30, 14), (62, 14), (64, 80), (104, 92), (108, 112), (26, 112)], hexc("#6b4428"))
    c.poly([(30, 14), (46, 14), (44, 112), (26, 112)], hexc("#845634"))
    c.rrect((24, 104, 110, 116), 4, hexc("#3a2414"))
    c.rrect((28, 26, 64, 34), 2, hexc("#d9a441"))
    c.rrect((28, 52, 64, 60), 2, hexc("#d9a441"))
    return c.finish()


def icon_amulet():
    c = Canvas(128)
    c.d.arc([30 * SS, 4 * SS, 98 * SS, 76 * SS], 200, 340, fill=hexc("#d9a441"), width=3 * SS)
    c.poly([(64, 46), (92, 74), (64, 118), (36, 74)], hexc("#d9a441"))
    c.poly([(64, 54), (84, 75), (64, 106), (44, 75)], hexc("#d23b5a"))
    c.poly([(64, 54), (84, 75), (64, 78), (44, 75)], hexc("#ff7a96"))
    c.glow(hexc("#ff5a7a"), 5, 0.8)
    return c.finish()


def icon_axe():
    c = Canvas(128)
    c.line([(30, 112), (88, 26)], hexc("#6b4428"), 9)
    c.line([(30, 112), (88, 26)], hexc("#8a5a35"), 4)
    c.d.pieslice([56 * SS, 6 * SS, 124 * SS, 74 * SS], 290, 70 + 360, fill=hexc("#aab6c4"))
    c.poly([(70, 24), (92, 40), (82, 54), (64, 40)], hexc("#7d8896"))
    c.d.arc([58 * SS, 8 * SS, 122 * SS, 72 * SS], 300, 400, fill=hexc("#eef3f8"), width=3 * SS)
    return c.finish()


def buff_icon(bg_top, bg_bot, draw_fn):
    size = 64
    c = Canvas(size)
    # radial-ish disk background
    for i in range(24):
        t = i / 23
        col = lerp(bg_top, bg_bot, t)
        inset = t * 4
        c.rrect((2 + inset * 0.2, 2 + inset * 0.2, size - 2 - inset * 0.2, size - 2 - inset * 0.2), 12, None)
    grad = to_img(vgrad(size * SS, size * SS, [(0, bg_top), (1, bg_bot)]))
    mask = Image.new("L", c.img.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, size * SS - 1, size * SS - 1], 14 * SS, fill=255)
    c.img.paste(grad, (0, 0), mask)
    c.d = ImageDraw.Draw(c.img)
    draw_fn(c)
    return c.finish()


def flame(c):
    c.poly([(32, 8), (46, 28), (48, 44), (40, 56), (24, 56), (16, 44), (18, 30), (26, 34)], hexc("#ffb03a"))
    c.poly([(32, 24), (40, 38), (40, 48), (34, 54), (28, 54), (24, 46), (26, 38)], hexc("#fff0a8"))


def snowflake(c):
    for a in range(0, 180, 60):
        r = math.radians(a)
        dx, dy = 22 * math.cos(r), 22 * math.sin(r)
        c.line([(32 - dx, 32 - dy), (32 + dx, 32 + dy)], hexc("#e9f7ff"), 3.5)
        for s in (-1, 1):
            bx, by = 32 + s * dx * 0.6, 32 + s * dy * 0.6
            for b in (-1, 1):
                rb = r + b * math.radians(45) + (math.pi if s < 0 else 0)
                c.line([(bx, by), (bx + 7 * math.cos(rb), by + 7 * math.sin(rb))], hexc("#e9f7ff"), 2.5)


def bolt(c):
    c.poly([(36, 6), (16, 36), (30, 36), (24, 58), (48, 26), (34, 26), (42, 6)], hexc("#fff3a0"))


def ward(c):
    c.poly([(32, 8), (52, 16), (50, 36), (32, 56), (14, 36), (12, 16)], hexc("#d8f6ff"))
    c.poly([(32, 14), (46, 20), (44, 34), (32, 48)], hexc("#9fe0ff"))


def skull(c):
    c.ellipse((14, 10, 50, 44), hexc("#d9ffd0"))
    c.rrect((22, 36, 42, 54), 4, hexc("#d9ffd0"))
    c.ellipse((20, 22, 30, 32), hexc("#1d5a2a"))
    c.ellipse((34, 22, 44, 32), hexc("#1d5a2a"))
    for x in (26, 32, 38):
        c.line([(x, 44), (x, 52)], hexc("#1d5a2a"), 2)


def heart(c):
    c.ellipse((12, 14, 34, 36), hexc("#ff8aa0"))
    c.ellipse((30, 14, 52, 36), hexc("#ff8aa0"))
    c.poly([(13, 29), (51, 29), (32, 54)], hexc("#ff8aa0"))
    c.ellipse((17, 18, 25, 26), (255, 255, 255, 170))


# ---------------------------------------------------------------- images
def portrait(name, seed, palette):
    """Stylised hooded character bust (256x256)."""
    w = h = 256
    bg_top, bg_bot, cloak, cloak_hi, skin, eye = palette
    base = vgrad(w, h, [(0, bg_top), (1, bg_bot)])
    # radial light behind the head
    yy, xx = np.mgrid[0:h, 0:w]
    d = np.sqrt((xx - 128) ** 2 + (yy - 100) ** 2) / 150
    glow = np.clip(1 - d, 0, 1)[:, :, None] ** 2
    base[:, :, :3] += glow * np.array(eye[:3], np.float32) * 0.45
    img = to_img(base)
    c = Canvas(w)
    # shoulders / cloak
    c.d.chord([10 * SS, 170 * SS, 246 * SS, 380 * SS], 180, 360, fill=cloak)
    c.poly([(70, 200), (128, 150), (186, 200), (160, 256), (96, 256)], shade(cloak, 0.8))
    # hood
    c.ellipse((62, 40, 194, 190), cloak)
    c.ellipse((70, 46, 186, 180), cloak_hi)
    # face in shadow
    c.ellipse((88, 76, 168, 178), skin)
    c.poly([(80, 92), (128, 60), (176, 92), (170, 120), (86, 120)], shade(cloak, 0.55))
    # eyes
    for ex in (110, 146):
        c.ellipse((ex - 9, 120, ex + 9, 130), eye)
    # clasp
    c.ellipse((116, 196, 140, 220), hexc("#d9a441"))
    c.ellipse((122, 202, 134, 214), hexc("#fff0b0"))
    layer = c.finish()
    # eye glow
    alpha = layer.split()[3]
    img = Image.alpha_composite(img, layer)
    eyes = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ed = ImageDraw.Draw(eyes)
    for ex in (110, 146):
        ed.ellipse([ex - 14, 114, ex + 14, 136], fill=eye[:3] + (160,))
    eyes = eyes.filter(ImageFilter.GaussianBlur(6))
    img = Image.alpha_composite(img, eyes)
    # vignette
    v = np.clip(np.sqrt((xx - 128) ** 2 + (yy - 128) ** 2) / 181, 0, 1) ** 2.2
    arr = np.array(img, np.float32)
    arr[:, :, :3] *= (1 - 0.55 * v)[:, :, None]
    del alpha
    save(to_img(arr), name)


def mountains_backdrop(name, w, h, sky, layers, sun, stars=True, seed=0):
    arr = vgrad(w, h, sky)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    sx, sy, sr, scol = sun
    d = np.sqrt((xx - sx) ** 2 + (yy - sy) ** 2)
    halo = np.clip(1 - d / (sr * 6), 0, 1) ** 2.5
    arr[:, :, :3] += halo[:, :, None] * np.array(scol[:3], np.float32) * 0.8
    disk = np.clip((sr - d) / 1.5, 0, 1)
    arr[:, :, :3] = arr[:, :, :3] * (1 - disk[:, :, None]) + disk[:, :, None] * np.array(lerp(scol, (255, 255, 255, 255), 0.6)[:3], np.float32)
    if stars:
        r = np.random.default_rng(seed + 7)
        n = int(w * h / 900)
        px = r.integers(0, w, n)
        py = (r.random(n) ** 1.8 * h * 0.6).astype(int)
        b = r.random(n)
        for x, y, v in zip(px, py, b):
            arr[y, x, :3] = np.maximum(arr[y, x, :3], 140 + 115 * v)
    img = to_img(arr)
    for i, (base_y, amp, col, haze) in enumerate(layers):
        n = value_noise(w, 1, w / 3.0, octaves=6, seed=seed + i * 13)[0]
        ridge = base_y - amp * (n - 0.5) * 2
        layer = np.zeros((h, w, 4), np.float32)
        mask = yy >= ridge[None, :]
        shade_t = np.clip((yy - ridge[None, :]) / (h - ridge[None, :] + 1), 0, 1)
        for c in range(3):
            layer[:, :, c] = col[c] * (1 - 0.35 * shade_t)
        layer[:, :, 3] = mask * 255
        img = Image.alpha_composite(img, to_img(layer))
    # bottom fog
    fog = vgrad(w, h, [(0, (0, 0, 0, 0)), (0.75, (0, 0, 0, 0)), (1, layers[-1][2][:3] + (200,))])
    img = Image.alpha_composite(img, to_img(fog))
    save(img, name)


def minimap(name, size=256):
    n = value_noise(size, size, 90, octaves=6, seed=42)
    yy, xx = np.mgrid[0:size, 0:size]
    # island falloff
    d = np.sqrt((xx - size / 2) ** 2 + (yy - size / 2) ** 2) / (size / 2)
    hgt = n - 0.45 * d**2
    arr = np.zeros((size, size, 4), np.float32)
    deep, shallow = np.array([18, 44, 74]), np.array([38, 92, 120])
    sand, grass, forest, rock = np.array([196, 178, 120]), np.array([88, 132, 70]), np.array([46, 90, 52]), np.array([120, 120, 124])
    cols = np.zeros((size, size, 3), np.float32)
    h = hgt
    cols[:] = deep
    m = h > 0.18
    cols[m] = shallow
    cols[h > 0.25] = sand
    cols[h > 0.28] = grass
    cols[h > 0.42] = forest
    cols[h > 0.55] = rock
    # contour shading
    shade_ = (np.gradient(h, axis=0) - np.gradient(h, axis=1)) * 600
    cols += np.clip(shade_, -25, 25)[:, :, None]
    arr[:, :, :3] = cols
    arr[:, :, 3] = 255
    img = to_img(arr)
    d2 = ImageDraw.Draw(img)
    # roads
    d2.line([(30, 200), (90, 160), (128, 128), (170, 110), (226, 60)], fill=(214, 196, 150, 255), width=3, joint="curve")
    d2.line([(128, 128), (140, 190), (120, 236)], fill=(214, 196, 150, 255), width=2, joint="curve")
    # grid (drawn on an overlay: ImageDraw does not blend onto RGBA)
    grid = Image.new("RGBA", img.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(grid)
    for i in range(0, size, 32):
        gd.line([(i, 0), (i, size)], fill=(255, 255, 255, 22))
        gd.line([(0, i), (size, i)], fill=(255, 255, 255, 22))
    save(Image.alpha_composite(img, grid), name)


def crest(name, c1, c2, shape):
    c = Canvas(64)
    grad = to_img(vgrad(64 * SS, 64 * SS, [(0, c1), (1, c2)]))
    mask = Image.new("L", c.img.size, 0)
    ImageDraw.Draw(mask).ellipse([0, 0, 64 * SS - 1, 64 * SS - 1], fill=255)
    c.img.paste(grad, (0, 0), mask)
    c.d = ImageDraw.Draw(c.img)
    white = (255, 255, 255, 230)
    if shape == "wolf":
        c.poly([(16, 20), (24, 30), (40, 30), (48, 20), (46, 38), (32, 50), (18, 38)], white)
    elif shape == "star":
        pts = []
        for i in range(10):
            r = 20 if i % 2 == 0 else 8.5
            a = math.radians(-90 + i * 36)
            pts.append((32 + r * math.cos(a), 33 + r * math.sin(a)))
        c.poly(pts, white)
    elif shape == "moon":
        c.ellipse((14, 14, 50, 50), white)
        c.ellipse((22, 10, 56, 44), c1)
    elif shape == "tower":
        c.rrect((22, 22, 42, 50), 2, white)
        for x in (20, 28, 36):
            c.rrect((x, 14, x + 8, 24), 1, white)
    elif shape == "flame":
        c.poly([(32, 12), (44, 30), (44, 42), (36, 52), (28, 52), (20, 42), (22, 30), (28, 34)], white)
    elif shape == "eye":
        c.ellipse((12, 22, 52, 42), white)
        c.ellipse((26, 26, 38, 38), c2)
    return c.finish()


def card_art(name, w, h, sky, accent, seed):
    """Small banner art for store/battle-pass cards."""
    arr = vgrad(w, h, sky)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.sqrt((xx - w * 0.7) ** 2 + (yy - h * 0.35) ** 2)
    halo = np.clip(1 - d / (h * 0.9), 0, 1) ** 2
    arr[:, :, :3] += halo[:, :, None] * np.array(accent[:3], np.float32) * 0.9
    n = value_noise(w, h, 40, octaves=4, seed=seed)
    arr[:, :, :3] *= (0.85 + 0.3 * n)[:, :, None]
    img = to_img(arr)
    # angular light rays
    rays = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    rd = ImageDraw.Draw(rays)
    cx, cy = w * 0.7, h * 0.35
    for i in range(9):
        a = math.radians(i * 40 + seed * 7)
        rd.polygon([(cx, cy), (cx + 2 * w * math.cos(a - 0.08), cy + 2 * w * math.sin(a - 0.08)), (cx + 2 * w * math.cos(a + 0.08), cy + 2 * w * math.sin(a + 0.08))], fill=accent[:3] + (26,))
    img = Image.alpha_composite(img, rays.filter(ImageFilter.GaussianBlur(2)))
    save(img, name)


def main():
    os.makedirs(OUT, exist_ok=True)
    icons = {
        "sword": icon_sword,
        "shield": icon_shield,
        "potion-health": lambda: icon_potion(hexc("#e0344f"), hexc("#ff5a72")),
        "potion-mana": lambda: icon_potion(hexc("#2f7bf0"), hexc("#5aa8ff")),
        "gem": icon_gem,
        "bow": icon_bow,
        "helmet": icon_helmet,
        "ring": icon_ring,
        "scroll": icon_scroll,
        "boots": icon_boots,
        "amulet": icon_amulet,
        "axe": icon_axe,
    }
    for name, fn in icons.items():
        save(fn(), f"item-{name}.png")
    buffs = {
        "fire": (hexc("#ff7a2f"), hexc("#a2210c"), flame),
        "frost": (hexc("#5ec8ff"), hexc("#1b4d9e"), snowflake),
        "haste": (hexc("#ffd84a"), hexc("#b8730a"), bolt),
        "ward": (hexc("#7fd8ff"), hexc("#2b5d8a"), ward),
        "poison": (hexc("#6fe06a"), hexc("#1f6b2a"), skull),
        "regen": (hexc("#ff7d9a"), hexc("#9c1f45"), heart),
    }
    for name, (a, b, fn) in buffs.items():
        save(buff_icon(a, b, fn), f"buff-{name}.png")

    portrait("portrait-mage.png", 1, (hexc("#23305e"), hexc("#0b1024"), hexc("#3b2f6b"), hexc("#4a3c85"), hexc("#d6a98a"), hexc("#7fe3ff")))
    portrait("portrait-rogue.png", 2, (hexc("#5a2a1e"), hexc("#140906"), hexc("#2b2b33"), hexc("#3a3a46"), hexc("#c9926e"), hexc("#ffb347")))

    mountains_backdrop(
        "bg-menu.jpg", 1280, 720,
        [(0, hexc("#070b1f")), (0.55, hexc("#1d2457")), (0.8, hexc("#5b3a78")), (1, hexc("#c2667a"))],
        [(500, 120, hexc("#2e2556"), None), (575, 100, hexc("#1b1838"), None), (650, 70, hexc("#0e0d22"), None)],
        (930, 250, 34, hexc("#ffd6a0")), seed=3,
    )
    mountains_backdrop(
        "bg-loading.jpg", 1280, 720,
        [(0, hexc("#0a1a24")), (0.5, hexc("#12404a")), (0.85, hexc("#d38a4a")), (1, hexc("#f2c27a"))],
        [(470, 110, hexc("#173640"), None), (560, 90, hexc("#0f252c"), None), (640, 70, hexc("#081418"), None)],
        (360, 430, 46, hexc("#ffcf8a")), stars=True, seed=11,
    )
    minimap("minimap.png")
    for name, c1, c2, shape in [
        ("crest-wolf", hexc("#4f7cff"), hexc("#1d2f80"), "wolf"),
        ("crest-star", hexc("#ffb547"), hexc("#a8540d"), "star"),
        ("crest-moon", hexc("#9f6bff"), hexc("#3d1d8a"), "moon"),
        ("crest-tower", hexc("#40c4a0"), hexc("#11604d"), "tower"),
        ("crest-flame", hexc("#ff5d6c"), hexc("#8a1426"), "flame"),
        ("crest-eye", hexc("#e8e8f0"), hexc("#55607a"), "eye"),
    ]:
        save(crest(name, c1, c2, shape), f"{name}.png")
    card_art("card-ember.png", 320, 180, [(0, hexc("#3a0d12")), (1, hexc("#120406"))], hexc("#ff7a3a"), 1)
    card_art("card-tide.png", 320, 180, [(0, hexc("#06263a")), (1, hexc("#020c14"))], hexc("#3ad0ff"), 2)
    card_art("card-verdant.png", 320, 180, [(0, hexc("#0e2e16")), (1, hexc("#041006"))], hexc("#7dff7a"), 3)
    card_art("card-void.png", 320, 180, [(0, hexc("#1d0c3a")), (1, hexc("#070312"))], hexc("#b77cff"), 4)


if __name__ == "__main__":
    main()
