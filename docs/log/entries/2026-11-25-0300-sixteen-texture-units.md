# 16 fragment texture units

2026-11-25, a CE5300 board (SGX545 rev 1014, the 7.3.3 kernel it boots, open microkernel v0.2.1),
our Mesa in the Debian root, surfaceless EGL. Issue #5: Half-Life: Source and Portal show a black
screen. Their D3D9-to-GL layer (ToGL, Valve's public source) binds D3D9's 16 samplers and never
reads `GL_MAX_TEXTURE_IMAGE_UNITS`, which was 8. A tester's logs show the errors on its slots
8-15:
- `glActiveTexture(GL_TEXTURE15)`: `GL_INVALID_ENUM`;
- `glUniform1i(sampler, 15)`: `GL_INVALID_VALUE`.

On the board, a texture bound that way silently lands on the active unit, and the sampler reads
unit 0.

**What held it at 8:** nothing in the core. Every sample brings its own 4 state words (ISA.md
"Texture units"). With the count raised to 16, 16 2D samplers in one program drew correctly,
but every dEQP `texture.units.all_units` case with cube maps failed:
- the fragment variant for mipmapped cubes, sampled with a dependent coordinate, takes the LOD
  from derivatives (`lower_dep_lod`), and that needs the texture's size;
- the size is a driver constant (`SGX_K_TEXSIZE`, two per unit), and the block held units 0-7;
- so a cube on units 8-15 kept its `txs`, the variant didn't compile ("texture op 8"), and the
  draw was skipped.

**Change:**
- `SGX_K_TEXSIZE` holds 16 units, numbered from 2008 to 2039, just under `SGX_K_INSTANCE`.
  `sgx_context.h` checks that the two don't overlap.
- The SGX545 gives fragment programs 16 units.
- The SA left for state words is still checked per program: one with more distinct texture
  states than fit in SA is not drawn, with a message, as before.

**Tests:**
- `mesa/tests/sgx_units16`: one program reads 16 mipmapped units, 2D on even units and cube on
  odd ones, each through a matrix uniform, and column k must be unit k's colour.

  | Mesa | result |
  |---|---|
  | dev | fails: 8 units |
  | 16 units without the size block | 16 of 16 columns black (the draw skipped) |
  | this change | every unit right, no GL error |

- dEQP-GLES2 `functional.texture.units` (120 cases, not in the mustpass list), with 16 units:
  - 118 pass, all 20 `all_units` cube and mixed cases among them;
  - the 2 that fail, `2_units.mixed.2` and `2_units.only_cube.0`, fail on dev with 8 units too.
- piglit, dev against this change:
  - `texunits`, `gl-2.0-texture-units`, `vp-combined-image-units`,
    `arb_fragment_program-sparse-samplers` and `gl-2.0-active-sampler-conflict` pass on both.
  - `max-samplers` (and `border`) newly fail. The test loops over a sampler array as long as
    the unit count. The compiler's `max_unroll_iterations` is 8, so a 16-trip loop stays rolled,
    and GLSL 1.10 refuses its indexing at link. They're expected fails now.
- Full dEQP-GLES2 (mustpass): 16,656 pass, 70 expected fails, 1 fail, 438 skips. dev had
  16,649 pass and 446 skips:
  - 8 cases that dev skips for the unit count run now, and 7 of them pass;
  - the eighth, `uniform_api.random.23`, fails to link with "Too many fragment shader default
    uniform block components", the 128 vec4 fragment limit. It's an expected fail now;
  - nothing that passes on dev fails.
  - Of the 16,656, 13 are cases that `sgx-fails.txt` lists. They pass on dev too.

Whether this fixes the black screen in Source is unconfirmed: the game can't run here.
