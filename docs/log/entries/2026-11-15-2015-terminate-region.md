# The TA terminate closed only 720x480: big targets' pages stayed counted (issue #5's FBO slowdown, fixed)

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
attribute, 0x0016000e, is the region the terminate closes: `(tile columns - 1) << 16 | (32-pixel
rows - 1)`. On a bigger target the tiles outside 736x480 were never closed, and the DPM kept their
pages counted after the render.

**Fix:** each render's terminate is sized to its target. With it (`SGX_TERMSIZE=1` while testing),
2048x1200's 0x724 is 0 after every render (0x52, 0xa4, 0xf6, ... without), 1280x720's stays 0; a
2048x1200 frame takes ~7.6 ms instead of ~4.9 (the TA now closes every tile).
