"""Jura: шире зазор у крючков цифр 5, 6, 9. Остальные глифы не трогаются.

Запуск: python -I fix_digit_gaps.py <Jura.ttf> <выход.ttf>
Сдвиг пропорционален толщине штриха (84 на wght 300, 168 на wght 700), поэтому
правятся и дефолтные координаты, и дельты gvar.
"""
import sys
from fontTools.ttLib import TTFont
from fontTools.varLib.iup import iup_delta

STEM300, STEM700 = 84, 168
# цифра: (индексы точек крючка, доля толщины штриха; + вверх, - вниз)
SPEC = {
    "5": (range(3, 9), -0.5),    # хвост снизу слева уходит вниз, раскрывает чашку
    "6": (range(11, 17), 0.6),   # крючок вверху справа поднимается от чашки
    "9": (range(11, 17), -0.6),  # крючок внизу слева опускается от чашки
}

src, dst = sys.argv[1], sys.argv[2]
f = TTFont(src)
glyf, gvar, cmap = f["glyf"], f["gvar"], f.getBestCmap()
for ch, (idx, k) in SPEC.items():
    name = cmap[ord(ch)]
    g = glyf[name]
    coords, ends, _ = g.getCoordinates(glyf)
    n = len(coords)
    tvs = gvar.variations[name]
    assert len(tvs) == 1, (name, len(tvs))
    tv = tvs[0]
    orig = [tuple(c) for c in coords] + [(0, 0)] * 4
    full = iup_delta(tv.coordinates, orig, list(ends))
    full = [tuple(d) for d in full]
    shift300, extra = round(k * STEM300), round(k * (STEM700 - STEM300))
    for i in idx:
        x, y = g.coordinates[i]
        g.coordinates[i] = (x, y + shift300)
        dx, dy = full[i]
        full[i] = (dx, dy + extra)
    tv.coordinates = full
    g.recalcBounds(glyf)
    print(name, "ok, shift at 300:", shift300, "at 700:", shift300 + extra)
f.save(dst)
