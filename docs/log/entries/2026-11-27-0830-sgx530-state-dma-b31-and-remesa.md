# SGX530: b31 in the state DMA control, and reMESA's notes on the state task

**2026-11-27, 05:05 to 05:15 UTC. The OMAP3630 test board, under lock, a fresh software reboot
before each run. Released clean.** The TA stays parked
([2026-11-27-0800](2026-11-27-0800-sgx530-ta-img-sgx-stream-parked.md)).

## b31 of the state DMA control (C)

`SGXGEN_STATE_DMA_B31` (new, off by default) sets b31 in the state program's DMA control
word, as the public SGX535 driver does. The SGX530 stock leaves it clear. The frame tool
was rebuilt with the knob. Each run used the branch image (sgx530 ce7a1b60) and img-sgx's
stream forms (`STATE_1DMA`, `STATE_PRIMARY`, `DRAW535`).

| run | result |
|---|---|
| control: the rebuilt tool, pointer mode, no b31 | the hang as before. The VDM stops after the first block (0x334 = base + 8), the TE signatures stay 0, and the job times out at 2 s |
| pointer mode, b31 | the same: 0x334 = base + 8, the TE signatures 0, the timeout |
| value mode (img-sgx's form, `EMIT530=state1`, no pre-terminate, no empty update), b31 | **fault at 0x0000d000, requestor b1**, as without b31 |

The frame tool reported "done" on some timed-out runs and "NOT done" on others. That is only
whether the reset signaled the fence before the tool's 2 s wait ended. It isn't a
difference.

## reMESA (L)

reMESA is a public SGX535 (Poulsbo) driver project, cleared for reference by Joseph on
2026-11-27. By its own provenance record, its facts come from several kinds of source:
- public GPL/MIT sources;
- register captures and experiments on an SGX535;
- disassembly of the closed Poulsbo user-mode library.

I read its notes on the state task, the MTE and TA completion (`cold-dpm-origin.md`,
`poulsbo-interrupts.md`, `registers.md`, the phase 9 TA-state and program-dependency
notes):
- **`cold-dpm-origin.md`** is the project's own buffer bookkeeping for zeroed DPM pools. It
  holds no hardware fact about the DPM.
- **`poulsbo-interrupts.md`** describes the Poulsbo interrupt path from the public kernel
  sources. On the SGX535, the BIF fault requestor bits are CACHE b1, TA b2, VDM b3, 2D b4,
  PBE b5, TSP b6, ISP b7, USSE/PDS b8, HOST b9. On the SGX530 we measured VDM stream reads
  at b4, USSE code fetches at b2 and PBE writes at b6. That is the SGX535's list shifted up
  by one, which would make the 530's b1 the unit below the cache, with no SGX535
  counterpart (G).
- **The TA state reference** (from its disassembly of the closed library, so a static fact)
  gives the state update's two words. Word 0 is 0x40000000 | the PDS address >> 4, with b29
  set when the state block's bit 13 (terminate) is set. Word 1 is 0x0002a200 | the PDS data
  dwords << 24 | the state dwords in fours. That is the form we already emit.
- **The state task's emit and how the MTE consumes it** are UNKNOWN in its own dependency
  table. It cites only the architecture patent there.

Nothing in reMESA says how the state task's output must look for the MTE. So no new test
follows from it.

## Where it stands

Both leads are null. The TA stays parked on the question 2026-11-27-0800 ends with:
- what the SGX530's state task must hand the MTE;
- or, if the MTE takes a pointer, what record it expects there and what releases the VDM
  afterward.
