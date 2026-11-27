# SGX530: first pixels from a 3D-only render (pixel-task stores), and where the PBE writes

2026-11-26 21:00 to 2026-11-27 02:00 UTC, an OMAP3630 board, kernel 7.3.3, RAM only. Our own
code: powervr_sgx with bring-up switches, the sgx530 branch's microkernel with the 0x45 3D
invalidate mask, and `powervr_sgx_blit`. Every run recovered through the driver's reset. The
board stayed on its normal SD boot.

Where it stood: the clear transfer starts the 3D and faults on fixed reads, 0x0f00f000 and then
0x0b860000 ([2026-11-26-1000](2026-11-26-1000-sgx530-3d-only-transfer.md),
[2026-11-26-2015](2026-11-26-2015-sgx530-stock-transfer.md)).

## Result

**A 32×32 block of the destination reads back as the clear colour, 1024/1024 pixels right.** The
background object's pixel program stores 0xff00ff00 into it with `st.a.d`, and the test reads it
through the client's CPU mapping (`powervr_sgx_blit -c -w 32 -S @clear.hex`, the shader from
`tools/first-output/clear_stores.py`). The 3D pass then faults on the PBE's own write (below),
after the stores. (C)

The smaller steps on the way, each C:
- A pixel task's store lands where it's told. `limm r0, #0x0b8f0000; limm r1, #0xcafef00d;
  st.a.d r0, #0, r1` wrote 0xcafef00d at 0x0b8f0000 (a probe page). Four stores at the
  destination made pixels (0,0)-(3,0) right.
- `imae` takes its two multiplicands as 16 bits; only the addend is 32 bits. A pointer goes in
  the addend: `imae r0, r3, #1, r0 [t2=u32]` adds r3 to r0. `imae r0, r0, #1, #4` truncated the
  pointer to 0x0000a004.
- A store's offset field takes up to #127 dwords.
- Loops don't work yet. Neither `(p0) br #-3` / `#-4` nor `(p0) ba #557` repeats the store:
  one pixel lands, then the job times out. Unknown why (the branch's units, or the predicate).

## The fixed reads, explained (C)

- **0x0f00f000 is our clear colour.** 0xff00ff00 masked to 28 bits is 0x0f00ff00, in page
  0x0f00f000. With the colour 0x12345678 (`-K`) the first fault moves to 0x02345000. So a pixel
  task's `limm o0, #c [end]` gets o0 read as an address by requestor b1. The TA's MTE reads the
  state task's o0 the same way (sgx530.md, "The TA stream"). `limm r0, #c; nop [end]` gives no
  such read.
- **Requestor b6 is the PBE, and 0x0b860000 is where it writes.** With pages mapped there
  (`probe_va0_addr`, `probe_va0_pages`), the PBE writes 16 runs of 8 words, 0x8d70 bytes
  apart, from 0x0b85741c to 0x0b8dbdac, and then faults at 0x0f000000. Without the
  end-of-tile program's emits it writes nothing.
  - The first `emit.t0` writes the 16 runs; the second, alone, goes straight to 0x0f000000.
  - The data is the tile's colour with its bytes reversed: 0x78563412 for 0x12345678, and
    0x00ff00ff for 0xff00ff00. It's stale from an earlier run when the pixel task writes no o0.
    The on-chip tile data survives a module reload.
- **What doesn't move the PBE's writes:**
  - the destination (+64 KiB);
  - each of our four PBE emit words and the sideband (`-P`);
  - the kernel data heap (moved 1 MiB, `probe_kdata_base`);
  - 3D_REQ_BASE;
  - the PBE words loaded into o0-o3 or pa0-pa3 too, or a pointer to them in o0 (`-X`, `-Q`);
  - the end-of-tile task's temporaries (`-t 8`), `limm` without `[skipinv]`, the emit
    immediates (512 to 4096);
  - a pixel-event program that only launches the end-of-tile task (`-V`, word 1 = 0x00800000
    or 4).
- In the end-of-tile task, the same store program (`-X`) faults at 0x08039000, requestor b2,
  instead of writing 0x0b8f0000. So what that task's registers hold isn't what our `limm`s put
  there. That fits the emits sending stale values.

## What's left for the PBE path

The PBE takes its state from somewhere the end-of-tile task's code doesn't reach as we write it.
The candidates: how a task launched by the pixel event program sees its registers on this core,
or a PBE setup that the 545 does through the emit words and this core does another way. The
stock end-of-tile program would answer it, and its code is off limits. Its output isn't: with
a stock transfer running, the PBE's writes to a texture are observable (O), as the 11-20 PB
dump was.

## Tools added (all off by default, SGX530 only)

- powervr_sgx: `probe_va0_addr`, `probe_va0_pages` (a patterned range at any address in every
  client VM; the changed words are logged as runs), and `probe_kdata_base` (move the kernel data
  heap within its range).
- `powervr_sgx_blit`:
  - `-K` (colour 0x12345678);
  - `-S words|@file` (the clear shader);
  - `-X words` (the end-of-tile program);
  - `-P i=v` (a PBE word);
  - `-Q` (o0 = a pointer to the PBE words);
  - `-t n` (the temporaries field);
  - `-V w1` (a one-DOUTU pixel-event program).
- `tools/first-output/`: `clear_stores.py` (the store clear) and `sgxsnap.c` (our register
  sampler for observation runs).
