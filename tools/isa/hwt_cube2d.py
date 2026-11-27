"""hwt suite for hwt_open.py: which coordinate components smp.2d reads when
the texture state is a cube map's (T1 layout 2), against smp.3d.

hwt_cubegrad found, with the cube's state, that smp.2d and smp.2d.grad pick
the face from the direction on +X and -Y, but on +Z (direction (0, 0, 1))
read texel 0 of a level and, plain, level 3 where the others read level 0.
The hypothesis: a 2D sample fetches two coordinate components, so z is 0 and
the hardware sees (x, y, 0).  On +Z that is (0, 0, 0): no major axis
(ma = 0), so s and t are 0 / 0 and the implicit LOD's scale 1 / ma is
infinite.

Same cube as hwt_cubegrad: 8x8 RGBA8888 with its chain on every face, the
"pat" heap (dword i = 0x5a5a0000 | i, i = 85 face + texel), T0 0x07fa0000,
T1 0x4d007007.  A texel's index comes back in the middle bytes of o0
(0x5a<lo>.. : b2 | b1 << 8).  Face centres (level 0, Morton): 48.

  dirs: (0, 0, 1)        +Z centre: 3d -> 4*85 + 48
        (0, 0, -1)       -Z centre: 3d -> 5*85 + 48; 2d the same as (0,0,1)
                         if z isn't read
        (0.25, 0, 1)     +Z, right of centre with 3d; with z dropped,
                         (0.25, 0, 0) is +X's centre
        (1, 0.5, -0.25)  +X off centre: 3d reads z (-0.25: s 0.625), 2d with
                         z = 0 reads s 0.5 (hwt_cubegrad's c2_off: texel 36)
  r10 (z) garbage on smp.2d: 7.0 and -3.0 in place of the direction's z
  and smp.2d with the direction's z moved to r9 (y) and y to r10: if z is
  dropped, the face follows the swapped register
"""
from hwt_lib import END


def f32(x):
    import struct
    return struct.unpack("<I", struct.pack("<f", x))[0]


def state(d):
    return ["limm r4, #0x07fa0000  [skipinv]",
            "limm r5, #0x4d007007  [skipinv]",
            "limm r6, TEX  [skipinv]",
            "limm r7, #0x00800800  [skipinv]",
            f"limm r8, #{f32(d[0]):#010x}  [skipinv]",
            f"limm r9, #{f32(d[1]):#010x}  [skipinv]",
            f"limm r10, #{f32(d[2]):#010x}  [skipinv]",
            "limm r0, #0x5a5a5a5a  [skipinv]"]


TESTS, TEXS = [], {}


def add(name, d, lines):
    TEXS[name] = "pat"
    TESTS.append((name, state(d) + lines + ["wdf", f"mov o0, r0  {END}"], None))


DIRS = (("pz", (0.0, 0.0, 1.0)), ("nz", (0.0, 0.0, -1.0)),
        ("pz_x", (0.25, 0.0, 1.0)), ("off", (1.0, 0.5, -0.25)),
        ("px", (1.0, 0.0, 0.0)), ("ny", (0.0, -1.0, 0.0)))

for dname, d in DIRS:
    add(f"{dname}_3d", d, ["smp.3d r0, r8, r4  [skipinv]"])
    add(f"{dname}_2d", d, ["smp.2d r0, r8, r4  [skipinv]"])
    add(f"{dname}_3d_lod0", d, ["limm r12, #0x00000000  [skipinv]",
                                "smp.3d.lod r0, r8, r4, r12  [skipinv]"])
    add(f"{dname}_2d_lod0", d, ["limm r12, #0x00000000  [skipinv]",
                                "smp.2d.lod r0, r8, r4, r12  [skipinv]"])

# r10 garbage under smp.2d: unchanged if z isn't read
for dname, d in (("px", (1.0, 0.0, 0.0)), ("pz", (0.0, 0.0, 1.0))):
    for g in (7.0, -3.0):
        add(f"{dname}_2d_r10_{g}", (d[0], d[1], g),
            ["smp.2d r0, r8, r4  [skipinv]"])
        add(f"{dname}_3d_r10_{g}", (d[0], d[1], g),
            ["smp.3d r0, r8, r4  [skipinv]"])

# the +Z direction with y and z swapped in the registers: (0, 1, 0) is +Y's
# centre for both forms if the 2D one reads r8, r9
add("swap_yz_2d", (0.0, 1.0, 0.0), ["smp.2d r0, r8, r4  [skipinv]"])
add("swap_yz_3d", (0.0, 1.0, 0.0), ["smp.3d r0, r8, r4  [skipinv]"])
