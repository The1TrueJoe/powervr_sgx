# SGX530: the PBE's tile word decoded, and one tile per job

**2026-11-27, 05:35 to 06:20 UTC. The OMAP3630 test board, under lock, RAM only, the driver with
0xa64 = 0x4ffe. Released clean.** It follows
[2026-11-27-0900](2026-11-27-0900-sgx530-pbe-writes-a-tile.md).

New blit-tool knobs (off by default):
- `-R x0,y0,x1,y1` renders only those tiles;
- `-D` now also prints the bounding box of the written words;
- `-G` now makes the whole pixel-event program the SGX535's (L, img-sgx):
  - the SGX535 tile word;
  - no emit task on in1 b2 or b8 (the SGX535 has none);
  - `ptoff [end]` for the ptoff event.

## The tile word is the SGX535's (C)

One tile at a time (`-R`), with the words from 0900:

| tile | SGX535 tile word (in0 << 11) | the SGX545 form (in0 << 3) |
|---|---|---|
| (0,0) | (0,0)-(15,15) | (0,0) |
| (1,0) | **(16,0)** | (0,0) |
| (0,1) | **(0,16)** | (16,0) |
| (2,0) | **(32,0)** | nothing |
| (3,3) | **(48,48)** | nothing |

So on the SGX530 the end-of-tile task's DOUTU word 2 is the SGX535's:
- b28 enables the write;
- the tile x from in0 b0-7 lands at b11-18;
- the tile y from in0 b9-15 lands at b20-26.

## The PBE words, separated (C)

With the PBE words in r4-r7 and the emits' third operand in r0, varied on its own:
- **The emits' third operand sets the row spacing:** (field + 1) × 2 words, from its low
  bits. r0 = 31 is a 64-pixel pitch. The first emit's first word doesn't change it.
- The second emit's first word needs only b15. 0x8000 and the SGX535's (stride − 1) << 15
  both work.
- The first emit's immediate decides which tile is written in a multi-tile render:
  - img-sgx's 16: the first tile, (0,0);
  - 0 or 2048: the second;
  - with 16, the first emit's first word must be 0, as img-sgx has it.
- The emits' flags (`f12`, `f3_20` or none) change nothing.

## The clear without the o0 read (C)

With img-sgx's emit form (first emit imm = 16, first word 0), a clear that writes **r0**
(`limm r0, #colour [skipinv end]`) or pa0 puts the colour in the tile: 220/256 right. There's
no b1 read, so no page needs to be mapped (`probe_kpage` dropped). The wrong pixels are left
over from earlier tasks. The o0 clear gets 254/256 but still has the b1 read of its value as
an address.

## One tile per job (C, open)

Every job writes exactly one tile, then stops:
- in a tile range, it's the first tile (imm 16) or the second (imm 0/2048), never more;
- a single tile is written, and then the job doesn't complete;
- at the stop:
  - `PIXELBE_EMIT` (0x4ec) reads 3 (both status bits);
  - `EVENT_STATUS` 0 (no pixel back-end end-of-render, b18);
  - on a single tile the ISP and TSP clocks are off (the 3D has finished);
- a single tile at an odd x faults at 0x0f400000 (requestor b2: a code fetch at code base 0).
  At an even x it times out.

What doesn't change it:
- 0xa64:
  - every field tried (pipes, PNS, data master, output size, SD; 0x4fff);
  - the attribute size (b13-21 = 2) is needed for the PBE to write at our address at all;
- the end-of-render emit task;
- the ptoff task;
- the emit flags.

So the PBE takes one end-of-tile's state and then nothing. Both of `PIXELBE_EMIT`'s status
bits stay set, and no end-of-render event reaches the microkernel. That looks like the PBE's
emit slots are never released. What releases them on this core is the open question.
Candidates:
- the stock end-of-tile program's own form (not observable from here);
- something the SGX530's PBE needs in the six emitted words that this decode hasn't found.

The 3-of-8 end-of-tile register result (11-27) wasn't retested. Every job here stops after
one tile, so it can't be told apart.
