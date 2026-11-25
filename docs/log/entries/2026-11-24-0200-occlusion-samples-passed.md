# Occlusion queries on by default, and a count an engine can cull by

2026-11-24, a CE5300 board (SGX545 rev 1014, the 7.3.3 kernel it boots, open microkernel),
our Mesa in the Debian root, surfaceless EGL, desktop GL 2.1 into an FBO. Issue #5
(danilolc, an N2600): Red Eclipse 1.6.0 doesn't draw map geometry and models it can see. It
wraps them in `GL_SAMPLES_PASSED` queries and culls anything under `oqfrags` (8) samples.

**Cause:** without the `occlusion_query` cap, Mesa still exposes `ARB_occlusion_query`, which
is GL 1.5, with `GL_QUERY_COUNTER_BITS` 0. It answers every query itself, never asking the
driver (`query_type_is_dummy()`, `main/queryobj.c`), and the answer is 0. So any engine that
trusts the count culls everything it queries. The cap was on only with `SGX_DEBUG=occlusion`.
There, `SAMPLES_PASSED` got 1 for anything visible, which is still under 8.

**Change:**
- The store path (docs/GL3.md "Status", checked on hardware on 2026-11-20) is on by default.
  `SGX_DEBUG=noocclusion` brings back Mesa's stand-ins.
- `SAMPLES_PASSED` has no counter. When the query's slot is set, it returns the most samples
  any of the query's draws' framebuffers has (width × height × samples, what one draw covering
  it once would pass). When the slot isn't set, it returns 0.
- `ANY_SAMPLES_PASSED` is unchanged.

**Result:** `mesa/tests/sgx_samplespassed`, a 128×128 FBO with a depth buffer:

| Mesa | counter bits | no draw | visible quad | hidden quad | visible again |
|---|---|---|---|---|---|
| dev (888d1568) | 0 | 0 | 0 | 0 | 0 |
| dev, `SGX_DEBUG=occlusion` | 64 | 0 | 1 | 0 | 1 |
| this change | 64 | 0 | 16384 | 0 | 16384 |
| this change, `SGX_DEBUG=noocclusion` | 0 | 0 | 0 | 0 | 0 |

**Cost:** a draw inside a query is a translucent pass, so it gives up hidden-surface removal.
`sgx_samplespassed bench` draws 16 full-target quads into a 1024×600 FBO each frame, with each
quad in its own query or with no queries:

| order | no queries | a query each |
|---|---|---|
| front to back | 2.9 ms | 7.1 ms |
| back to front | 2.8 ms | 72 ms |

An engine that queries what it draws normally sorts front to back for the queries' sake.
Draws outside a query are unchanged.

**Tests:**
- Full dEQP-GLES2: 16,649 pass, 70 expected fails, 446 skips, nothing new failing. 13 of the
  passes are unexpected improvements, against a baseline older than #45's cube fixes.
- piglit's 23 occlusion and conditional-render tests, dev against this change:
  - From skip to pass: `arb_occlusion_query2-api` and `-render`, `occlusion-query-discard`,
    `occlusion_query_meta_no_fragments`, `occlusion_query_order`, and `GetQueryCounterBits`.
  - From skip to fail: `occlusion_query_meta_fragments` and `occlusion_query_meta_save`.
  - From pass to fail: `occlusion_query`. It checks counts once the counter bits aren't 0.
  - In all three, each check gets the framebuffer's count where it wants the exact one (18000
    for 400, for example), and every check that wants 0 gets 0. They are in
    `sgx-piglit-fails.txt`.
  - `occlusion_query_conform`'s `GetObjivAval_multi2` still fails, as before with the switch.
  - `nv_conditional_render` stays skipped.
