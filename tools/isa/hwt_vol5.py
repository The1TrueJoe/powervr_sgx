"""hwt suite for hwt_open.py: NPOT volume mip levels and flat levels (TEX.md 14).

hwt_vol4 found an NPOT volume addressed as the next power of two. Here the
level after an NPOT one: where it starts says how big the one before was
(padded from its own size, or the padded base halved). And a level one slice
deep (32x32x2's level 1), which the tile rule (side min(w, h, d, 16)) makes
plain rows.
"""
from hwt_vol4 import T0, T3, add, at, t1

TESTS = []
TEXS = {}

T0M = T0 & ~(1 << 18)


def level(w, n):
    return max(w >> n, 1)


for w, h, d in ((12, 4, 6), (5, 7, 3), (6, 6, 6), (32, 32, 2), (24, 40, 20)):
    for lod in range(0, 6):
        lw, lh, ld = level(w, lod), level(h, lod), level(d, lod)
        pts = [(0, 0, 0), (1 % lw, 0, 0), (0, 1 % lh, 0), (0, 0, 1 % ld), (lw - 1, lh - 1, ld - 1)]
        if lw > 8:
            pts.append((lw - 1, 0, 0))
        if lh > 8:
            pts.append((0, lh - 1, 0))
        for x, y, z in dict.fromkeys(pts):
            add(f"nm_{w}x{h}x{d}_l{lod}_{x}_{y}_{z}", T0M, t1(1, w, h), T3 | (d - 1),
                *at(lw, lh, ld, x, y, z), lod=float(lod), tests=TESTS, texs=TEXS)
        if max(lw, lh, ld) == 1:
            break
