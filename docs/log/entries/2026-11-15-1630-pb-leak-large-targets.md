# Renders into large targets leave parameter-buffer pages behind (issue #5's FBO slowdown)

2026-11-15, a CE5300 board, RAM boots of release/v0.2 kernels (the v0.2 release kernel, and test
builds of it) with the open microkernel; the packaged mesa-sgx 0.2.1, EGL surfaceless, no X.
The test is `mesa/tests/sgx_fboslow` (a tester's FBO test, issue #5, with sizes, recreation and
counters of its own); `-D` prints the DPM page counters after each round's glFinish from a test
kernel's status dump (0x700, 0x710, 0x724, 0x72c).

**The tester's report reproduces exactly.** Five FBOs from 512x300 to 2048x1200, 8 frames each a
round: flat until round 20, then the 2048x1200 frames take 3.4, 8.8, 11.6, 16.3, 19.2 and 48 ms,
and a job times out at round 27. After the reset it is fast again. Shmem and the process's GPU
memory stay flat: no buffers leak.

**What runs out is the parameter buffer.** After every round, with the GPU idle, 0x724 (the pages
the DPM has handed out, both halves equal) is 0xbe higher: 0xbe, 0x17c, 0x23a, ... 0x101e at
round 27, against a PB of 0x1022 pages. The renders get slow as the TA starts running out of
memory (the microkernel's out-of-memory mark, control block +0x84, reads 0x101e at the timeout),
and the reset re-arms the PB. Nothing returns the pages until then.

**Which renders leave pages behind** (each size alone, 4 frames a round, pages left after a
render; mx x my = tiles per macrotile, the RT numbers of STATE.md section 6):

| target | mx x my | padded tiles | pages left a render |
|---|---|---|---|
| 512x300, 720x480, 1024x600 | up to 8x12 | up to 32x48 | 0 |
| 1280x720 | 12x12 | 48x48 | 0 |
| 1448x848 | 12x16 | 48x64 | 3.75 |
| 1024x1200 | 8x20 | 32x80 | 4 |
| 2048x600 | 16x12 | 64x48 | 9.25 |
| 2048x1040 | 16x20 | 64x80 | 15.5 |
| 2048x1024 | 16x16 | 64x64 | 18 |
| 2048x1200 | 16x20 | 64x80 | 20.5 |

Every target with a macrotile side of 16 tiles or more leaves pages behind at every render; none
smaller does. The RT numbers were captured at 720x480 only (8x8 tiles a macrotile); nothing
larger than 12x12 had been run before. By the same formula a 1600x900 or 1920x1080 target has
16-tile macrotiles.

Ruled out along the way:
- switching between RT datasets: the 2048x1200 target alone loses 20 pages a render too (pairs of
  sizes only ran dry sooner because they rendered the big target more often);
- recreating the FBOs every 5 rounds (new RT datasets, the same PB): the same curve;
- the RT entries' 0x40 and 0x50 TA-data blocks made per PB instead of per dataset: the same curve;
- zeroing a dataset's tail-pointer array at each scene's first kick: the same 0xbe a round.

No page faults, WARNs or BUGs; each timeout recovered with the microkernel restarted.
