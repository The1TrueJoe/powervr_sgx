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
