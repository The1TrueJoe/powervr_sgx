# SGX530: one tile per job, leads checked

**2026-11-27, 06:20 to 06:55 UTC. The OMAP3630 test board, under lock, RAM only. Released clean.**
It follows [2026-11-27-1000](2026-11-27-1000-sgx530-pbe-tile-word-one-tile-per-job.md). New
knob: `powervr_sgx_blit -k mask`, the in1 bits that run the ptoff task (off by default).

## TI's header (L)

- `ISP_STATUS2` (0x4e4) is the ISP's current tile: x b0-7, y b8-15, and the primitive number
  b16-20. So ours ending at 0x202 (11-26) is tile (2,2), and the stock's 0x303 is (3,3).
- `PIXELBE_EMIT` (0x4ec) has only two status bits. `PIXELBE` (0x400) has the alpha threshold
  and `IDF_USE_FBADDR` (b8). No slot count or release field is named for the PBE.
- `EVENT_STATUS` has an ISP end-of-tile event (b23) and the pixel back end's end of render
  (b18). Neither is set at our stop.

## What changed nothing (C)

All runs use `-G`, img-sgx's emit form and the r0 clear:
- 0x400 = 0x00ffff80 (img-sgx's SGX535 render) or 0x180 (`IDF_USE_FBADDR`);
- 0x404 = 0, img-sgx's render type. That faults at 0 (requestor b8) and writes nothing;
- `DMS_CTRL` (0xa74):
  - more pixel partitions or tasks, and the stock's 0x083f8200;
  - with 0 EDM tasks the microkernel never takes the job;
- the end-of-tile program without `phas.i`, and its DOUTU temporaries (`-t` 0, 1, 4, 8);
- the ptoff task on any in1 bit other than end-of-tile's (`-k`);
- with the end-of-tile program `nop [end]` (`-E`), so the PBE gets nothing: no completion
  either.

## What the controls show (C)

- **0x4ec = 3 is its normal value.** It reads 3 at idle on the stock stack (11-26 capture). It
  also reads 3 after our store clear with 0xa64 = 1. That clear "worked" on 11-27, but only
  because its stores finished first: the PBE then faults at 0x0b860000 and the job is reset.
  So no transfer of ours has ever ended with an end-of-render event on this core.
- **The ISP walks every tile in the range.** With `-R`, 0x4e4 ends on the range's last tile:
  (3,0) for a row, (0,3) for a column, (1,1), (2,2). The full 4×4 is the exception at (2,2),
  not (3,3). Why is open.
- So the stop isn't the ISP. The PBE takes one end-of-tile's emits, the other tiles' emits
  write nothing, and the end of render never comes.

## The stock transfer (O, the 11-26 capture)

Its whole transfer happens inside one sample of the register sampler: 0x4e4 goes 0 → 0x303 and
0x4d4/0x4d8 change in the same sample. `EVENT_STATUS` shows only the timer between samples. So
a per-tile view of the stock would need a much faster observation than `sgxsnap`'s polling.
Lead 3 (observe the stock per tile) wasn't run.

## Where it stands

Still one tile per job. The PBE accepts the first end-of-tile's state and nothing after it,
whatever we change in:
- the end-of-tile program;
- the pixel-event program;
- the PBE, ISP and data-master registers the header names.

The odd-x fault at code base 0 wasn't retested (tiles don't chain).
