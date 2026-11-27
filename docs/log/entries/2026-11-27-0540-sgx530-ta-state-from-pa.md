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
