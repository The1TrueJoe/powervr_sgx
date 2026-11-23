"""hwt suite for hwt_open.py: the operands of smp's gradient LOD mode
(ISA.md "The texture-sample sequence": h8-9 = 3, src2 the first of the
derivative registers; how many and in which order unknown).

An 8x8 RGBA8888 twiddled texture with its mip chain (TEX.md "Mipmaps": level
n+1 follows level n, 64 + 16 + 4 + 1 dwords), every dword i of the heap
0x5a5a0000 | i ("pat"), so the texel read names its level: index 0-63 level 0,
64-79 level 1, 80-83 level 2, 84 level 3. T0 0x07fa0000: WORD0's defaults
with no_mipmaps clear (nearest, nearest between levels, repeat); T1 RGBA8888
8x8; T3 the U8 return. Coordinate (0.5, 0.5) in r8-r9: texel (4, 4) of level
0, (2, 2), (1, 1), (0, 0) of the next ones.

Controls: a plain dependent sample (constant coordinate: no gradient, level
0) and the LOD mode at 0..3 (each level's own texel). Then the gradient mode
with r12-r15 (r16-r17 zero, in case more are read):
  grad_zero        all 0: level 0
  grad_one_k       r(12+k) = 0.5 alone (8 x 0.5 = 4 texels a pixel: LOD 2)
  grad_pair_j_k    r(12+j) = r(12+k) = 0.287 (8 x 0.287 = 2.3: LOD 1.2 if
                   apart, 2.3 x sqrt 2 = 3.25: LOD 1.7 if one vector's)
"""
from hwt_lib import END

LEVEL = {}
for i in range(85):
    LEVEL[0x5a5a0000 | i] = 0 if i < 64 else 1 if i < 80 else 2 if i < 84 else 3

STATE = ["limm r4, #0x07fa0000  [skipinv]",
         "limm r5, #0x0d007007  [skipinv]",
         "limm r6, TEX  [skipinv]",
         "limm r7, #0x00800800  [skipinv]",
         "limm r8, #0x3f000000  [skipinv]",
         "limm r9, #0x3f000000  [skipinv]",
         "limm r0, #0x5a5a5a5a  [skipinv]"]


def f32(x):
    import struct
    return struct.unpack("<I", struct.pack("<f", x))[0]


def grads(vals):
    return [f"limm r{12 + k}, #{f32(v):#010x}  [skipinv]" for k, v in enumerate(vals)]


TESTS, TEXS = [], {}


def add(name, lines):
    TEXS[name] = "pat"
    TESTS.append((name, STATE + lines + ["wdf", f"mov o0, r0  {END}"], None))


add("plain", ["smp.2d r0, r8, r4  [skipinv]"])
for lod in range(4):
    add(f"lod_{lod}", [f"limm r12, #{f32(float(lod)):#010x}  [skipinv]",
                       "smp.2d.lod r0, r8, r4, r12  [skipinv]"])
add("grad_zero", grads([0, 0, 0, 0, 0, 0]) + ["smp.2d.grad r0, r8, r4, r12  [skipinv]"])
for k in range(6):
    v = [0.0] * 6
    v[k] = 0.5
    add(f"grad_one_{k}", grads(v) + ["smp.2d.grad r0, r8, r4, r12  [skipinv]"])
for j in range(4):
    for k in range(j + 1, 4):
        v = [0.0] * 6
        v[j] = v[k] = 0.287
        add(f"grad_pair_{j}_{k}", grads(v) + ["smp.2d.grad r0, r8, r4, r12  [skipinv]"])
