# Mipmapped R32, RG32 and RGBA32 textures

2026-11-25, a CE5300 board (SGX545 rev 1014, the 7.3.3 kernel it boots, open microkernel
v0.2.1), our Mesa in the Debian root, surfaceless EGL. `SGX_DEBUG=glsl130`'s description said
RG32 and RGBA32 "sample wrong until the compiler fetches them word by word". The question for
ES 3.0 was what still samples wrong.

**What dEQP-GLES3 says** (the 648 integer-format cases of `texture.*`, on dev with 2D arrays,
transform feedback and the ES 3.0 switch merged in for an ES 3.0 context):
- A single level of every 32-bit format passes (`texture.format.sized.2d`: R32, RG32, RGB32
  and RGBA32, signed and unsigned, POT and NPOT). The compiler does read them a plane at a time
  since 689e008e; the description was stale.
- What failed were mipmaps. `texture.specification.basic_teximage2d.*32*_2d` draws each level
  minified to its own size with NEAREST_MIPMAP_NEAREST. Every 32-bit word texture (R32
  included) failed from level 1 on, because it was stored as strided rows and sampled as 0x12,
  a layout with no mip chain, so every LOD read the base level.

**Change:**
- A 2D R32, RG32 or RGBA32 texture (integer or float) whose levels are twiddled
  (`sgx_tex_word_chain`) is stored as an RGBA8 one is: each plane a twiddled chain to 1x1.
  It's sampled through 0x0c, which returns the word unchanged (docs/GL3.md H2), with NEAREST
  forced, so the sampler picks the level and wraps. Depth textures, NPOT ones and the rest stay
  strided. A render into a level writes it as an RGBA8 level is, its old contents loaded back
  through 0x0c.
- The plane reads of an RG32 or RGBA32 texel are dependent samples, so a mipmapped one took the
  TPU's wrong LOD in quads its primitive covers in part, as any dependent sample does. Such a
  unit's samples go through `lower_dep_lod` (the LOD from fdsx / fdsy), plain ones included.
  The compiler's plane split takes the txd it makes.
- A MAX_LEVEL tail of an integer format decimates its words instead of averaging them as
  floats.

**Tests:**
- dEQP-GLES3, the same 648 cases: 192 → 196 pass with the chains (R32 and R32I/UI of
  `basic_teximage2d` and `basic_texsubimage2d`). With the plane reads' LOD, 12 more: RG32,
  RGB32 (stored RGBA32) and RGBA32, signed and unsigned, of both. That's 16 fail → pass from
  this change and none pass → fail. The run that had both also had the LOD control branch,
  whose volume LOD gave 8 more (8-bit `basic_texsubimage3d`).
  - Still failing in that list: the 16-bit integer formats at every level (no driver format;
    st has no fallback for R16UI), cubes of the 32-bit formats (not offered), NPOT mipmaps
    (`texstorage2d` is 117x97), and volumes of the 32-bit formats.
- `mesa/tests/sgx_inttex` (desktop GL under the GL 3.0 overrides, `SGX_DEBUG=glsl130`; one
  16x16 level of each integer format, sampled and rendered): 15/15 on dev and with this change.
- `mesa/tests/sgx_es3fmt` (ES 3.0 under the override, `SGX_DEBUG=glsl130`): every check passes
  on dev and with this change, the REPEAT and MIRRORED_REPEAT RGBA16F, RGBA32F and R32F cases
  included (the TPU wraps a chain itself now). Its case 9, a mipmapped 4x4 RGBA32F read with
  textureLod at level 1, was informative only: dev reads level 0, this change level 1. It's a
  check now.
- Full dEQP-GLES2 (default flags; R32F is the one word format sampled there): 16,644 pass,
  70 expected fails, 446 skips, the same as dev case for case.
