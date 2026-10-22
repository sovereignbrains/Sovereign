"""Правка цифр 0, 2, 4 в переменной Jura (поверх fix_digit_gaps.py).

0: убрать точку внутри, сузить (стороны сдвигаются, толщина штриха сохраняется).
2: нижняя ножка волной вместо прямой.
4: рукописная, как «ч»: левая стойка с загибом в перекладину и отдельная правая стойка.

Глифы 2 и 4 строятся обводкой осевой линии с круглыми концами в двух мастерах (wght 300 и 700)
с одинаковой топологией, поэтому ложатся в gvar как дефолт + дельты.
Использование: python -I fix_digits_024.py in.ttf out.ttf
"""
import math
import sys
from array import array

from fontTools.pens.boundsPen import BoundsPen  # noqa: F401
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import ttProgram
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphCoordinates
from fontTools.ttLib.tables.TupleVariation import TupleVariation
from fontTools.varLib.iup import iup_delta

CAP = 1266
H300, H700 = 42.0, 84.0  # половина толщины штриха I на 300 и 700
N = 18  # точек на сегмент Безье
NCAP = 10


def bez(p0, p1, p2, p3, n=N):
    out = []
    for i in range(n):
        t = i / n
        a, b, c, d = (1 - t) ** 3, 3 * t * (1 - t) ** 2, 3 * t * t * (1 - t), t ** 3
        out.append((a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0],
                    a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1]))
    return out


def path(segs, last):
    pts = []
    for s in segs:
        pts += bez(*s)
    pts.append(last)
    return pts


def stroke(pts, h):
    """Контур обводки ломаной pts шириной 2h с круглыми концами; по часовой стрелке."""
    n = len(pts)
    nor = []
    for i in range(n):
        a = pts[max(i - 1, 0)]
        b = pts[min(i + 1, n - 1)]
        dx, dy = b[0] - a[0], b[1] - a[1]
        l = math.hypot(dx, dy) or 1
        nor.append((-dy / l, dx / l))
    left = [(p[0] + h * q[0], p[1] + h * q[1]) for p, q in zip(pts, nor)]
    right = [(p[0] - h * q[0], p[1] - h * q[1]) for p, q in zip(pts, nor)]

    def cap(c, q, sign):
        base = math.atan2(q[1], q[0])
        return [(c[0] + h * math.cos(base + sign * math.pi * k / (NCAP + 1)),
                 c[1] + h * math.sin(base + sign * math.pi * k / (NCAP + 1)))
                for k in range(1, NCAP + 1)]

    poly = left + cap(pts[-1], nor[-1], -1) + right[::-1] + cap(pts[0], (-nor[0][0], -nor[0][1]), -1)
    area = sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1]
               for i in range(len(poly)))
    return poly[::-1] if area > 0 else poly  # по часовой: площадь < 0


def build_two(xl, xr, h):
    X = lambda u: xl + u * (xr - xl)
    T = CAP + 17 - h  # центр верхней дуги (выступ как у круглых глифов)
    B = h
    segs = [
        ((X(0.03), 950), (X(-0.03), 1150), (X(0.24), T), (X(0.52), T)),
        ((X(0.52), T), (X(0.88), T), (X(1.0), 1150), (X(1.0), 930)),
        ((X(1.0), 930), (X(1.0), 640), (X(0.36), 400), (X(0.10), 215)),
        ((X(0.10), 215), (X(-0.03), 120), (X(0.02), B + 5), (X(0.22), B + 5)),
        ((X(0.22), B + 5), (X(0.42), B + 5), (X(0.40), B + 150), (X(0.62), B + 150)),
        ((X(0.62), B + 150), (X(0.84), B + 150), (X(0.82), B + 5), (X(1.0), B + 40)),
    ]
    return [stroke(path(segs, (X(1.0), B + 40)), h)]


def build_four(xl, xr, h):
    X = lambda u: xl + u * (xr - xl)
    T = CAP - h
    yb = 350
    right = [(X(0.85), T), (X(0.85), T - 1), (X(0.85), h + 1), (X(0.85), h)]
    right = [(X(0.85), T - i * (T - h) / 40) for i in range(41)]
    segs = [((X(0.03), T), (X(0.03), T - 300), (X(0.0), yb + 260), (X(0.02), yb + 120))]
    segs += [((X(0.02), yb + 120), (X(0.04), yb), (X(0.2), yb), (X(0.34), yb))]
    left = path(segs, (X(0.34), yb))
    left += [(X(0.34) + i * (X(1.0) - X(0.34)) / 12, yb) for i in range(1, 13)]
    return [stroke(left, h), stroke(right, h)]


def build_zero(g, glyf, full_default, full_master):
    """Убрать контур точки и сузить: возвращает координаты (300, 700) и концы контуров."""
    raise NotImplementedError


