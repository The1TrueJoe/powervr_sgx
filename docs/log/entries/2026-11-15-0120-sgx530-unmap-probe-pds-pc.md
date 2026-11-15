# SGX530 board: the 530 DMA control form, an MMU fault tracer, and where the PDS sits

**2026-11-14 23:57 to 2026-11-15 01:25 UTC. Our OMAP3630 test board, kernel 7.3.3, RAM
only, under lock. Each variant ended with a job timeout or fault, recovered by HWR; modules
unloaded after each. Microkernel image: sgx530 07af02bb (`INV_DONE_TA = 0x23`).**

All runs are the 64x64 kick (`powervr_sgx_frame -s 64x64`) with every 530 client form on
(`SGXGEN_VDM_REL TILE16 TERM530=0x00030003 STATE_BIT21 OUTSEL530 VDMINFO530 DRAW530
PRETERM530 MTE530 ISPB530 PDS_8WORD`), loaded with `allow_unisolated=1 edm_init=1
read_ukregs=1`.

## The 530's DOUTD control form doesn't unblock the TA

The 530 vendor stream's DOUTD control words read `(dwords - 1) | kind << 21 | secondary << 24`,
for example 0x00e00007, 0x01200009, 0x01000018 and 0x0140000a. The SGX545's form is
`kind << 22 | secondary << 25`. `SGXGEN_DMA530` (sgxgen 60294834) selects the 530 form.
`SGXGEN_CLEAR_KIND` (8288f8f9) sets the clear state block's kind; the vendor's 10-dword state
block uses 1.

All four variants hang the same way: the forms alone, the forms + DMA530, the forms + DMA530
+ CLEAR_KIND=1, and the minimal set + DMA530. CLKGATESTATUS 0x00111000, EVENT_STATUS 0,
EVENT_STATUS2 0x10, BIF_INT_STAT and BIF_FAULT 0, region headers and tail pointers 0. The
knobs stay, because the vendor stream uses those forms, but they aren't the blocker.

