"""Шире зазор у концов крючков в а, б, е, ё, у и длиннее перемычка в ю (переменная Jura).

Как fix_digit_gaps.py: сдвиг пропорционален толщине штриха, чтобы держался на всех весах.
Использование: python -I fix_letters.py in.ttf out.ttf
"""
import sys

from fontTools.ttLib import TTFont
from fontTools.varLib.iup import iup_delta

STEM300, STEM700 = 84, 168
# буква: (точки, k по y) — точки по контуру на wght 300
SPEC = {
    "а": (range(28, 34), 0.5),    # конец верхнего крючка выше, дальше от чаши
    "б": (range(8, 24), 0.5),     # флажок выше над чашей
    "е": (range(25, 32), -0.4),   # нижний конец ниже, дальше от перекладины
    "ё": (range(49, 56), -0.4),
    "у": (range(3, 12), -0.5),    # конец хвоста ниже, дальше от чаши
}
YU_SHIFT = 120  # ю: овал вправо, перемычка длиннее


def load(f, ch):
    glyf, gvar = f["glyf"], f["gvar"]
    name = f.getBestCmap()[ord(ch)]
    g = glyf[name]
    coords, ends, _ = g.getCoordinates(glyf)
    tvs = gvar.variations[name]
    assert len(tvs) == 1
    tv = tvs[0]
    orig = [tuple(c) for c in coords] + [(0, 0)] * 4
    full = [tuple(d) for d in iup_delta(tv.coordinates, orig, list(ends))]
    return name, g, tv, full


src, dst = sys.argv[1], sys.argv[2]
f = TTFont(src)
glyf = f["glyf"]
for ch, (idx, k) in SPEC.items():
    name, g, tv, full = load(f, ch)
    shift300, extra = round(k * STEM300), round(k * (STEM700 - STEM300))
    for i in idx:
        x, y = g.coordinates[i]
        g.coordinates[i] = (x, y + shift300)
        dx, dy = full[i]
        full[i] = (dx, dy + extra)
    tv.coordinates = full
    g.recalcBounds(glyf)

name, g, tv, full = load(f, "ю")
for i in list(range(11, 31)) + list(range(34, 52)):  # внешний контур овала и внутренний
    x, y = g.coordinates[i]
    g.coordinates[i] = (x + YU_SHIFT, y)
tv.coordinates = full
g.recalcBounds(glyf)
adv, lsb = f["hmtx"][name]
f["hmtx"][name] = (adv + YU_SHIFT, lsb)
f.save(dst)
print("ok")
