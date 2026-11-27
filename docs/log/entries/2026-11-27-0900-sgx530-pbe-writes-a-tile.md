# SGX530: the PBE writes a tile to the destination

**2026-11-27, 05:15 to 05:50 UTC. The OMAP3630 test board, under lock, RAM only, the driver with
0xa64 = 0x4ffe (#86) and the sgx530 3D image. Released clean.**

img-sgx (L), the public SGX535 driver Joseph cleared on 2026-11-27, was read for its
end-of-tile program, its PBE words and its pixel-event program. Its words were decoded with
our own tools (`sgx-disasm`, our PDS encoding notes). The SGX530 runs below use programs
assembled by `sgx-asm` from our own source, and blit-tool knobs (all off by default):
- `-G` / `-g`: the event program's tile word;
- `-D`: list the destination's nonzero words.

## What img-sgx does at the end of a tile (L)

- **Its end-of-tile USSE program** is two emits:
  - `emit.t0 pa2, pa3, r0 [f3_20 imm=16]`;
  - `emit.t0 pa0, pa1, r0 [end f3_20 imm=516]`.

  No `phas.i`, no `limm`. Our `sgx-asm` gives the same two words.
- **The PBE words come from memory into pa0-pa3.** The event program DMAs four words:
  - the pack mode (b28-31) | (stride in pixels − 1) << 15;
  - the address;
  - 0;
  - (h − 1) << 12 | (w − 1).
- **The pixel-event program** (0xa5c, data 0xa60 = 4, info 0xa64 = 0x4fff on the SGX535):
  - it tests in1 for the event. b3 runs `ptoff [end]`; b1 is the end of a tile.
  - for the end of a tile, it DMAs those four words. It then starts the end-of-tile task with
    DOUTU word 2 = **0x10000000 | ((in0 << 11) & 0x07f7f800) | ((in1 << 25) & 0x08000000)**.
  - ours (the SGX545's form) sends 0x00800000 | ((in0 << 3) & 0x3ffff8) | ((in1 << 20) &
    0x400000).
- **The clear** is one `limm o0, #colour [skipinv end]`.

**TI's header (L) names 0xa64 `EVENT_PIXEL_PDS_INFO`.** Its fields:
- SD b0;
- output-data size b1-7;
- USE pipe b8-9;
- PNS b10;
- data master b11-12;
- attribute size b13-21;
- secondary exec b25.

The SGX530 stock's 0x4ffe is the pixel data master, both pipes, PNS, output size 0x7f,
attribute size 2, SD clear.

## On the SGX530 (C)

The control first, with the end-of-tile program that stores 0xcafef00d: the store lands at
pixel (0,0). So the end-of-tile task runs under 0xa64 = 0x4ffe.

| change | result |
|---|---|
| img-sgx's emit forms with our tile word (six variants: pa or r, with or without `phas.i`, SGX535 or SGX545 word layout) | nothing written, timeout |
| **the SGX535 tile word** (`-G`), with a clear that writes r0 | **the destination gets writes for the first time**. They are stale register contents, in row 0 |
| `-G` with the default clear (`limm o0`) or img-sgx's clear | **0xff00ff00 in the destination, the right bytes at the right address**: 38-46 pixels, then the known b1 read of o0 as an address (0x0f00f000) |
| the same with a page mapped at 0x0f00f000 (`probe_kpage`) | no fault, the same 38/46 words, then a timeout |

**Then, the PBE words one at a time** (`-P`, our end-of-tile program: `emit (r0, r1)
imm=2048`, then `emit (r2, r3) [end] imm=<sideband>`):
- **r0, the emits' third operand too, sets the row spacing.** The PBE writes 16 runs of
  8 words, (field + 1) × 2 words apart. The field is r0 b0 up; b16 and b15 don't count. r0 = 31
  gives a 64-pixel pitch: the rows land 64 words apart.
- **r1 = (h − 1) << 12 | (w − 1)**, img-sgx's 12-bit form.
- **r2 needs only b15 (0x8000).** 0, or anything without b15, writes nothing.
- **r3 is the address.**
- **The sideband (second emit's immediate) 0x204 makes the rows 16 pixels wide.** 0x200,
  ours, makes them 8.
- The first emit's immediate: 0 and 2048 work, and 16 (img-sgx's) writes nothing.

With r0 = 31, r1 = 0x0003f03f, r2 = 0x8000, r3 = the destination and the sideband 0x204, **one
full 16×16 tile is written: 256 words of 0xff00ff00 at the right pitch** (250-252 right; the
few wrong ones are stale register contents).

**The tile word, `-g` (in0 shift, mask; in1 shift, mask; constant):**
- **b28 enables the write.** Without it nothing is written, and b23 (the SGX545's constant)
  stops it.
- With the SGX545's shift (in0 << 3) and b28, the tile lands at x = 0. With the SGX535's
  in0 << 11, it lands at x = 16 (on a 16×16 target, at x = 0).
- Only one tile is ever written: on 16×16, 32×32 and 64×64 targets. The job then times out,
  or completes with the other tiles missing.
- r0-r3 are left over from earlier tasks, across module reloads. A task that stores them
  reads the last run's values, so they aren't the DOUTU tile word.

## Where it stands

**The PBE on the SGX530 writes a tile to our destination (C).** The run needs:
- 0xa64 = 0x4ffe;
- a b28 tile word from the event program;
- the four words and the 0x204 sideband above;
- a pixel task that writes o0, with the o0 address read mapped.

That's the first output on this core that isn't a store from a pixel task.

**Open:**
- **The other tiles.** One tile's event writes, and the others write nothing or stall. With a
  uniform colour, which tile's event wrote it can't be told. The next step is a per-tile
  marker, for example a colour from the event, or a target that only one tile can reach. Then
  the tile word's x/y fields.
- **The meaning of the words.** The row field turned out to live in the emits' third operand,
  r0, which our program and img-sgx's both name. Which of the 530's six emitted words carries
  what is G beyond the measurements above.
- The b1 read of a pixel task's o0, which stops the job unless that page is mapped.
- Whether this explains the end-of-tile task's 3-of-8 register results under 0x4ffe (11-27).
  Not tested.
