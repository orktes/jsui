"""Builds the small test fonts in this directory (run from the repo root):

  pip install fonttools brotli
  python3 tests/native/fonts/make_fonts.py

  inter-abc.{ttf,woff,woff2}  Inter Regular subset to "ABC" (container decoding)
  inter-abc.ttc               Inter Regular + Bold subsets as one collection
  jsui-var.ttf                variable font, 'wght' 100..900: "I" advance 200 / 500 / 900 units at wght 100 / 400 / 900,
                              U+F0000 advance 300 (glyph fallback)
  jsui-pua.ttf                static font with only U+F0000, advance 900
"""
import os
from fontTools import subset
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables.TupleVariation import TupleVariation
from fontTools.ttLib.ttCollection import TTCollection

OUT = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(OUT, "..", "..", "..", "assets", "fonts")


def inter_subset(name):
    opts = subset.Options()
    opts.layout_features = []
    opts.name_IDs = ["*"]
    f = TTFont(os.path.join(FONTS, name))
    s = subset.Subsetter(opts)
    s.populate(text="ABC")
    s.subset(f)
    return f


reg = inter_subset("Inter-Regular.ttf")
reg.save(os.path.join(OUT, "inter-abc.ttf"))
for flavor in ("woff", "woff2"):
    f = TTFont(os.path.join(OUT, "inter-abc.ttf"))
    f.flavor = flavor
    f.save(os.path.join(OUT, "inter-abc." + flavor))
ttc = TTCollection()
ttc.fonts = [TTFont(os.path.join(OUT, "inter-abc.ttf")), inter_subset("Inter-Bold.ttf")]
ttc.save(os.path.join(OUT, "inter-abc.ttc"))


def rect(w, h=700):
    pen = TTGlyphPen(None)
    pen.moveTo((0, 0))
    pen.lineTo((0, h))
    pen.lineTo((w, h))
    pen.lineTo((w, 0))
    pen.closePath()
    return pen.glyph()


def build(family, glyphs, cmap, advances):
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(glyphs)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf({g: rect(max(advances[g] - 20, 10)) for g in glyphs})
    fb.setupHorizontalMetrics({g: (advances[g], 0) for g in glyphs})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": family, "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200, usWeightClass=400)
    fb.setupPost()
    return fb


# Variable: "I" (and its advance) 200 units at wght 100, 500 at 400, 900 at 900.
glyphs = [".notdef", "I", "pua"]
adv = {".notdef": 500, "I": 500, "pua": 300}
fb = build("JsuiVar", glyphs, {ord("I"): "I", 0xF0000: "pua"}, adv)
fb.setupFvar([("wght", 100, 400, 900, "Weight")], [])
# glyf points of "I": 4 corners + 4 phantom points (advance is phantom point 2).
def deltas(dx):
    return [(0, 0), (0, 0), (dx, 0), (dx, 0), (0, 0), (dx, 0), (0, 0), (0, 0)]
fb.setupGvar({
    "I": [TupleVariation({"wght": (0, 1.0, 1.0)}, deltas(400)), TupleVariation({"wght": (-1.0, -1.0, 0)}, deltas(-300))],
    ".notdef": [], "pua": [],
})
fb.save(os.path.join(OUT, "jsui-var.ttf"))

fb = build("JsuiPua", [".notdef", "pua"], {0xF0000: "pua"}, {".notdef": 500, "pua": 900})
fb.save(os.path.join(OUT, "jsui-pua.ttf"))