def contours_of(coords, ends):
    res, s = [], 0
    for e in ends:
        res.append(list(range(s, e + 1)))
        s = e + 1
    return res


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


def fix_zero(font):
    glyf, gvar = font["glyf"], font["gvar"]
    name = font.getBestCmap()[ord("0")]
    g = glyf[name]
    coords, ends, _ = g.getCoordinates(glyf)
    tv = gvar.variations[name][0]
    orig = [tuple(c) for c in coords] + [(0, 0)] * 4
    full = [tuple(d) for d in iup_delta(tv.coordinates, orig, list(ends))]
    c300 = [tuple(c) for c in coords]
    c700 = [(c[0] + d[0], c[1] + d[1]) for c, d in zip(c300, full)]
    adv300, _ = font["hmtx"][name]
    adv700 = adv300 + full[-3][0]
    cont = contours_of(c300, list(ends))
    areas = []
    for ix in cont:
        xs = [c300[i][0] for i in ix]
        ys = [c300[i][1] for i in ix]
        areas.append((max(xs) - min(xs)) * (max(ys) - min(ys)))
    dot = areas.index(min(areas))
    keep = [ix for k, ix in enumerate(cont) if k != dot]
    outer, inner = sorted(keep, key=lambda ix: -(max(c300[i][0] for i in ix) - min(c300[i][0] for i in ix)))

    def narrow(cs, d):
        out = {}
        xs = [cs[i][0] for i in outer]
        cx = (min(xs) + max(xs)) / 2
        for ix in (outer, inner):
            half = (max(cs[i][0] for i in ix) - min(cs[i][0] for i in ix)) / 2
            s = (half - d) / half
            for i in ix:
                out[i] = (cx + (cs[i][0] - cx) * s, cs[i][1])
        return out

    n300 = narrow(c300, 38)
    n700 = narrow(c700, 70)
    polys300 = [[n300[i] for i in ix] for ix in (outer, inner)]
    polys700 = [[n700[i] for i in ix] for ix in (outer, inner)]
    lsb = min(c300[i][0] for i in outer)
    xmin = min(p[0] for poly in polys300 for p in poly)
    set_glyph(font, name, polys300, polys700, adv300 - 76, adv700 - 140, round(lsb))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    f = TTFont(src)
    cm = f.getBestCmap()
    fix_zero(f)
    # 2: lsb 119, adv 1128/1150; 4: lsb 60, adv 1036/1246 (значения исходной Jura)
    for ch, build, lsb, a3, a7 in (("2", build_two, 119, 1128, 1150), ("4", build_four, 60, 1036, 1246)):
        p3 = build(lsb + H300, a3 - lsb - H300, H300)
        p7 = build(lsb + H700, a7 - lsb - H700, H700)
        set_glyph(f, cm[ord(ch)], p3, p7, a3, a7, lsb)
    f.save(dst)
    print("ok")


def spline(pts, n=N):
    """Catmull-Rom через опорные точки -> плотная ломаная."""
    P = [pts[0]] + list(pts) + [pts[-1]]
    out = []
    for i in range(1, len(P) - 2):
        p0, p1, p2, p3 = P[i - 1], P[i], P[i + 1], P[i + 2]
        c1 = (p1[0] + (p2[0] - p0[0]) / 6, p1[1] + (p2[1] - p0[1]) / 6)
        c2 = (p2[0] - (p3[0] - p1[0]) / 6, p2[1] - (p3[1] - p1[1]) / 6)
        out += bez(p1, c1, c2, p2, n)
    out.append(pts[-1])
    return out


def build_two(xl, xr, h):  # noqa: F811  по наброску пользователя: петелька внизу слева, волна, хвост вверх
    X = lambda u: xl + u * (xr - xl)
    T = CAP + 17 - h
    Y = lambda v: h + v * (T - h)
    raw = [(0.10, .83), (0.20, .95), (0.42, 1.0), (0.58, .88), (0.65, .72), (0.62, .48), (0.50, .33),
           (0.30, .19), (0.10, .06), (0.02, .0), (0.0, .10), (0.06, .21), (0.20, .22), (0.34, .12),
           (0.45, .0), (0.56, .0), (0.78, .13), (1.0, .25)]
    return [stroke(spline([(X(u), Y(v)) for u, v in raw]), h)]


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


def main():  # noqa: F811
    src, dst = sys.argv[1], sys.argv[2]
    f = TTFont(src)
    cm = f.getBestCmap()
    for ch, build, lsb, a3, a7 in (("0", build_zero, 120, 1096, 1228),
                                   ("4", build_four, 60, 1036, 1246)):  # 2 оставлена как в Jura
        p3 = build(lsb + H300, a3 - lsb - H300, H300)
        p7 = build(lsb + H700, a7 - lsb - H700, H700)
        set_glyph(f, cm[ord(ch)], p3, p7, a3, a7, lsb)
    f.save(dst)
    print("ok")


if __name__ == "__main__":
    main()


