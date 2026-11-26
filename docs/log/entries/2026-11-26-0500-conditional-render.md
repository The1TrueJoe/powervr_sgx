# Conditional render

2026-11-26, a CE5300 board (SGX545 rev 1014, open microkernel 0.2.1), our Mesa in the Debian
root, surfaceless EGL, desktop GL 2.1. GL 3.0 needs `NV_conditional_render`.

**Change:** the driver exposes `NV_conditional_render` and `ARB_conditional_render_inverted`
on its occlusion queries (`SAMPLES_PASSED`, `ANY_SAMPLES_PASSED`).
- No predicate is known in the VDM or ISP. So `render_condition` keeps the query, and before
  each draw, glClear, clear_render_target/depth_stencil and blit that honours the condition,
  the driver reads the query's result on the CPU (`sgx_render_condition_passes`), as llvmpipe
  does.
- The wait modes flush the query's frame and wait for it. The no-wait modes draw while the
  result isn't in, which the spec allows, and flush nothing.
- Only the result is read, so the check doesn't depend on how the query counts. An exact
  `SAMPLES_PASSED` count would change nothing here.
- The blitter's own draws drop the condition (`util_blitter_save_render_condition`), and
  `sgx_blit` checks it once, so a GPU blit and a CPU blit both honour it.

**Tests:**
- `mesa/tests/sgx_condrender.c`: both targets, all 8 modes, a query that passes and one that
  fails the depth test, then a conditional quad or glClear. 64 of 64 pass. On dev the test
  skips, because dev doesn't expose the extension.
- piglit, the conditional-render, occlusion and clipping tests (1,470 results), dev
  (78d0655b) against this change. The 13 `nv_conditional_render` tests go from skip to pass,
  and nothing else changes. `fixed-clip-enables` flakes on both.
- Full dEQP-GLES2, with this change and the clip-plane change together: 16,644 pass, 70
  expected fails, 446 skips, nothing new failing. Each result is the same as in the 0.2.3
  gate run on dev's CI Mesa (2026-11-24, this board).
