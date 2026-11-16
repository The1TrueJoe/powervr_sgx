# The TA terminate closed only 1440x960: big targets were partly drawn and kept their pages counted (issue #5's FBO slowdown, fixed)

2026-11-15, a CE5300 board, RAM boots of release/v0.2 kernels with the DPM counters in the status
dump (0x700/0x710/0x724/0x72c and the 3D side's 0x70c/0x71c/0x728/0x730) and the open microkernel
(stock, or its UK_DPMLOG build recording 0x700/0x724 at a scene's first TA finish).

Follows 2026-11-15-1630-pb-leak-large-targets. Each 2048x1200 render left pages counted in use;
measured one render a round (four cleared frames in a round had been one render), it is 82 pages a
render:

- The TA allocates 0x99 = 153 pages for a 2048x1200 scene, as the DDK does (its capture, values
  only, with Joseph's OK).
- After the render the in-use count (0x724, both halves, and the 3D side's 0x728) is 82 higher, every
  render, while the free list's head and tail (0x700/0x710, and the 3D side's 0x70c/0x71c) advance
  by the full 153: the pages are back in the list, only the count drifts. It climbs until the TA's
  thresholds fire: out-of-memory partial renders, slower and slower frames, a job timeout.
- What is freed per scene depends on the macrotile layout, not on the scene: 71 pages for every 16x20
  layout (2048x1040, 1920x1080, 2048x1200), 64 and 57 for 16x16, 40 for 16x12, 63 for 12x16, 61 for
  8x20, all of 1280x720's 66 (12x12).
- Ruled out: the RT numbers (the DDK computes the same: MTILE 0x84020030/0x0502803c, 320 tiles a
  macrotile, the same RT data), the microkernel's DPM writes (in the emulator the DDK's and ours
  match for one scene and for three; the DDK's write order on the board changes nothing), reloading
  the free lists' counts after each render (the high half of 0x724 held, the low half still climbed).

**Cause:** the winsys ended every scene with the TA terminate of the 720x480 capture. Its second
attribute, 0x0016000e, bounds the region the terminate closes. On a bigger target the tiles outside
it were never closed: their pixels were not drawn as they should be, and the DPM kept their pages
counted after the render. The DDK's value fits `(32-pixel columns - 1) << 16 | (32-pixel rows - 1)`
at 720x480, but the region 0x0016000e closes measures 1440x960 (below).

**Fix:** each render's terminate is sized to its target by that formula. With it (`SGX_TERMSIZE=1`
while testing), 2048x1200's 0x724 is 0 after every render (0x52, 0xa4, 0xf6, ... without), and
1280x720's stays 0.

**What the old terminate drew** (`sgx_fboslow -n 3`: three half-transparent quads over the whole
target; pixels unlike the bottom-left one, by quarter of the target):

| target | 0.2.1's terminate | sized to the target |
|---|---|---|
| 2048x1200 | 1075712 wrong: all right of x = 1440, all above y = 960 | 0 |
| 1920x1080 | 691712, the same boundary | 0 |
| 1448x848 | 6784: the strip from x = 1440 to 1448 | 0 |
| 1280x720 | 0 | 0 |

So a big FBO was only partly drawn before the fix. Drawn whole, it costs what its size says: `sgx_gamefix
bigfbo` at 2048x1200 takes 2055 ms a frame with the fix and ~1100 ms without; 1448x848 takes ~1030 ms
both ways.

**Heavy scenes and the job timeout.** With the fix, `sgx_gamefix bigfbo` at 2048x1200 timed out 16 or
17 times in 60 frames, against once without. That is the 2 s job timeout (`job_timeout_ms`), not a
hang:

- 30 textured, added full-screen quads (`sgx_fboslow -n 30 -T`) cost ~64 ms each at 2048x1200 (1, 2,
  4, 8, 16 and 30 layers: 67, 131, 258, 512, 1019 and 1907 ms a frame, all pages back after each
  render; 720x480 30 layers: 275 ms).
- With three frames to a round, the deferred flush of an off-screen frame puts the three in one job
  (the driver's stats: 90 draws, 1 flush): 3.6 s at 2048x1200 and 2 s at 1448x848. Every such job
  timed out, with the fix and without (10 of 10 rounds each). The out-of-memory flags and marks
  stayed 0 throughout, and the DPM had 312 of the PB's 4130 pages out.
- With `powervr_sgx.job_timeout_ms=10000`, the same runs and `sgx_gamefix bigfbo` have no timeouts
  with the fix. Without it, bigfbo's 2048x1200 frames ran the PB dry (0x724 = 0x1020 of 0x1022 pages)
  and hung: the job was never consumed, and the reset ended in "device lost".

A job that legitimately runs past 2 s is reset; that is its own bug (the timeout, and how many
off-screen frames one job may hold), left for later.
