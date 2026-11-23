"""hwt suite for hwt_open.py: smp's gradient LOD mode on a cube map
(h8-9 = 3 with smp.3d): how many derivative registers it reads from src2,
in which order, and in which space (the direction's, or the face's s, t).

An 8x8 RGBA8888 cube with its mip chain on every face (TEX.md "Cube maps":
faces in GL order, each a whole chain, packed below a 16x16 tile: 85 dwords
a face), every dword i of the heap 0x5a5a0000 | i ("pat"), so the texel read
names its face and its level: i = 85 f + (0-63 level 0, 64-79 level 1,
80-83 level 2, 84 level 3). T0 0x07fa0000 (mipmaps on, nearest, repeat),
T1 0x4d007007 (layout 2, cube; RGBA8888 8x8), T3 the U8 return.

The direction is (1, 0, 0), the +X face's centre (ma = 1, sc = -rz,
tc = -ry): a face coordinate moves by half the direction's minor component,
so a derivative of 0.5 on y or z is 2 texels a pixel of level 0 (LOD 1) if
the hardware projects the gradients onto the face, and 4 (LOD 2) if it
takes them as s, t ones. The same on -Y (direction (0, -1, 0)).

Controls: a plain dependent sample and the LOD mode at 0..3 (each level's
own texel). Then the gradient mode with r12-r19:
  grad_zero        all 0: level 0
  grad_one_k       r(12+k) = 0.5 alone
  grad_quarter_k   r(12+k) = 0.25 alone (LOD 0 or 1: which space)
"""
from hwt_lib import END


def face_level(i):
    f, j = divmod(i, 85)
    return f, 0 if j < 64 else 1 if j < 80 else 2 if j < 84 else 3


LEVEL = {}
for i in range(6 * 85):
    LEVEL[0x5a5a0000 | i] = "f%d L%d" % face_level(i)


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


def grads(vals):
    return [f"limm r{12 + k}, #{f32(v):#010x}  [skipinv]" for k, v in enumerate(vals)]


TESTS, TEXS = [], {}


def add(name, d, lines):
    TEXS[name] = "pat"
    TESTS.append((name, state(d) + lines + ["wdf", f"mov o0, r0  {END}"], None))


for dname, d in (("px", (1.0, 0.0, 0.0)), ("ny", (0.0, -1.0, 0.0))):
    add(f"{dname}_plain", d, ["smp.3d r0, r8, r4  [skipinv]"])
    for lod in range(4):
        add(f"{dname}_lod_{lod}", d,
            [f"limm r12, #{f32(float(lod)):#010x}  [skipinv]",
             "smp.3d.lod r0, r8, r4, r12  [skipinv]"])
    add(f"{dname}_grad_zero", d,
        grads([0.0] * 8) + ["smp.3d.grad r0, r8, r4, r12  [skipinv]"])
    for val, tag in ((0.5, "one"), (0.25, "quarter")):
        for k in range(8):
            v = [0.0] * 8
            v[k] = val
            add(f"{dname}_grad_{tag}_{k}", d,
                grads(v) + ["smp.3d.grad r0, r8, r4, r12  [skipinv]"])


# wider: every register from r11 to r27 zero, then one at a time 0.5 from r11
def wide(vals):
    return [f"limm r{11 + k}, #{f32(v):#010x}  [skipinv]" for k, v in enumerate(vals)]


add("px_wide_zero", (1.0, 0.0, 0.0),
    wide([0.0] * 17) + ["smp.3d.grad r0, r8, r4, r12  [skipinv]"])
add("px_wide_zero_src_r11", (1.0, 0.0, 0.0),
    wide([0.0] * 17) + ["smp.3d.grad r0, r8, r4, r11  [skipinv]"])
add("px_wide_neg", (1.0, 0.0, 0.0),
    wide([-0.0] * 17) + ["smp.3d.grad r0, r8, r4, r12  [skipinv]"])
for k in range(17):
    v = [0.0] * 17
    v[k] = 1e-6
    add(f"px_wide_tiny_{11 + k}", (1.0, 0.0, 0.0),
        wide(v) + ["smp.3d.grad r0, r8, r4, r12  [skipinv]"])
# the 2D gradient form on the cube state, for comparison
add("px_2dgrad_on_cube", (1.0, 0.0, 0.0),
    wide([0.0] * 17) + ["smp.2d.grad r0, r8, r4, r12  [skipinv]"])

# smp.2d.grad with the cube's state: does the face still come from (x, y, z),
# and are r12-r15 face-space (s, t) gradients as for a 2D texture?
for dname, d in (("px", (1.0, 0.0, 0.0)), ("ny", (0.0, -1.0, 0.0)),
                 ("pz", (0.0, 0.0, 1.0)), ("off", (1.0, 0.5, -0.25))):
    add(f"c2_{dname}_zero", d, wide([0.0] * 17) +
        ["smp.2d.grad r0, r8, r4, r12  [skipinv]"])
    add(f"c2_{dname}_plain2d", d, ["smp.2d r0, r8, r4  [skipinv]"])
    for k in range(4):
        for val in (0.5, 0.25, 0.125):
            v = [0.0] * 17
            v[1 + k] = val
            add(f"c2_{dname}_g{k}_{val}", d, wide(v) +
                ["smp.2d.grad r0, r8, r4, r12  [skipinv]"])

# map each of r12-r17 on each face: smp.2d.grad on the cube's state
for dname, d in (("px", (1.0, 0.0, 0.0)), ("ny", (0.0, -1.0, 0.0)),
                 ("pz", (0.0, 0.0, 1.0))):
    for k in range(6):
        for val in (1.0, 0.5, 0.25):
            v = [0.0] * 17
            v[1 + k] = val
            add(f"m_{dname}_r{12 + k}_{val}", d, wide(v) +
                ["smp.2d.grad r0, r8, r4, r12  [skipinv]"])