**EVENT_STATUS2 b4 is TRIG_TA on the 530** (the GPL 530 header; on the SGX545 it's b7). The
0x10 seen mid-hang in every run since 2026-11-14 07:00 is the TA trigger, still pending.

## BIF_MEM_REQ_STAT fluctuates, so the reads aren't stuck

The dump now reads `BIF_MEM_REQ_STAT` (0xca8, outstanding reads in b0-b7). With the GPL
530 header's name for it, it also reads `PDS_PC_BASE` (0xb2c), the PDS program counter. Each
was sampled three times, 0.4 s apart, mid-hang.

`BIF_MEM_REQ_STAT` moves between 0 and 2 from sample to sample. Reads are completing and
being reissued; nothing is stuck in the BIF. Something is polling memory. After a fault (below)
it stays at 2.

## The fault tracer: what the TA touches

`probe_unmap_va`/`probe_unmap_size` (powervr_sgx 5a734a30, off by default, SGX530 only)
unmap a range of the job's VM just before the job is queued. A read of the range should then
turn the silent stall into a BIF fault with an address.

| unmapped | what it holds | result |
|---|---|---|
| none | | timeout, no fault |
| 0x01841000 +0x5000 | VDM control stream | **fault at 0x01841000**, BIF_INT_STAT 0x00004010 |
| 0x01801000 +0x40000 | gen (TA command structures) | **fault at 0x01801000**, BIF_INT_STAT 0x00004010 |
| 0x0e001000 +0x10000 | PDS vertex heap: state programs, state blocks, vertex fetch | timeout, **no fault** |
| 0x0fc08000 +0x8000 | USSE vertex code | timeout, no fault |
| 0x0c400000 +0x10000 | TA data: region headers, tail pointers | timeout, no fault |

So faults are reported on this core, and the requestor that reads the VDM stream reports
them. The TA reaches its command and the VDM stream. Nothing reads the vertex USSE code or
writes TA data, as expected, since no vertex task ever runs.

## PDS_PC_BASE: the PDS sits on the first VDM state program

`PDS_PC_BASE` is 0 at idle and 0x002004c0 mid-hang, the same in every sample and every run
that got past the VDM stream. In the two faulted runs it stays 0. Read as dwords from
`PDS_EXEC_BASE` (0x0d800000), 0x002004c0 is 0x0e001300. That is the first state program
in the PDS vertex heap: its VDM word is 0x40080130, which is (0x0e001300 - 0x0d800000) >> 4
in the relative form. The VDM has decoded the state update and handed the PDS that program.

With the whole PDS vertex heap unmapped, the PDS still sits at 0x002004c0, and there is
**no fault**. That program's code fetch and its DOUTD of the state block both read that heap.
So either the PDS never gets as far as translating that address, or its faults aren't reported.

## The PDS's fetch never reaches the BIF

`SGXGEN_DUMP_PDS` places the programs: 0x0e001100-0x0e001200 hold the context's empty-state,
terminate and pre-terminate programs, and **0x0e0012c0** holds the clear state block's program.
Its data segment starts with the block's address 0x0e001260 and the DOUTD control 0x0160000b.
The PC's 0x0e001300 is that program's code, right after its four data rows (4 × 16 bytes
past 0x0e0012c0).

`SGXGEN_VDM_PDS_ADD=0x100000` (sgxgen 7056a124) adds 1 MiB to the address in that VDM word.
It then points at 0x0e1012c0, inside the PDS vertex heap's range but never mapped and never
written. `PDS_PC_BASE` follows: **0x002404c0**, exactly 0x40000 dwords (1 MiB) further,
which confirms the units. **Still no BIF fault**, and the same hang. No cache can hold an
address nothing ever wrote, so the PDS takes the VDM's program address and never issues its
code fetch to the BIF, or issues it without translation reporting a fault. The VDM's own
reads fault normally (above).

So the TA's stall is at the PDS: the vertex data master's first PDS program never starts.
The microkernel's own PDS programs (EDM, kernel code heap) do run. The difference sits
between the PDS and the memory interface for the vertex data master.

## 0xa74 isn't it

The vendor's init writes 0xa74 = 0x003f8200 and ours 0x00208200 (sgx530-vendor-setup.md §1).
`probe_a74` (powervr_sgx deaf101b, off by default, SGX530 only) overrides the init write.

| 0xa74 | result |
|---|---|
| 0x003f8200 (vendor) | the same stall: PDS PC 0x002004c0, no fault |
| 0x00108200 | the same stall |
| 0x00008200 | the microkernel never runs: "BIF invalidate not taken", the job is never sent, TA clock off |

So the field at b16 gates the EDM's PDS work (0 stops it), and 0x10-0x3f make no difference
to the vertex data master.

## Next lead

The VDM hands the PDS a valid program address. The PDS's fetch for the vertex data master
neither completes nor faults, whether the address is mapped or not. The EDM's PDS fetches
from the kernel code heap work. What to look at, in order:

1. **What lets the PDS issue vertex-DM fetches.** Look for a per-data-master enable or
   partition in the PDS or the memory arbiter that the EDM's path doesn't need. Candidates are
   the init-script registers the vendor writes and we don't, from sgx530-vendor-setup.md §1/§7:
   0xa6c/0xa70, 0xa58's b14 (0x2204f00f against our 0x2204b00f), the cache control value 2,
   and the clock-gate control 0x00222220.
   **0xa58 b14** is the cheapest of these to try: one bit, an event enable.
2. **The PDS cache's state.** 0x138 reads 0x110 mid-hang: `PDS_INV3`'s done bits (b4, b8) are
   set and never acknowledged. Something asked for a `PDS_INV3` invalidate whose completion
   nobody consumes. A stuck invalidate in the PDS cache would block its fills for every data
   master that misses.
3. **The BIF reset wait.** The GPL single-context reset waits for `BIF_MEM_REQ_STAT` to drain
   after INVALDC; ours doesn't (vendor-setup §8). Cheap to add, though nothing yet points at it.

## Follow-up 01:56 UTC: the vendor's 3-row PDS programs; the memory reads are idle traffic

**Tight data rows don't change it.** Every vendor VDM state program has 3 data rows
(48 bytes): its rows run to the last data word, with DS1[0] inside. Ours round each 8-word
block pair up to 4. `SGXGEN_PDS_TIGHT` (sgxgen 94d436f5) gives the vendor's count. The
clear block's program moves to 0x0e001200 with 3 rows. `PDS_PC_BASE` follows to 0x0020048c
(0x0e001230, its code start), and the hang is the same, with and without CLEAR_KIND=1.

**`BIF_MEM_REQ_STAT` is 1-2 at idle too**, sampled 1 s after the module loads, before any
job. The fluctuating reads mid-hang aren't the TA's. Something on the microkernel side reads
memory continuously while the GPU sits idle. Next: read the EDM-task progress pair (0xb10,
0xb1c; `read_ukregs`, 9ec87647) at idle and mid-hang, to see whether an EDM task stays
resident.
