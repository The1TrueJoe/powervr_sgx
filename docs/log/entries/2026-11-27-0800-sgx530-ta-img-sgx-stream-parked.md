# SGX530: the SGX535 driver's stream and state task against ours, and the TA parked

**2026-11-27, 05:00 to 05:10 UTC. The OMAP3630 test board, under lock, a fresh software reboot
before each run. Released clean.**

img-sgx (L) is the public GPL/MIT SGX535 driver Joseph cleared for reference on 2026-11-27.
Its host-driven TA runs a VDM stream whose state task feeds the MTE. Its stream, its state
PDS program and its state task were compared with ours word by word, read from its source
and its captured heap image. The facts below are in my own words. Its words were decoded
with our own encoding (`tools/gen/sgxgen.c`'s PDS macros, `sgx-disasm`). They stay static
facts until the SGX530 confirms them.

## The comparison (L, against ours)

| part | img-sgx (SGX535) | ours on the SGX530 (`tools/gen/sgxgen.c`) |
|---|---|---|
| state update, word 0 | 0x40000000 \| the state program's address >> 4 (absolute, 28-bit) | the same, the address relative to the PDS base (`SGXGEN_VDM_REL`) |
| state update, word 1 | 0x0c02a200 \| the attribute size in fours: data size 3, MTE emit, SD, pipe 1, one partition | rows << 26 \| 0x2a200 \| the size: the same fields (the 530 stock's form) |
| state PDS program | DOUTD (address, control); DOUTU (task word, 0, 0); HALT | by default the DDK's tst/bra/DOUTD ×2/DOUTU; `SGXGEN_STATE_1DMA` is img-sgx's shape |
| state DMA control | b31 \| (n−1) << 21 \| (n−1) | `SGXGEN_STATE_PRIMARY`: the 530 stock's form, (n−1) << 21 \| (n−1), no b31 |
| the task's DOUTU word | b21 set, low bits 0x24 | b21 set (`SGXGEN_STATE_BIT21`), with the 530 stock's own low-bit form |
| state task | `mov o0, pa0 [skipinv rcnt rpt=n]`, then `emit.t1 r0, #n, #0 [end f3_20]` | identical (`SGXGEN_EMIT530=state1`, `state_emit_<n>_530s`) |
| state block | a presence mask (0x4f41 in its capture), then the groups | a presence mask (0xd4c3 for the clear), then the groups |
| draw | 0x81400000 \| count (index lists 2 and 4/5), index address, 0, the vertex program >> 4, the record size word | `SGXGEN_DRAW535` gives the same shape |
| end of stream | a state update with b29 set (attribute size 1) whose task emits two words (0x2000 and the last tile), then 0xc0000000 | the same, plus an empty update and a pre-terminate unless left out (`SGXGEN_END_NOEMPTY`, no `PRETERM530`) |

So the only img-sgx word with no knob on our side is b31 of the state DMA control. The
SGX530 stock leaves it clear in its own TA state DMA (11-20, O).

## On the board (C)

Each run started from a fresh boot. They used the branch image with the 0x614 fix
(sgx530 ce7a1b60) and `probe_a74` = 0x05208200.

| run | stream and state task | result |
|---|---|---|
| pointer mode | the pointer-mode recipe + `STATE_1DMA`, `STATE_PRIMARY`, `DRAW535` | the hang. The VDM stops after the first block (0x334 = base + 8). 0x340/0x344 stay 0, `MTE_SIG1`/`SIG2` stay 0, no DPM page, no wait timeout |
| value mode, img-sgx's form | E without `PRETERM530`, + `STATE_1DMA`, `STATE_PRIMARY`, `DRAW535`, `EMIT530=state1`, `END_NOEMPTY` | **fault at 0x0000d000, requestor b1**: the state block's first word (0xd4c3) is taken as an address, as in every value-mode form since 11-15 |

b31 wasn't tried. The value-mode fault already shows that the DMA'd block reaches the task
and its first word reaches the reader as an address. A DMA control bit can't change what
the reader then does with that word.

## The TA is parked: where it stands

**Found and confirmed on the SGX530 (C):**
- the TA kick's invalidates, waits and DPM handshakes all complete:
  - `PDS_INV0` or `PDS_INV1` with the CSC;
  - the MADD invalidate;
  - the DPM state/control clears;
  - the OTPM clear;
  - the TA free-list load;
  - the DPM INIT once per boot.
- 0xa74 b22-27 (`DMS_CTRL`'s vertex and pixel partitions) must allow partitions, or no
  state task issues.
- the VDM reads the stream and the PDS runs the first state program. Its USSE task runs (the
  vertex service counter moves).
- whatever the state task's first output word is, it is fetched as an address (requestor
  b1). An address that resolves gets no further. The VDM then waits after that first
  block, and the TE never sees a word.

**Every register-side lead is spent:**
- the stock's observed values (11-20);
- every register TI's header names in the TA, TE, MTE, VDM and DPM blocks (11-27);
- the SGX535 driver's DPM init and TA kick order (11-27);
- its stream, state program and state task (here).

**What's left is on the USSE side:** what the state task's output must look like for this
core's MTE to take state words, not a pointer. Or, if it takes a pointer, what record it
expects there and what releases the VDM afterward. Neither the header nor the SGX535
driver says. They are the next things to find when the TA is picked up again:
- a stock SGX530 state task's output, observed at the MTE;
- an ISA description of the SGX530's emit forms.

The 3D-only path (transfers, the full-screen clear from pixel-task stores) is unaffected
and stays the SGX530's working output.
