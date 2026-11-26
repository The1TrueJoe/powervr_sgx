# LOD bias, MIN_LOD, MAX_LOD, and BASE_LEVEL after a MAX_LEVEL tail

2026-11-25, a CE5300 board (SGX545 rev 1014, the 7.3.3 kernel it boots, open microkernel
v0.2.1), our Mesa in the Debian root, surfaceless EGL. ES 3.0 makes BASE_LEVEL, MAX_LEVEL,
MIN_LOD and MAX_LOD core, and dEQP-GLES3 tests them. piglit's `tex-miplevel-selection` runs
had shown two failures since the GL 3.0 work turned on `mixed_framebuffer_sizes` (docs/GL3.md
"Status", runs 4 and 5).

**Cause 1, BASE_LEVEL 5 read level 4:** not the hardware. `mesa/tests/sgx_baselevel` uploads
chains from the CPU and samples every level as BASE = MAX = l. Every level reads right, in
RGBA8888, RGBA4444, RGB565 and L8, at 32x32 to 256x256, 64x16 and 16x64 (TEX.md "A chain from
any level"). The failure came from the driver's MAX_LEVEL tail:
- A 2D view of levels l..m with mipmaps has the device's levels past m box-filtered from m
  (`sgx_resource_tail`, `fill_tail`), so that a sample past MAX_LEVEL reads (a smaller copy
  of) level m.
- piglit draws BASE = MAX = 4 with NEAREST_MIPMAP_NEAREST before BASE = MAX = 5. The first
  draw left level 5 holding level 4's texels.
- `sgx_resource_tail` returned early for a tail at the chain's end, so the real level 5 never
  came back. `sgx_baselevel`'s "tail" mode shows it at every level of every chain: 206
  failures on dev.

**Cause 2, the sampler's LOD bias and clamps did nothing:** no texture-word field takes them.
T0 b21-26 only biases down (STATE.md), and nothing clamps.

**Change:**
- `sgx_resource_tail` gives a view its levels back: a mipmapped view sets the tail from its
  MAX_LEVEL (the chain's end included), and a view without mipmaps from a level past the
  tail puts that level back first.
- `lower_lod_ctl`: a sample on a unit whose sampler has a LOD bias, a MIN_LOD above 0 or a
  MAX_LOD below the view's levels takes an explicit LOD (smp's LOD mode 2):
  - the shader's (textureLod), or log2 of the coordinate's coarse derivatives scaled by the
    base level's size, plus the shader's bias;
  - then the sampler's bias, clamped to [MIN_LOD, MAX_LOD], MAX_LOD no further than the
    view's last level. The three come from driver constants, `SGX_K_LOD` + 3u (2304 on;
    `SGX_MAX_CONST_K` 2304 → 2368), so a change of value costs no variant.
- The same goes for a cube or volume view whose MAX_LEVEL is short of the chain the TPU
  takes from the size (only 2D textures get a tail), and for a cube view from a later
  BASE_LEVEL: a cube's words start at level 0, so its constants are offset by its base level.
- A volume's implicit-LOD dependent sample takes its LOD from the derivatives, as 2D and cube
  ones already did (`lower_dep_lod`): a shader bias on a volume read the wrong level in quads
  its primitive covers in part.

**Tests** (dev 78d0655b against this branch):
- `sgx_baselevel`: 206 failures → 82. The 82 are older and not LOD control: RGB565 sampled
  with R and B swapped from a highp desktop-GL program (every level, every size), 128x8's
  levels (a texture under 16 on a side that may be rendered into is linear and has no
  mipmaps), and the last level of a non-square L8 chain (TEX.md: a base address 2 bytes into
  a word).
- piglit `tex-miplevel-selection`, `-nobias -nolod`, `-nobias` and the default (`-lod`,
  `-lod-bias`): fail → pass, 5292/5292 for the last.
- piglit, the 711 tests whose names have miplevel, lod, mipmap, base or max level: 207 → 226
  pass, 19 fail → pass, none pass → fail, crashes 8 and 8:
  `levelclamp`, `lodclamp`, `lodclamp-between`, `lodclamp-between-max`, `mipmap-setup`,
  the three `tex-miplevel-selection` above, `fbo-mipmap-copypix`, `sampler-objects`, six
  `arb_shader_texture_lod` and three GLSL 1.20 `tex-miplevel-selection` 2D cases.
  - A first run had six `textureProj` 1D cases crash: `lower_lod_ctl` used the coordinate's
    source index after removing the projector source. Fixed; the run above is after the fix.
- dEQP-GLES3 `texture.mipmap` (236 cases; dev with 2D arrays, transform feedback and the ES
  3.0 switch merged in for an ES 3.0 context): 149 → 183 pass, nothing pass → fail.
  - Fail → pass: `min_lod` and `max_lod` of 2D, 3D and cube (cube with NEAREST minification),
    `max_level` of 2D and 3D, `bias` of 3D, cube `base_level` and `max_level` with NEAREST
    minification.
  - Still failing: NPOT mipmaps (the base level only), and every cube case with LINEAR
    minification, `basic` among them. That's not LOD control and isn't looked at here.
- Full dEQP-GLES2 (GLES2 has no sampler LOD state, so only the tail and the volume LOD could
  change it): 16,644 pass, 70 expected fails, 446 skips, as on dev, case for case.
- piglit's fixed-function list (358 tests): 3 fail → pass (the three `tex-miplevel-selection`),
  nothing else changes. The GL 2.1 list (2105): the same as dev test for test.
