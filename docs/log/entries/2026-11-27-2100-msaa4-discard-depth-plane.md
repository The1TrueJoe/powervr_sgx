# A discard on a 4x target: the depth plane comes after the sample positions

2026-11-27, a CE5300 board (SGX545 rev 1014), our Mesa (x86_64, glvnd), surfaceless EGL,
desktop GL 2.1. Issue #89 (Cedarview, N2600): with 4x MSAA, trees in Super Mario 64 (alpha
cut-outs, `discard` on the texel's alpha) are drawn in front of the trees that should hide them.
The reporter's `discard_probe` shows two bugs, both only with 4 samples and only in a program
that can discard, even when the discard is never taken:

1. with one float varying, the varying reads the quad's depth (red 128 for depth 0.5);
2. the depth such a draw stores is about 0, with any number of varyings.

**What the registers hold.** A discarding program gets one more PDS iteration after its
inputs, the depth plane (source 0x22, 3 components) that `pcoeff` hands to the ISP. A debug
build moved pa<k> into o0 instead of the color, so the pixel read back is the register's raw
bits. One varying (pa0), plane from pa1, a flat quad at depth 0.5:

| register | 1 sample | 4 samples |
|---|---|---|
| pa1 | 0 | `eaa26e26` |
| pa2 | 0 | 0 |
| pa3 | 0.5 | 0 |
| pa4 | the next pixel's | 0.5, in the next pixel's pa0 |

With 4 samples the PDS puts the render's sample positions word in front of the plane, the
value the TA command's tail carries at +0x2a4 (`0x88` with one sample). Read as (x, y) nibble
pairs in 1/16 pixel, `eaa26e26` is (6, 2), (14, 6), (2, 10), (10, 14), a rotated 4x grid. The
program's PDS size word gave it 4 registers, so the plane's last word went to the next pixel's
pa0: bug 1, every pixel's `u` is the plane's constant. With two varyings the program has 8
registers and the varyings survive. `pcoeff pa1, pa2` read the sample word and A as the plane,
so C was B = 0: bug 2.

**Change:** a discarding program drawn into a 4x target gets its own variant
(`sgx_fs_program(..., ms4)`), with `pcoeff` on pa<n+1>, pa<n+2>, and the draw gives it n + 4
primary registers. A one-sample target keeps the same program and sizes as before.

**Result:** `mesa/tests/sgx_msaa_discard` (new): 88 checks at 0 and 4 samples (varyings, the
stored depth at six depths, a sloped plane compared pixel by pixel with the plain program's,
alpha test, a gl_FragCoord cut, discard with gl_FragDepth).

| Mesa | 0 samples | 4 samples |
|---|---|---|
| dev (f78b1f8c) | 44 of 44 | 19 of 44 |
| this change | 44 of 44 | 44 of 44 |

`discard_probe` passes `fbo 4`, `win 4` and `es2 4`, and prints the same as dev with 0 samples.
`sgx_msaa`, `sgx_fragdepth`, `sgx_lines`, `sgx_occlusion`, `sgx_samplespassed`, `sgx_depthclear`
and `sgx_mrt` give the same results as on dev.

piglit, 2851 tests (the 4x `ext_framebuffer_multisample` list, GL 1.x/2.x, discard and alpha
test), dev against this change: 911 against 909 pass, no timeouts, no device lost. The one
difference is `rasterpos@glsl_vs_uniforms`, a glRasterPos through st's draw module. It isn't
this change's logic: it fails on dev with 128 bytes of padding added to the end of
`struct sgx_pipe_shader`, and when that struct last grew (`no_discard`, 5b8039ce) it went the
other way, from fail to pass. Something depends on the struct's size; not found yet. 140 of the
161 multisample tests skip on this driver, so piglit covers little of 4x.
