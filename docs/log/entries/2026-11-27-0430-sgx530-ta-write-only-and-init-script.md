# SGX530: write-only registers, the stock init script, and 0xa64 counted

2026-11-27 03:50-04:30 UTC, an OMAP3630 board. Our own stack, RAM only, plus one observation-only
capture boot (Joseph's OK, 2026-11-27). Tags: **C** (ours, on the board), **L** (the GPL kernel
module's init-script register writes, recorded as it ran them), **O** (stock values). No stock
code was read or kept.

## 0x42c = 0 (C)

The stock transfer command writes 0x42c = 0, where ours writes 0x80000000, and 0x42c reads back 0
after a write. Neither use of 0 changes anything:
- on the TA kick (a scratch microkernel write just before `TA_KICK`), on the fault-free
  pointer-mode recipe: the same hang;
- on the transfer (`probe_xfer` word 7 = 0, with word 11 = 0x4ffe): no PBE write.

## The stock init script (L)

The capture patch's init-script log was on `SGXRunScriptOnCore`, a multi-core path. The
single-core SGX530 runs `SGXRunScript`, so it had never logged. With the log on both, 35 writes,
in order:
- 0xa58 = 0;
- 0xacc = 0;
- 0x130 = 0x4000;
- 0x110 = 0;
- 0x13c = 0;
- 0xa7c = 0;
- 0xa80 = 0;
- 0xa74 = 0x003f8200;
- 0x4e8 = 0;
- 0xa78 = 0;
- 0xabc = 0x40;
- **0x804 = 0xffff**;
- 0xa00 = 0x7c000;
- the 16 code bases (0x800 apart: 512 KiB windows);
- 0xab8 (PDS exec base);
- 0xb30 = 0x100;
- 0xac4 (kicker);
- 0xa68, 0xa6c = 3, 0xa70 = 0x13d01 (event PDS);
- 0xa58 = 0x2204f00f.

Against ours, everything is the same, a layout address, or already tried (11-14 to 11-20),
except 0x804's low 16 bits.

## 0x804 = 0xffff (C)

Ours has 0x804 = 0 at idle; the stock's idle value is 0x1000ffff.
- **Written at init** (`probe_init`): our microkernel never takes the kick. The control block's
  heartbeat stays at 1 (init only), one CCB command is consumed, and the free list is never
  loaded. Our microkernel reads the host-written CCB, and 0x804's low bits look like cache
  enables (G).
- **Written just before `TA_KICK`** (scratch microkernel): the kick runs and 0x804 reads
  0x1000ffff at the hang, the stock's idle value. The TA still hangs and the DPM hands out no
  page.
- The transfer's end-of-tile store with it written at init: 0 of 3 land.

## 0xa64 = 0x4ffe, counted (C)

The end-of-tile store of 2026-11-27-0500, repeated:

| 0xa64 | the store lands | otherwise |
|---|---|---|
| 1 (ours) | 0 of 4 jobs | a fault at 0x08039000, every time |
| 0x4ffe (the stock's) | 3 of 8 | no fault, then the job timeout |

0x4ffe changes the end-of-tile task's behaviour and sometimes gives it working registers. It
isn't the whole fix.

## Where the TA stands

Everything observable is now matched to the stock or tried on ours, and the TA still never asks
the DPM for a page:
- every readable register at the kick;
- the stock init script's writes;
- the write-only 0x42c;
- the stream and task forms;
- the non-PB memory.

What's left, the stock microkernel's TA-start sequence, could only be read from its code. That's
off limits, so the TA is parked here.

## 04:50-05:00 UTC: the SGX535's DPM map, and the EVM init in pointer mode

Sources, all public:
- Intel's EMGD `psb_regs.h` (MIT), the SGX535's register header;
- Intel's psb kernel module (GPL), for the event flow.

Tagged **L**, in our words:
- 0x604/0x60c/0x614/0x61c are free lists, tail in b16-31 and head in b0-15. 0x608/0x610 and
  0x60c/0x614 are the host dalloc and alloc lists, beside the 3D (0x600/0x604) and TA
  (0x618/0x61c) ones.
- 0x630 is `DPM_STATE_CONTEXT_ID` (b0 load/store, b1 dalloc, b2 alloc).
- 0x680/0x684/0x688/0x690 load the 3D, TA, host and dhost free lists. Their done bits are
  EVENT_STATUS2 b1, b0, b2 and b3.
- **0x6a8 is `DPM_TA_EVM`, b0 INIT, done when EVENT_STATUS b22 (`DPM_INITEND`) sets.**
- 0x724/0x72c are page counts: TA in b16-31, total in b0-15.

On the 530 (C), the fault-free pointer-mode recipe with scratch microkernel switches:

| run | result |
|---|---|
| 0x6a8 = 1 just before `TA_KICK`, then wait for b22 | b22 sets (and stays set after the clear, as on 11-15). The TA still hangs, and the DPM hands out no page |
| the four free-list loads, then that | the host and dhost loads time out (those lists aren't programmed), then a requestor-b2 fault at 0x0f800000 |

The rest of the TA start in those sources matches what our microkernel already does:
- the PDS_INV1 and CSC invalidate, with its 0x138 wait;
- 0x804 b28, with its EVENT_STATUS b26 wait;
- 0xa08;
- the TA request base.

The TA stays parked.

A third source, an SGX535 driver whose kick sequence is a transcription of a disassembled closed
driver (its own header says so), wasn't used.
