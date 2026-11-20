"""hwt suite for hwt_open.py: volume textures past 4x4xD (TEX.md 14).

The heap is sgx_hwt's `texidx` one: dword i = i, so a texel names its index.
RGBA8888 volumes, nearest, one dependent smp.3d at a texel centre per test.

  map_WxHxD_*   layout 1 (the twiddled volume), T3 low bits = D - 1: one
                coordinate bit at a time (x = 2^k, y = 0, z = 0, ...), then a
                few mixed points. A power-of-two volume's order is a bit
                permutation: the single-bit indices give it and the mixed
                ones check it (hwt_vol4_fit.py).
  lin<L>_*      layouts 3 (linear) and 6 (strided) with a depth: whether
                either reads a stack of linear slices (NPOT volumes).
  npot_*        layout 1 at 3x5x3 and 6x6x6.
  wrapr<V>_*    T0 b0-2 = V at r outside [0, 1] on 4x4x4: the wrap R field.
  mip_*         a 8x8x8 / 4x4x16 / 16x4x4 chain read with smp.3d.lod.
  lin_*         a linear filter across r (mag_linear) half way between two
                slices.

`python3 tools/isa/hwt_vol4_fit.py <hwt_open output>` fits the orders.
"""
import random
import struct

from hwt_lib import END

TESTS = []
TEXS = {}

T0 = 0x07FE0000  # nearest, repeat, no mipmaps (hwt_smp's 2D word)
T3 = 0x00800800
HEAP_DWORDS = 1 << 22  # sgx_hwt's texidx heap


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def t1(layout, w, h):
    return layout << 29 | 0x0D << 24 | (w - 1) << 12 | (h - 1)


def add(name, t0, t1w, t3, s, t, r, lod=None, tests=TESTS, texs=TEXS):
    texs[name] = "idx"
    smp = "smp.3d r0, r8, r4  [skipinv]" if lod is None else \
        "smp.3d.lod r0, r8, r4, r11  [skipinv]"
    pre = [] if lod is None else [f"limm r11, #{f32(lod):#010x}  [skipinv]"]
    tests.append((name, [
        f"limm r4, #{t0:#010x}  [skipinv]",
        f"limm r5, #{t1w:#010x}  [skipinv]",
        "limm r6, TEX  [skipinv]",
        f"limm r7, #{t3:#010x}  [skipinv]",
        f"limm r8, #{f32(s):#010x}  [skipinv]",
        f"limm r9, #{f32(t):#010x}  [skipinv]",
        f"limm r10, #{f32(r):#010x}  [skipinv]",
        *pre,
        "limm r0, #0x5a5a5a5a  [skipinv]",
        smp,
        "wdf",
        f"mov o0, r0  {END}"], None))


def at(w, h, d, x, y, z):
    return (x + 0.5) / w, (y + 0.5) / h, (z + 0.5) / d


SIZES = [(8, 8, 8), (16, 16, 16), (32, 32, 32), (64, 64, 64), (128, 128, 128),
         (64, 8, 2), (2, 8, 64), (8, 2, 64), (16, 64, 4), (32, 16, 8),
         (4, 4, 128), (2, 2, 256), (256, 2, 2), (2, 256, 2), (1, 1, 64),
         (64, 1, 1), (128, 32, 4)]

rnd = random.Random(14)
for w, h, d in SIZES:
    assert w * h * d <= HEAP_DWORDS
    tag = f"map_{w}x{h}x{d}"
    pts = [(0, 0, 0)]
    for n, axis in ((w, 0), (h, 1), (d, 2)):
        k = 1
        while k < n:
            p = [0, 0, 0]
            p[axis] = k
            pts.append(tuple(p))
            k <<= 1
    pts += [(w - 1, h - 1, d - 1)]
    pts += [(rnd.randrange(w), rnd.randrange(h), rnd.randrange(d)) for _ in range(4)]
    for x, y, z in pts:
        add(f"{tag}_{x}_{y}_{z}", T0, t1(1, w, h), T3 | (d - 1), *at(w, h, d, x, y, z))

# linear / strided layouts with a depth: 8x4x4 (pitch 8 either way), 5x3x3
for layout in (3, 6):
    for w, h, d in ((8, 4, 4), (5, 3, 3)):
        for x, y, z in ((1, 0, 0), (0, 1, 0), (0, 0, 1), (0, 0, 2), (w - 1, h - 1, d - 1)):
            add(f"lin{layout}_{w}x{h}x{d}_{x}_{y}_{z}", T0, t1(layout, w, h), T3 | (d - 1),
                *at(w, h, d, x, y, z))

# NPOT on the twiddled volume layout
for w, h, d in ((3, 5, 3), (6, 6, 6)):
    for x, y, z in ((1, 0, 0), (2, 0, 0), (0, 1, 0), (0, 2, 0), (0, 4 % h, 0), (0, 0, 1),
                    (0, 0, 2), (w - 1, h - 1, d - 1)):
        add(f"npot_{w}x{h}x{d}_{x}_{y}_{z}", T0, t1(1, w, h), T3 | (d - 1), *at(w, h, d, x, y, z))

# wrap R: r at slice 5.5 and -0.5 of a 4-deep volume, T0 b0-2 = 0, 1, 2
for v in range(3):
    for k, r in (("hi", 5.5 / 4), ("lo", -0.5 / 4)):
        add(f"wrapr{v}_{k}", T0 | v, t1(1, 4, 4), T3 | 3, 0.125, 0.125, r)

# mip chains: levels 1-3 at a few texels, nearest mip (T0 b18 clear)
T0M = T0 & ~(1 << 18)
for w, h, d in ((8, 8, 8), (4, 4, 16), (16, 4, 4)):
    for lod in (1, 2, 3, 4):
        lw, lh, ld = max(w >> lod, 1), max(h >> lod, 1), max(d >> lod, 1)
        if max(w, h, d) >> lod < 1:
            continue
        for x, y, z in ((0, 0, 0), (1 % lw, 0, 0), (0, 1 % lh, 0), (0, 0, 1 % ld)):
            add(f"mip_{w}x{h}x{d}_l{lod}_{x}_{y}_{z}", T0M, t1(1, w, h), T3 | (d - 1),
                *at(lw, lh, ld, x, y, z), lod=float(lod))

# a linear filter across r, half way between slices 1 and 2 of 4x4x4
TL = T0 | 1 << 12 | 1 << 10
add("lin_r", TL, t1(1, 4, 4), T3 | 3, 0.125, 0.125, 0.5)
add("lin_s", TL, t1(1, 4, 4), T3 | 3, 0.25, 0.125, 0.125)
