# SGX530 board: the vendor's VDM and state-block words don't unstall the TA

**2026-11-14 21:42 UTC. Our OMAP3630 test board, kernel 7.3.3. Under lock. Clean, recovered
by HWR; modules unloaded.**

A word-by-word read of the vendor stack's 64x64 stream (kick snapshot) against sgxgen's shows
these differences. Each one is now an sgxgen knob:

| knob | vendor form | sgxgen's (SGX545) form |
|---|---|---|
| `SGXGEN_VDMINFO530` | info low bits: state DMA `0x2a200 \| dwords`, DOUTU-only `0x16100`, terminate `0x2a201` | `0x2022400 + counter \| dwords`, `0xe100`, `0x2022401` |
| `SGXGEN_DRAW530` | draw word 0 b24 set, batch 8 (`0x70000000`), batching field 2 at 8 vertices per task | b24 clear, batch 16, field 3 |
| `SGXGEN_PRETERM530` | a type 4 program, `DOUTA (0x800, 0), (0, 0x100)` + task, before the terminate (info `0x2c201`) | none |
| `SGXGEN_OUTSEL530` | output select: output dwords at **b24** (clear `0x08001800`, draw `0x04001000`) | at b16 |
| `SGXGEN_MTE530` | MTE control: clear `0x00010000`, draw `0x00088000` | `0x40`, `mte_ctrl()` |
| `SGXGEN_ISPB530` | draw ISPB without the line width (`0x000000f0`) | `0x000010f0` |

The vendor does emit a region clip. It goes in a separate first state block (header `0xd481`,
clip `80000003 00000003`), and later blocks (`0xd443`, draws `0xdf43`) leave it out.

On top of the relative VDM addresses, 16-pixel tiles, two-DOUTA terminate and state-emit
bit 21, each knob was tried alone and then all six together. Every run gives the same hang:
region headers and tail pointers 0, TA clock on, wait 0xd x2. One run printed `render done`,
but only after the 2 s timeout; its coredump shows the same hang (HWR signalled the fence).
