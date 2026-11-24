# The 4x colour resolve on the GPU

2026-11-24, a CE5300 board (SGX545 rev 1014, a 7.3.3 test kernel with the 0.2.1 powervr_sgx
module, open microkernel), our Mesa in the Debian root, surfaceless EGL, desktop GL 2.1 on a
pbuffer. Issue #43 (Cedarview, N2600): a trivial scene runs at 210 fps without AA and 31.6 fps
with a 4x window. `SGX_DEBUG=stats` shows a 1024x600 frame read back every frame.

**Cause:** `sgx_blit()` sends a blit out of a 4x buffer to `sgx_blit_resolve()`. A same-size
resolve there was `resource_copy_region()`, a CPU map of the colour buffer. The map waits
for the render and then copies out of write-combined memory.

**Change:** the PBE resolves each tile as it stores it (STATE.md "Multisampling (4x)"), so
the 4x buffer's memory is a one-sample image of the resolved pixels. `sgx_blit_resolve_gpu()`
flushes the source's render, presents the resource as one-sample for the length of the blit,
and hands it to `sgx_blit_gpu()`. u_blitter then samples it as a plain texture at any scale or
format. The resource itself is relabelled, not a copy of it: the draw marks what it samples
(`use_seq`, the render's buffer list), and a later write to the 4x buffer must wait for this
read. Whatever `sgx_blit_gpu()` refuses still goes to the CPU copy, for example a scissored blit
or a format the TPU can't sample. `SGX_DEBUG=cpuresolve` brings back the old path.

**Result:** `mesa/tests/sgx_msaa bench`, a 4x 1024x600 FBO drawn and resolved by
`glBlitFramebuffer` every frame, 100 frames, alternating:

| Mesa | fps |
|---|---|
| dev (504913f6), CPU copy | 22.7, 22.8 |
| GPU resolve | 98.1, 98.1 |
| one-sample source, for comparison | 156.7 |

With `SGX_DEBUG=stats`, 183 GPU blits in 2 s, nothing read back. `sgx_msaa`'s correctness
cases (colour, depth, scaled, at 720x480 and 256x256) pass on both, with the same pixel counts in
every class (outside, inside, each quarter step). Full dEQP-GLES2: see the PR.

Not done: the window case could skip the resolve altogether if the PBE stored a 4x window's
tiles straight into its back buffer. That is st's decision (it allocates the 4x buffer and
resolves at the swap), so it is left for later.
