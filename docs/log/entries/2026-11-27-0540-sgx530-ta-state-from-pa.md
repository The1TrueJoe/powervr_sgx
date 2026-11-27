# SGX530: a state task that emits from pa0 still hands the MTE o0 as an address

2026-11-27 05:35 UTC, an OMAP3630 board, RAM only, our own code. A follow-up to
[2026-11-27-0500](2026-11-27-0500-sgx530-stock-transfer-command-0xa64.md): in a pixel task only an
o-bank write is read as an address, so a TA state task that never writes the o bank might hand
the MTE its words as data.

`SGXGEN_EMIT530=statep` (new, off by default) makes each state task one instruction,
`emit.t1 pa0, #n, #0 [end f3_20]`, emitting the words the PDS DMA'd into pa0.. with no o-bank
write. Same frame, knobs and microkernel image (the sgx530 branch, 0x45 3D invalidate mask) as
the control, with `probe_a74=0x05208200`:

| run | result |
|---|---|
| control (data mode: `mov oN, paN`, then the SGX545's emits) | fault at **0x0000d000**, requestor b1: the state header 0xd4c3 read as an address, as on 2026-11-15 |
| `SGXGEN_EMIT530=statep` | fault at **0x00000000**, requestor b1 |

**C:** the MTE reads the task's o0 as the state block's address whatever the emit's source
operand is: with nothing written to o0 it reads at 0. So on this core the state reaches the MTE
through a pointer in o0, the "pointer mode" of 2026-11-15 to 11-20. That mode runs the stream
fault-free but the TA never finishes. The o-bank finding from the 3D side doesn't open a data
mode for the TA.

## 03:42-03:50 UTC: what a stock frame writes in GPU memory (O)

To find a structure the stock TA reads or writes that ours doesn't set up, one more
observation-only capture boot (Joseph's OK, 2026-11-27) diffed the stock render's GPU memory:
- **Before:** the capture kernel's snapshot at the first TA kick. It holds every mapped page but
  the parameter buffer and the USSE code heaps.
- **After:** the same physical pages read back while the render process was held alive, with
  our own reader.

Only words that changed were looked at, so unchanged program pages were never examined. The
diff stays with the captures, outside the repo. 424 pages, 4390 changed words:

- the region headers, tail pointers and region array (the TA data block), as 11-20 described;
- the second TA kick's VDM words and its TA command. The client writes those after the first
  kick, and the command's tail holds the TE words we already match;
- the microkernel's bookkeeping in kernel memory (CCB offsets, counters, context pointers);
- four pages of a render buffer.

Nothing the TA hardware writes turned up outside the parameter buffer. The parameter buffer
itself isn't in the snapshot. So the stock frame's memory shows no TA-side structure that ours
lacks, in the ranges this method covers.

## 04:00-04:30 UTC: every readable register the stock TA kick leaves, on ours (C)

A whole-window diff was taken:
- **The stock TA:** the 11-20 samples (idle values, then each change) replayed to the second TA
  kick.
- **Ours:** the 11-20 hang sweep (958 offsets).

41 offsets differ. Most are layout addresses (code bases, PDS and event bases, the TE and VDM
bases). The rest, on the fault-free pointer-mode recipe:

| run | result |
|---|---|
| the stock's PB size (`SGXGEN_PB_PAGES=0x3bb`, 0x3dd pages: every page number fits 12 bits; head 0x03dc0000) | hang, the DPM hands out no page |
| ZLS threshold 0x624 = 0x200 just before `TA_KICK` (scratch microkernel switch `UK530_POKE2`) | hang |
| all three thresholds at the stock's, 0x3ed / 0x200 / 0x103ed, with the stock's PB size | hang (they hold) |
| 0xa64 = 0x4ffe (it holds through the TA) | hang |

0x26c (written by the hardware during a stock TA, 0x44a) stays 0 on ours. 0xa6c (our event PDS
program's data size) is ours to choose and wasn't changed.

**So no register a host can read separates the stock TA from ours**, between 11-20 and today.
The stock frame's memory shows no TA-side structure ours lacks either. Rebuilding our state
block in the stock's layout was considered and not done. The hang doesn't depend on the
record's contents (11-19), and a stream of only the end word hangs the same way, so the state
block can't be what ends the pass. What's left can't be seen by observation alone:
write-only registers, and the order and timing of the stock microkernel's TA start.
