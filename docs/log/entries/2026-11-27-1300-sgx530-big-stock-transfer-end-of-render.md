# SGX530: a long stock transfer seen tile by tile, b27 ends a render, one end-of-tile event

**2026-11-27, 09:20 to 09:40 UTC. The OMAP3630 test board. One one-shot capture boot of the stock
stack (observation only), the normal SD boot restored after it, then our own stack. Released
clean.** This parks the multi-tile PBE work. The SGX530's PBE result is the one correct 16×16
tile of [2026-11-27-0900](2026-11-27-0900-sgx530-pbe-writes-a-tile.md) and
[1000](2026-11-27-1000-sgx530-pbe-tile-word-one-tile-per-job.md).

## A long stock transfer (O)

The kernel poll of [1200](2026-11-27-1200-sgx530-stock-transfer-polled.md) ran on 1024² and
2048² `glTexImage2D` uploads: five registers every 1.73 µs, with the OR of every event-register
value.
- **The upload runs as a series of transfers, one tile row each,** 64 tiles across. A new one
  is queued while the previous one runs.
- **`ISP_STATUS2` (0x4e4) advances one tile every ~2.6 µs,** in row order: (2,1), (3,1) …
  (63,1), then the next row in the next transfer.
- **`PIXELBE_EMIT` (0x4ec)** drops from 3 to 0, 1 or 2 for one sample at a time during a
  row, and is 3 again between transfers. So 3 means the PBE's two emit slots are free.
- **Nothing in `EVENT_STATUS` between tiles.** At the end of a row:
  1. the last tile;
  2. ~17 µs later, the ISP clock goes off (`CLKGATESTATUS` 0x00100110 → 0x00100100);
  3. ~12 µs later, **`EVENT_STATUS` = 0x00040000, the pixel back end's end of render (b18),
     for two samples**;
  4. it's acknowledged, and the clocks stop.
- `EVENT_STATUS2` stays 0 throughout. No ISP end-of-tile (b23) is ever seen.

So the stock's pass is: tiles one after another, with the PBE emit slots cycling and no
per-tile host event, then the PBE's end of render once after the last tile.

## On ours: b27 ends the render (C)

The SGX535's tile word puts in1 b2 at b27 (L). Forcing b27 into every end-of-tile tile word
(`-g 11,0x07f7f800,25,0x08000000,0x18000000`) changes the full render:
- **the job completes**, with no timeout or fault;
- the CCB is consumed and `EVENT_STATUS2`'s `TRIG_3D` is clear;
- one tile is written, and the job ends there.

So **b27 in the tile word is the end of render on this core**, and the end-of-render path to
the microkernel works. Without it, no end-of-render comes.
- None of in1 b0 or b4-b11 copied to b27 gives a last-tile flag: the job still stops after one
  tile.
- Neither does in0 b16.

## Only one end-of-tile event (C)

An end-of-tile program that increments a word in memory counts the end-of-tile tasks:
- **With every clear program tried, it runs once per render:**
  - `limm r0` or `limm o0`, with or without `phas.i`, with `nop`s;
  - img-sgx's clear with the o0 page mapped;
  - with the ptoff event also running it (`-j`);
  - with the SGX535's DOUTD of the PBE words into pa0-pa3 (`-M`, controls 0x80000003 and
    0x00600003);
  - with the SGX535's DOUTU word layout (0, then the tile word in word 2).

  The ISP still walks every tile ([1100](2026-11-27-1100-sgx530-one-tile-per-job-leads.md)).
- **With a clear that has `phas.i` and memory traffic** (a load, an add, a store,
  `idf`/`wdf`, then `limm r0 … [end]`), it runs 13-18 times, about once per tile. The same
  clear without `phas.i` gives 1.
- With that clear and the PBE end-of-tile program, the PBE writes nothing: the clear uses
  r1/r2.

So the one-tile limit sits before the PBE. The 3D issues the next tile's end-of-tile event
only if the pixel tasks before it ran a certain way. Here that's `phas.i` with memory traffic,
which the stock's clear (one `limm` to o0) doesn't need.

## Parked

What's established on the SGX530, through the transfer path:
- **Tile placement:** the SGX535's tile word (b28 enable, x b11-18, y b20-26).
- **End of render:** b27 in that word.
- **The PBE words:** the emits' third operand = pitch/2 − 1, (h − 1) << 12 | (w − 1), b15,
  the address; sideband 0x204.
- **0xa64 = 0x4ffe**, with an attribute size of 2.

What's open:
- why the 3D issues only one end-of-tile event after a plain pixel task;
- the last-tile flag's source (in0/in1 bit), so b27 can go on the last tile only;
- the b1 read of a pixel task's o0.

New blit knobs, off by default:
- `-M w0,w1,w2,w3[,ctl]`: the end-of-tile DMA of PBE words into pa;
- `-j`: ptoff runs the end-of-tile program;
- `-G` now also sends the SGX535's DOUTU word layout.
