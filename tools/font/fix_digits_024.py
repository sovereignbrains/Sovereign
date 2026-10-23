"""Правка цифры 0 в переменной Jura (поверх fix_digit_gaps.py).

0: без точки внутри, уже, кольцо-суперэллипс со скруглёнными углами.
2 и 4 остаются как в Jura (пробы своей «2» с петелькой и рукописной «4» отвергнуты, они в истории git).

Глиф строится в двух мастерах (wght 300 и 700) с одинаковой топологией, поэтому ложится в gvar как дефолт + дельты.
Использование: python -I fix_digits_024.py in.ttf out.ttf
"""
import math
import sys
from array import array

from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import ttProgram
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphCoordinates
from fontTools.ttLib.tables.TupleVariation import TupleVariation

H300, H700 = 42.0, 84.0  # половина толщины штриха I на 300 и 700


def set_glyph(font, name, polys300, polys700, adv300, adv700, lsb):
    glyf, gvar = font["glyf"], font["gvar"]
    pts300 = [p for poly in polys300 for p in poly]
    pts700 = [p for poly in polys700 for p in poly]
    assert len(pts300) == len(pts700)
    ends, c = [], -1
    for poly in polys300:
        c += len(poly)
        ends.append(c)
    xmin = min(p[0] for p in pts300)
    shift = lsb - xmin
    pts300 = [(round(x + shift), round(y)) for x, y in pts300]
    xmin700 = min(p[0] for p in pts700)
    pts700 = [(round(x + lsb - xmin700), round(y)) for x, y in pts700]
    g = Glyph()
    g.numberOfContours = len(ends)
    g.coordinates = GlyphCoordinates(pts300)
    g.endPtsOfContours = ends
    g.flags = array("B", [1] * len(pts300))
    g.program = ttProgram.Program()
    g.program.fromBytecode(b"")
    glyf[name] = g
    g.recalcBounds(glyf)
    deltas = [(b[0] - a[0], b[1] - a[1]) for a, b in zip(pts300, pts700)]
    deltas += [(0, 0), (adv700 - adv300, 0), (0, 0), (0, 0)]
    gvar.variations[name] = [TupleVariation({"wght": (0.0, 1.0, 1.0)}, deltas)]
    font["hmtx"][name] = (adv300, g.xMin)


def build_zero(xl, xr, h, cy=639.0, ytop=1287.0, nexp=2.6):
    """Кольцо из двух контуров: суперэллипс с закруглёнными углами, внешний по часовой, внутренний против."""
    a = (xr - xl) / 2
    b = ytop / 2 - h
    cx = (xl + xr) / 2
    M = 72
    ring = []
    for k in range(M):
        t = 2 * math.pi * k / M
        c, s = math.cos(t), math.sin(t)
        ring.append((cx + a * math.copysign(abs(c) ** (2 / nexp), c), cy + b * math.copysign(abs(s) ** (2 / nexp), s)))
    outer, inner = [], []
    for i in range(M):
        p0, p1 = ring[i - 1], ring[(i + 1) % M]
        dx, dy = p1[0] - p0[0], p1[1] - p0[1]
        l = math.hypot(dx, dy)
        nx, ny = dy / l, -dx / l  # наружу при обходе против часовой
        outer.append((ring[i][0] + h * nx, ring[i][1] + h * ny))
        inner.append((ring[i][0] - h * nx, ring[i][1] - h * ny))
    return [outer[::-1], inner]  # внешний по часовой, внутренний против


def main():
    src, dst = sys.argv[1], sys.argv[2]
    f = TTFont(src)
    cm = f.getBestCmap()
    lsb, a3, a7 = 120, 1096, 1228
    p3 = build_zero(lsb + H300, a3 - lsb - H300, H300)
    p7 = build_zero(lsb + H700, a7 - lsb - H700, H700)
    set_glyph(f, cm[ord("0")], p3, p7, a3, a7, lsb)
    f.save(dst)
    print("ok")


if __name__ == "__main__":
    main()
