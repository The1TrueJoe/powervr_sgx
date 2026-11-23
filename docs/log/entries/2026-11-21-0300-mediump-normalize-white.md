# OpenArena's ES renderer: far surfaces white, mediump normalize() overflowing F16

2026-11-21, a CE5300 board (the packaged 0.2.x kernel), our Mesa staged into the Debian
root by hand. Debian's OpenArena 0.8.8 (ioquake3 engine) unpacked beside it. SDL on KMS
(simpledrm, 720x480), the `demo088-test1` demo. Issue #5: with `r_PreferOpenglES 1` the
walls were white on 0.2.2. Before 0.2.2 these shaders didn't compile.

**Reproduced:** `cl_renderer opengl2`, `r_PreferOpenglES 1` (an OpenGL ES 2.0 context,
GLSL ES 1.00). The sky and every surface past a short distance draw white; near ones are
textured. The desktop GL 2.1 path (`r_PreferOpenglES 0`) draws the same scenes right.

**Narrowed down:**

| switch | far surfaces |
|---|---|
| none | white |
| `SGX_DEBUG=nof16` (no fp16 shader cap) | right |
| `SGX_DEBUG=nof16samples,nodepf16samples` | white |
| `SGX_COMPILER_DEBUG=nomediump` (our mediump pass off) | white |
| `nof16iter`, `nof16const`, `nominmax32`, `nonarrow16` | white |

Only the fp16 cap mattered, so the 16-bit NIR came from Mesa's GLSL front end: a mediump
fragment shader arrives lowered to 16 bits.

**Cause:** `normalize(v)` is `v * rsq(dot(v, v))`, and `length(v)` is `sqrt(dot(v, v))`.
With `v` mediump, the dot product is 16-bit. F16 tops out at 65504, so `dot(v, v)`
overflows once `|v|` passes 256. ioquake3's view and light directions are world-space
vectors, and Quake units make most surfaces farther than that. The overflowed dot
product made the lighting saturate to white.

**Fix:** the dot product a square root is taken of is computed in f32 from the same
operands, and so is the root. Only `1 / |v|` or `|v|` is rounded to F16 for its users;
both fit. The fix covers both forms:
- 16-bit NIR from the front end (`root_of_dot16_instr`);
- our own mediump lowering, which keeps such a dot product f32 (`root_of_dot_instr`).

A dot product of highp vectors is untouched.

**Checked:**
- Four screenshots at fixed demo frames (`timedemo 1`, `wait` counts):
  - the fix's match `nof16`'s;
  - 0.2.2's show the sky and distant geometry white in all four.
- dEQP-GLES2 `functional.shaders.*` (10,101 cases) on the fix: 10,047 pass, 1 expected
  failure, 53 skipped. The ExpectedFail matches dev's baseline; there are no new
  failures.
- Two compiler tests:
  - `fragment_mediump_normalize_dot_f32`;
  - `fragment_16bit_normalize_dot_f32`.
