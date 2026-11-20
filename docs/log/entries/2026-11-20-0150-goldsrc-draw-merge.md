# GoldSrc's immediate mode: the draws of one call merged into one

2026-11-20, a CE5300 board (the packaged powervr_sgx kernel, open microkernel), our Mesa staged
into the Debian root by hand, surfaceless EGL, desktop GL 2.1 on a pbuffer. Testers report
Counter-Strike 1.6 in OpenGL mode running slower than its software renderer. GoldSrc's
renderer is desktop GL 1.x in immediate mode: every world face is its own
`glBegin(GL_POLYGON)` with a base texture and a lightmap, the textures switched every few
faces, then models as strips and fans, then sprites.

**Bench:** `mesa/tests/sgx_gl1bench` draws such a frame: 1,500 faces of 5 vertices, a texture
switch every 6 faces, 8 lightmap updates and 100 alpha-tested sprites, 640x480. Options take
each part away. `SGX_DEBUG=stats` gives the driver's time.

**Where the time went (dev, fbc83abf):** 24.4 fps (40.9 ms a frame: world 26.8, sprites 5.3,
finish 8.5). The driver's draw path took 920 ms of every 2 s. A temporary count split it:
- about 15,400 `draw_vbo` calls carried 70,400 draws (4.6 a call: Mesa's vbo module hands over
  each run of `glBegin`/`glEnd` blocks between two state changes as one call);
- each call's state setup cost about 14 us, and each draw in it about 9 us more, whether by
  zero-copy or the CPU gather: a 5-vertex polygon cost more than its vertices.

| bench variant | fps |
|---|---|
| default | 24.4 |
| no texture switches (`-b 100000`) | 34.0 |
| `GL_TRIANGLE_FAN` instead of `GL_POLYGON` (`-t`) | 24.2 |
| one texture unit (`-1`) | 28.4 |
| a `glDrawArrays` a face from client arrays (`-a`) | 16.7 |
| no sprites, no lightmap updates | 29.2 |

**Change:** the draws of one call of a type drawn as a triangle or line list (polygons, fans,
strips, quads, quad strips, line strips and loops, and lists) become one draw: their
expanded indices concatenated, each vertex fetched once as one range. If the indices
exceed a draw's limit or the vertices are too sparse, the draws go one by one as before.
`SGX_DEBUG=nomerge` turns it off.

**Correctness:** `mesa/tests/sgx_merge` draws each merged type in its own band of a 256x256
pbuffer: polygons smooth and flat, fans, odd and even strips, quads and triangles with
vertices to trim, quad strips, line strips, loops and lists, `glMultiDrawArrays` of fans,
`glMultiDrawElements` of strips, polygons of 3 to 10 vertices. The 14 bands hash the same
merged (13 merged draws) as with `nomerge`, and as with `nomerge,nozerocopy`. None is empty
and there are no GL errors.

**Results (bench, default options):**

| build | fps | ms a frame (world) | draws in 2 s | driver draw ms in 2 s |
|---|---|---|---|---|
| dev | 24.4 | 40.9 (26.8) | 70,400 | 919 |
| merged | 30.5 | 32.8 (19.1) | 89,600 | 715 |
| merged, `nozerocopy` | 33.2 | 30.2 (17.1) | 97,600 | 687 |

(`draws` counts the draws Mesa handed over, before merging: more frames in 2 s, more draws.)

**Left:**
- The per-call setup, about 14 us, now dominates. GoldSrc switches textures every few faces.
- Zero-copy is slower than the CPU gather for these small draws (single-draw calls such as
  sprites).
- The CPU waited on fences for 350 ms of every 2 s at the bench's `glFinish`.
- Not yet run in Counter-Strike itself.
