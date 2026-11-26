# SGX530 board: pointer mode taken as native, and where its faults come from

2026-11-19 21:45-22:10 UTC, an OMAP3630 board, RAM only. A restart of the first-frame
work with no vendor code: our own frames, black-box observation through the BIF fault
registers and `PDS_PC_BASE`, and only register offsets already in our docs. Frame tool from
`wip/sgx530-frame5` (sgx530-kick-frame CI), modules from 864065ca (powervr-sgx-omap3630,
Linux 7.3.3), microkernel `sgx530_inv23`, `probe_a74=0x05208200`. Base environment as in
the 2026-11-15 entry (clear only); "pointer mode" is `SGXGEN_STATE_PTR=1
SGXGEN_EMIT530=state`. Board script `batch7.sh`.

## Why pointer mode is the lead, not a side path

The 2026-11-15/16 slots showed the MTE takes the state task's first output word as an
address in every encoding tried, including a `nop` ending and the header itself (0xd4c3
faults at 0xd000). So this core takes its state record by address. Emitting the record's
real address (`STATE_PTR`) is the mode to work in, and its next fault (requestor b2 at
`PDS_EXEC_BASE` + 0, 0x0d800000) is the thing to explain.

An unlogged run from 2026-11-15 23:31 had already shown that mapping that page
(`SGXGEN_MAP_PDS0=0`) removes the b2 fault: the job then ends in a requestor-b1 fault at
address 0.

## The b2 read at exec base + 0 (C)

| variant (pointer mode) | first fault |
|---|---|
| base | 0x0d800000, b2 |
| page at exec base mapped, filled with 0 | 0x00000000, b1; PDS PC 0x001d53a8 |
| the same page filled with PDS `HALT` (0xaf000000) | the same, PC 0x001d53a8 |
| record = header + 63 × 0x00012340, header one bit at a time (b0-b31) | 0x0d800000 b2, except b12 alone and b24 alone: 0, b1 |
| header 0xd4c3 or 0xffff, same marker words | 0x0d800000, b2 |

The fill doesn't matter, so the access isn't a program fetch but a data read. The marker
words would put any address taken from the record in its own unmapped page (0x0d923000,
0x0d812000, 0x0d848000 or 0x00012000 by unit); none appeared, so the read's address does
not come from the record.

## It comes after the stream's end (C)

| variant (pointer mode) | first fault |
|---|---|
| no draw in the clear (`SGXGEN_CLEAR_NODRAW`) | 0x0d800000, b2 |
| the draw's vertex program + 1 MiB (`SGXGEN_DRAW_PDS_ADD`) | 0x0e101000, b2 |
| no draw; the empty secondary, the pre-terminate or the terminate program + 1 MiB (`SGXGEN_END_ADD`); the state program + 1 MiB | 0x0e101000, b2, each |
| no draw, no pre-terminate | 0x00000000, b1 |
| no draw, the SGX545 terminate form (with or without the pre-terminate) | 0x00000000, b1 |
| no draw, the end tasks' first words set to unmapped addresses (`SGXGEN_PRETERM_A0` 0x00300800, `SGXGEN_TERM_A0` 0x00502000) | unchanged |
| no draw, pre-terminate info word 0x2c200, 0x2a201, 0x2c202, 0x16101, 0x2c281, 0x2c301 (ours 0x2c201) | 0x00000000, b1 |
| no draw, pre-terminate second DOUTA word (`SGXGEN_PRETERM_A1`) 0x40, 0x1000, 0x300000 | 0x00000000, b1 |
| the same, 0 (ours) or 0x00803000 | 0x0d800000, b2 |

Every program in the stream is fetched before the b2 read (moving any of them moves the
first fault to it). The read happens only when the pre-terminate program is admitted
(its exact info word) and hands the MTE a second word of 0 or 0x803000. It's a side
effect of the pre-terminate, not the end of the pass. The end tasks' first words are not
taken as addresses.

**Normal mode without the draw:** the 0xd000 fault becomes a b1 fault at 0. The MTE takes
the header as an address only once a primitive arrives.

## Where it stands

With the record taken by address and the pre-terminate out of the way, every
configuration ends in the same **requestor-b1 fault at address 0** after the terminate.
That looks like an end-of-pass write through a base register the 530 run leaves at 0.
Next: read the SGX545 TA kick's address registers (our record of our own microkernel's
writes, `docs/log/data/2026-10-28-ukernel-impl/ours-registers.txt` on the sgx530 branch)
on the 530 at that hang, with the new read-only module parameter `probe_read`.

## 22:30-22:40 UTC: the registers at the b1-at-0 hang (read only)

New module parameters: `probe_read` (a839a5b6; offsets added to the read_ukregs hang dump,
read only) and `probe_pb_skip` (912a985d; starts the free list N pages into the PB). All
offsets read are named in our own `docs/UKERNEL.md` (TA kick and PB tables) and in our record
of our SGX545 microkernel's writes (`docs/log/data/2026-10-28-ukernel-impl/ours-registers.txt`
on the sgx530 branch). Pointer mode, no draw, no pre-terminate:

| register | SGX530 at the hang | SGX545 (our microkernel, TA kick) |
|---|---|---|
| 0x204 TE_AA | 0 | 0x20000000 (a flag) |
| 0x208 / 0x20c / 0x210 / 0x214 | 0x01004004 / 0x01004004 / 0x00003003 / 0x10 | (frame-dependent) |
| 0x21c region headers / 0x220 tail pointers | 0x0c405000 / 0x0c400000 | 0x20005140 / 0x20000000 |
| 0x234 / 0x62c / 0x634 | 0x0c402000 / 0x0c406000 / 0x0c401000 | 0x20002000 / 0x20008140 / 0x20001000 |
| 0x238 VDM base | 0x01841000 | 0x3e040000 |
| 0x618 free list / 0x61c head / 0x648 end | 0x09422000 / 0x10210000 / 0x10200000 | 0x10422000 / 0x04210000 / 0x04200000 |
| 0x620 TA / 0x624 ZLS / 0x628 global | 0x1032 / 0x1000 / 0x11000 | 0x432 / 0x400 / 0x10400 |
| 0x668 PDS threshold | **0** (the kernel's descriptor says 0xf00) | 0x300 |
| 0x700 live head / 0x724 / 0x72c | 0x10210000 / 0 / 0 | |
| others in 0x600-0x67c, 0x700-0x74c | 0, except 0x614 = 0x2000, 0x644 = 0x3db8, 0x710 = 0x1020, 0x738 = 1 | |

Every address register holds a real parameter-heap address, and the PB thresholds follow
the SGX545 formulas for this PB (0x1022 pages). The DPM hands out no page before the fault
(the live head is unchanged). 0x668 isn't loaded on this path.

**The fault at 0 is not the DPM's first page (C):** with `probe_pb_skip` = 0x10 or 0x100
the live head's low half moves to 0x10 / 0x100, and the fault stays at absolute 0 (b1). The
same skip with the draw in normal mode keeps the 0xd000 fault.

## Where it stands (22:40 UTC)

Pointer mode reaches the end of the pass. Every configuration then ends in a requestor-b1
access at absolute 0 that isn't the record, a stream program, an end task's word, the
DPM's first page, or any of the TA's address registers above. Next options, no vendor code:
1. A kernel probe that maps a scratch page at device VA 0 in the client's VM (as
   `SGXGEN_MAP_PDS0` did for the PDS base). The run then goes past the fault, and the
   page's contents afterwards show whether the access is a write and what it writes.
2. Load 0x668 (the PDS threshold) on the SGX530 path of the open microkernel, as the SGX545 does.

## 22:48-22:58 UTC: the read at absolute 0 is the empty update's record; every update hands the MTE a pointer

New: module parameter `probe_va0` (d184dc6c; a page at device address 0 in each SGX530
client VM, filled with a pattern, changed words logged when the VM goes), and sgxgen
`SGXGEN_END_NOEMPTY` (61c15b9e; the stream without its empty secondary update) and
`SGXGEN_END_PTR` (f47cf381; the pre-terminate and terminate tasks hand the MTE the address of
a record holding their two words). Pointer mode as above.

| variant | result |
|---|---|
| VA 0 mapped (0xa5a5a5a5), no draw, no pre-terminate | the b1 fault is gone; next, the b2 read at the PDS exec base (fault 0x0d800000) |
| VA 0 and the PDS exec base page both mapped, with or without the draw | no fault, the TA times out (2 s): no region headers or tail pointers, no DPM page handed out. The VA 0 page is unchanged: **the access at 0 is a read** |
| 0x668 written 0xf00 by the host (`probe_init`) and loaded by the microkernel | reads back 0: **the SGX530 has no read/write register at 0x668 (C)** |
| no empty secondary update (`END_NOEMPTY`), no draw, no pre-terminate | the b1 fault at 0 is gone; b2 at the PDS exec base |
| the same, with the pre-terminate's first word 0x00300800 | **b1 fault at 0x00300000** |
| the same, terminate's first word 0x00502000 | b2 at the PDS exec base |
| `END_NOEMPTY` + `END_PTR` (pointer mode for every update), with or without the draw or the pre-terminate | b2 at the PDS exec base only |
| the same with the PDS exec base page mapped | no fault, the TA times out |
| the same, record markers in the pre-terminate's second word or the terminate's | b2 at the PDS exec base (one run without a fault did not repeat) |

**Every MTE-bound update's first output word is the address of its record (C):** the
clear's state (earlier), the empty secondary update (its task emits nothing, so 0, and the
MTE reads a record at absolute 0: that was the b1 fault at 0), and the pre-terminate (its
0x800 header, read as a record address at 0x800; moving it moves the fault). The SGX530
needs every state-type update in pointer form, or none in the stream.

**Correction to the 21:45-22:10 tables:** the b1 and b2 accesses both happen and race, and
the BIF reports whichever faults first. So the earlier "the pre-terminate's info / mode word
removes the b2 read" rows show which fault won, not a cause. With all updates in pointer
form, the b2 read at `PDS_EXEC_BASE` + 0 happens in every configuration tried.

## Where it stands (22:58 UTC)

In pointer mode everywhere, the one fault left is a requestor-b2 read at `PDS_EXEC_BASE` + 0.
With that page mapped, nothing faults and the TA hangs before the DPM hands out a page.
Next: find what issues that read. It isn't the record's PDS words (relative already, and
marker records don't move it). Candidates are a PDS address field this core takes from a
part of the record or command we haven't varied, or a register our docs name for event or
state programs. Every source moved so far moved a fault first, which hides a later read, so
the next probes should map the moved address rather than unmap it.

## 23:00-23:10 UTC: the read at the PDS exec base is the SGX545-form tasks' `emit.t3` (C)

New sgxgen knobs: `SGXGEN_STATE_1DMA`, `SGXGEN_TERM_NOTASK` (03fa3dea), `SGXGEN_SKIP_CLEAR`
(cd50456c). Pointer mode everywhere (`STATE_PTR`, `END_NOEMPTY`, `END_PTR`), no
pre-terminate unless noted. Because the b2 read was intermittent, each variant ran 2-5 times.

| variant | b2 fault at 0x0d800000 |
|---|---|
| base, no draw | 2 of 3 (plus earlier runs: most) |
| the state program as one unconditional DOUTD + DOUTU (`STATE_1DMA`) | 2 of 2 |
| the terminate without its task (`TERM_NOTASK`) | 2 of 3 |
| the clear's record moved (`CLEAR_PDSADD` 1 MiB), all markers, or header 0 | 6 of 6 |
| the event-program registers (0xa58 0x2204b00f, 0xa68 0x014042c0, 0xa6c 3, 0xa70 0x13d01) | the same in fault and no-fault runs; 0xa68 is the kernel's event program |
| no clear at all, only the end (`SKIP_CLEAR`) | 3 of 3 |
| the same without the terminate's task | **0 of 2** |
| **the state tasks' one-emit twins** (`SGXGEN_EMIT530=state1`: one `emit.t1` ending, no `emit.t3`): no draw, draw, end only, with the pre-terminate | **0 of 5** (a sixth run lost to the link) |

The read comes from the MTE-bound USSE tasks (the clear's state task and the terminate's),
and goes away when they don't end with the SGX545's `emit.t3`. That matches the 2026-11-16
emit sweep, where a final `emit.t3 r0, #0, #0` gave this fault and `emit.t1` didn't. On
the SGX530 `emit.t3` starts a PDS fetch (at its operand, here 0 → `PDS_EXEC_BASE` + 0).
**The SGX530 state tasks must end with the one-emit form** (G for what t3 means, C for
the effect).

**The fault-free hang.** With the one-emit tasks nothing faults, but the TA never
finishes. In every run the PDS PC is 0x0020046c = 0x0e0011b0, the terminate program's
first instruction (its data at 0x0e001180, 3 rows; `SGXGEN_DUMP_PDS`). It's the same with
only the end of the stream. The DPM hands out no page and no region headers or tail
pointers are written, with or without the draw.

## Where it stands (23:10 UTC)

The SGX530 stream now runs to its terminate without a fault: every MTE-bound update hands
over a record address, there's no empty update, and the state tasks use the one-emit
ending. The pass doesn't end. Next, all without vendor code:
- whether the terminate program runs at all, or the PDS waits in front of it (move it
  within mapped memory and see whether the PC follows; give it a HALT-only form);
- the terminate record's form for this core: header 0x2000 + the last tile (3, 3);
- whether the MTE accepts the clear's state at all: with the draw, no primitive reaches the
  DPM.

## 23:10-23:20 UTC: the terminate runs; the hang doesn't depend on any record's contents

Pointer mode everywhere, one-emit tasks (`SGXGEN_EMIT530=state1`), no empty update.

| variant | result |
|---|---|
| terminate's first word raw 0x00502000 (no `END_PTR`), with the clear or end only | **b1 fault at 0x00502000**, 3 of 3: the terminate's task runs and the MTE reads its record (C) |
| the clear's record pointer + 1 MiB | b1 fault at 0x0e101000: the MTE reads the clear's record (C) |
| the draw with the empty vertex task | no fault, hang |
| end only, the terminate record's last-tile word 0x00030003, 0x3003, 0, 0x0016000e, 0x33, 0x80030003 | no fault, hang; no event, no tail pointers |
| end only, the terminate record's header 0, 0x1000, 0x4000, 0x8000, 0x10000, 0x20000000, 0x80000000 | the same |

The PDS PC resting at the terminate's first instruction is just the last program it
launched: the terminate does run. The MTE reads every record it's handed, and the pass
still doesn't end whatever the terminate record holds. In every hang the DPM's live head
(0x700) is unmoved and 0x724/0x72c are 0: no page was ever handed out.

## Where it stands (23:20 UTC)

Recipe so far (SGX530, open microkernel `sgx530_inv23`):
- 0xa74 = 0x05208200;
- every MTE-bound update hands the MTE a record address (`STATE_PTR`, `END_PTR`);
- no empty secondary update (`END_NOEMPTY`);
- state tasks end with one `emit.t1`, not the SGX545's `emit.t3` (`EMIT530=state1`).

The stream then runs fault-free and the MTE reads every record, but the TA never ends and
the DPM never hands out a page. Open:
- whether the MTE takes the records at all past reading them: the content sweeps change
  nothing, as if it reads and then waits;
- what the DPM needs on this core to allocate. The free list is loaded, and the TA kick's
  DPM clear (0x694/0x698 = 2, confirmed earlier) runs. 0x630 and 0x65c are 0, as on a first
  kick in slot 0;
- the TE layout registers on this core (0x208/0x20c = 0x01004004, 0x210 = 0x3003, 0x214 = 0x10).

## 23:42-23:52 UTC: the DPM is idle; the MTE's input has address slots at o0 and o6

Same recipe (pointer mode everywhere, one-emit tasks, no empty update, no pre-terminate).
New sgxgen knobs `SGXGEN_STATE_PTR_N` (f9c07c9b; the k-word state task over the 1-word
pointer DMA) and `SGXGEN_STATE_PTR_WORDS` (204f558e; more output words after the pointer,
DMA'd with it, and the matching one-emit task).

**Lead (a), the DPM: not the blocker (C).** The documented PB/DPM registers (0x600-0x668,
0x680-0x69c, 0x6a8, 0x700-0x74c as named in UKERNEL.md) are identical at the hang for end
only, the clear without its draw, and the clear with its draw. The free list is loaded, the
live head is unmoved, the counters are 0 and no load or store is in flight. The TA stalls
before it asks the DPM for anything.

**The MTE's input words (C).** No faults in any of these runs except at the listed addresses.

| output words after the pointer | result |
|---|---|
| the one-emit task for 12 / 16 words over a 1-word DMA (`STATE_PTR_N`) | b1 fault at 0 / at 0x02492000 (leftover attribute contents taken as an address); 2 words: no fault |
| o1 = 0x00502000, 0, 0x00602000 or 0xc (`STATE_PTR_W1`) | no fault: o1 isn't an address |
| o1..o11 = i MiB + 0x1000 (2 runs), or o1..o7 | **b1 fault at 0x00601000: o6 is an address** |
| o1..o3 | no fault |
| only o6 set (7 words) | fault at its value; the same value in o5 (6 words): no fault |
| o6 mapped (0x0e001000), o7..o15 unmapped | no fault: no later word is dereferenced |
| o6 = 0x0e011000 / 0x0e010ffc | fault at 0x0e011000 (the end of the vertex PDS heap's mapping) |
| o6 = 0x0e010ff0, 0x0e010fc0, 0x0e010f00, 0x0e010c00 | no fault |

So on the SGX530 the MTE dereferences the state task's output words **o0 and o6**, and only
those. At o6 it reads more than 4 and at most 16 bytes. In every case the TA then hangs
without a fault, as before. The SGX545 record we've been placing at o0 has its
region-clip word at index 6, so the 530's input isn't that record. It's a fixed layout
with two pointer slots, whose other words and pointed-to blocks are still unknown.

## Where it stands (23:52 UTC)

Next, black box with our own frames:
- how much the MTE reads at o0, the same way (needs the pointer placed at a page end);
- what each of words 1-5 and 7+ does: sweep them one at a time for a change in the hang
  (a fault, an event, a tail pointer or region header written), with o0 and o6 both mapped;
- what the two pointed-to blocks hold: vary their contents (the record at o0 changed nothing
  so far, but o6 has never pointed at anything chosen).

## 2026-11-20 00:20-00:25 UTC: no input word or terminate emit form ends the pass

Same recipe. `SGXGEN_STATE_PTR_WORDS`, o6 = 0x0e001000 (mapped), 12 words:

- **Words 1-5 and 7-11 one at a time = 0xffffffff:** all the same no-fault hang, the PC at the
  terminate, no event, no tail pointers or region headers (word 10's run lost to the link).
  The hang doesn't depend on the MTE's input words.

**The terminate task's final emit** (end-only stream, where it's the only one-emit task, via
`SGXGEN_STATE_LAST`; fields per ISA.md: target h14-15, f10 h10, f12 h12, f13 h13, n h0-1 + h9,
end h18; low word 0xa0200100 = `r0, #2, #0` with f3_20):

| final emit | result |
|---|---|
| t1 (0xfb274000, ours), t0 | hang, no fault |
| **t2 (0xfb278000), t3 (0xfb27c000), t2 + f13** | **b2 fault at `PDS_EXEC_BASE` + 0** |
| t1 + f13 ("emit and cut" in ISA.md, unconfirmed), t1 + f12, t1 + f10 | hang |
| t1 with n = 1, 2, 3, 4 | hang |

On the SGX530, emit targets t2 and t3 start a PDS fetch at their operand (C: both, with
r0 = 0, read exec base + 0). That's why the SGX545's `emit.t3` ending faulted. t0 and t1 go
to the MTE. No target, flag or n makes the TA register a terminate: `EVENT_STATUS` b12 (TA
terminate) and b13 (TA finished), at the SGX545's positions on this core per sgx530.md,
never appear.

## Where it stands (00:25 UTC)

With our own frames, the SGX530's TA runs its whole stream fault-free and never ends. Every
part varied has been ruled out as the thing that ends it:
- the terminate's record (header and last-tile word);
- the MTE's input words, including the two pointer slots o0 and o6;
- the terminate task's final emit (target, flags, n);
- the DPM, which stays idle.

The cheap black-box probes on the end of the pass are used up. What's left needs a new idea
about how this core marks the end of a TA pass. The VDM-side terminate (its type-6 state
word and info form, and the 0xc0000000 word) is the one part of the end not varied
on this recipe.

## 2026-11-20 00:32-00:37 UTC: the VDM-side end, with Rogue as the hypothesis source; parked

New sgxgen knobs (74c34e13): `SGXGEN_VDM_EXTRA` (raw VDM words before the terminate),
`SGXGEN_END_NOTERM`, `SGXGEN_TERM_INFO`, `SGXGEN_VDM_ENDWORD`, `SGXGEN_TERM_W2`.
The hypotheses come from Mesa's Rogue `csbgen` (MIT): a PPP terminate block is a header with
`pres_terminate` (b25), then TERMINATE0 (clip_right b18-26, clip_bottom b0-8, in 32-px blocks)
and TERMINATE1 (clip_left b23-31, render target b0-10), reached by a VDM PPP_STATE_UPDATE
(block type 0: word count, address) before STREAM_TERMINATE (type 6). The SGX's 0xc0000000
is also block type 6. End-only stream, same recipe:

| variant | result |
|---|---|
| base | hang, no fault |
| the terminate's VDM info word 0x2a201 with each of bits 8-17 flipped | hang |
| the end word 0xc0000001 | hang |
| Rogue-style terminate records: header 0x02000000 + 0x00040001 + 0; header 0x02000000 + 0x000c0003 + 0; header 0x02002000 + 0x00040001 + 0 | hang |
| no terminate program: the stream is only 0xc0000000 (2 runs) | **hang**, PDS PC 0 |
| a raw VDM word 0x00502000 (block type 0) before the terminate | fault at **0x01408000** (= 0x00502000 << 2), requestor b4 (VDM) |
| raw VDM words 0x00000300, 0x00502000 | fault at 0 (0x300 << 2), requestor b4 |

A type-0 VDM word is an address in 4-byte units that the VDM fetches (C), most likely a
stream link (G). Nothing at the VDM side ends the pass. **Even a stream holding only the end
word hangs** (C), so the end of the pass isn't decided by the stream's contents.

**Parked here.** The recipe and the ruled-out list are in `docs/porting/cores/sgx530.md`
("The TA stream"). The next direction, when picked up, is the TA kick itself: compare it
against our own SGX545 microkernel's, since even an empty stream doesn't complete.

## 2026-11-20 00:39-00:46 UTC: the TA kick against our SGX545 microkernel's

Un-parked to compare the kick itself. Our open microkernel's source (`ukernel/ukernel.py`,
`cores/sgx530.py` on the sgx530 branch, the inv23 image's) and the hang dumps. Fault-free
end-only stream, same recipe.

- **The microkernel side is healthy (C).** The control block (`ta3dctl` in the dump, word 0 =
  its own address) shows no bounded wait timed out (+0xa0/+0xa4 = 0), the heartbeat at 3
  events, and the TA-in-flight flag (word 0x36) = 1. It kicked the TA and waits for
  `EVENT_STATUS` b13 (TA finished, 0x2000 in its dispatch), which the hardware never raises.
  `CLKGATESTATUS` shows the TA clock active (b12) through the hang.
  `BIF_MEM_REQ_STAT` b1 toggles at idle too, so it's the microkernel's own polling, not a stuck TA request.
- **The SGX545's TA command values (`ta_revert`, b0 tile width, b1 screen packing, b2 TE AA,
  b3 VDM control; 1, 2, 4, 8, 3, 15) and VDM control 0xa2 / 0x82:** the same hang.
- **0xa74 with each of bits 3-26 flipped** from 0x05208200 (b22's run lost to the link): the
  same hang. Bits 27-31 are the TA-flags field the microkernel rewrites at every kick.
- **Every register the kick writes from the SGX545's TA table exists on the 530 (C):** read
  at idle before the kick (all 0) and at the hang, 0x204-0x248 hold what was written (0x208/0x20c
  0x01004004, 0x210 0x3003, 0x214 0x10, 0x218 0x01000000, 0x21c/0x220/0x234 the region,
  tail and control arrays, 0x230 0x80, 0x238 the stream base 0x01841000, 0x23c 0x1a2,
  0x240/0x244 1e-6, 0x248 0x0003f03f). The ones the table zeroes (0x2a8-0x374, 0x74c, 0xa04,
  0xaa0) read 0 at idle too, so no default is lost.

So the kick programs every register the SGX545 needs, the stream runs, and the TA never
ends. What's left is a register the SGX530's TA needs that the SGX545's kick never writes.
Finding one by experiment means reading offsets our docs don't name, outside this work's
rules, so it waits for the coordinator.

## 2026-11-20 00:47-00:51 UTC: guarded read sweep of 0x200-0x3fc and 0x600-0x7fc

Approved by the coordinator under the standing go for this board (any SGX register read or
write; the named-registers rule is for the CE5300 boards). Read only, through the driver
(`probe_read`, 32 offsets per run, read only while the GPU is powered), at idle before the
kick and in the hang dump. Fault-free end-only stream. No wedge; every chunk read cleanly.

Registers that are nonzero at idle or change by the hang, apart from those the kick writes
(listed in the 00:39 entry):

| offset | idle | hang | note |
|---|---|---|---|
| 0x228, 0x22c | 0x1ff | 0x1ff | not written by the kick. A host write before the kick (3, 0x3f, 0, 1) reads back 0x1ff at the hang: restored by the TA soft reset at the kick, or read only |
| 0x334 | 0 | 0x01841000 | set during the TA (below) |
| 0x348 | 1 | 1 | |
| 0x614 | 0x2000 | 0x2000 | |
| 0x644 | 0x3db8 | 0x3db8 | |
| 0x738 | 1 | 1 | |

Everything else in both ranges reads 0 at idle and at the hang.

**0x334 / 0x338 follow the VDM's index blocks (C for the values, G for the meaning).** End only and the clear
without its draw: 0x334 = 0x01841000 (the stream's base, 0x238), 0x338 = 0. With the draw:
0x334 = 0x01841008 (the draw block's address, after the 2-word state update) and 0x338 = 3
(the draw's index count). So with a draw the VDM reaches it and stops there. Without one
these two keep their kick-time values, and nothing shows where the empty pass waits.

## Where it stands (00:51 UTC)

No register in the TA and DPM ranges shows a default the kick fails to set, apart from 0x228/0x22c,
which a write from the host can't change. Next options:
- write 0x228/0x22c (and 0x348, 0x738) from the microkernel after its TA soft reset (a
  microkernel change);
- with the draw, follow 0x334/0x338 to see whether any vertices are consumed;
- sweep the rest of the window's unnamed offsets (0x000-0x1fc, 0x400-0x5fc, 0x800-0xbfc,
  0xd00-0xfff) the same way.

## 2026-11-20 00:52-01:02 UTC: the rest of the register window, microkernel writes, the draw

**(c) The read sweep of the rest of the SGX window**, the same guards (`probe_read` through
the driver, 32 offsets a run, only while powered; the 2D FIFO pair 0xe10/0xe14 skipped in
case a read pops it). 0x000-0x1fc, 0x400-0x5fc, 0x800-0xbfc and 0xd00-0xffc; no wedge.
Nonzero at idle or changed by the hang, apart from known registers (clock control/status,
IDs, the event enables, `USE_CODE_BASE`, the event-program registers, `PDS_EXEC_BASE`,
`EVENT_KICKER`, `PDS_PC_BASE`):

| offset | idle | hang | note |
|---|---|---|---|
| 0x0118 `EVENT_STATUS2` | 0 | **0x10** | b4 rises at every TA; the SGX545's list names b0-3 and b5-10, not b4 |
| 0x0138 | 0 | 0x110 | the microkernel's invalidate-done bits |
| 0x0040 | 0x1e00 | 0x1e00 | |
| 0x0100 | 3 | 3 | |
| 0x04ec | 3 | 3 | |
| 0x0804 | 0 | 0x10000000 | set by the kick |
| 0x0aa4, 0x0aa8 | 0xaaaaaaaa | 0xaaaaaaaa | |
| 0x0b04 | 1 | 1 | |
| 0x0b08-0x0b1c | 0x10000 | 0x10000, except 0xb0c 0x10001 and 0xb10 0x10003 | |

**`EVENT_STATUS2` b4 is a one-shot event, not the blocker (C).** Cleared from the host
mid-run (write 0x10 to 0x114), it reads 0 and doesn't return, and the TA still never
finishes (end only, the clear without its draw, with it).

**(a) 0x228/0x22c are read only (C).** Written by the microkernel right after its TA soft
reset (scratch images built from the sgx530 branch's source with a `UK530_POKE` switch; with
no pokes the build is byte-identical to `sgx530_inv23`), to 3, 0x3f or 0, they read 0x1ff at
the hang, and the hang is unchanged.

**(b) The draw (C).** 0x334 / 0x338 sampled four times mid-run: 0x01841008 and 3 each time
(with the clear's vertex task or the empty one), while the PDS PC is on the terminate
program. The VDM goes past the draw and issues the terminate. 0x334/0x338 hold the last
index block's address and count, not progress.

## Where it stands (01:02 UTC)

Across the whole register window, no register shows a default the kick fails to set or a
pending event that stops the pass. The TA runs its stream to the terminate and never raises
TA finished, whatever the stream holds. What hasn't been varied is the order and timing of
the kick itself on this core: the TA soft reset (0x80 b3), the free-list and DPM loads, the
invalidate, and the `TA_KICK` write.

## 2026-11-20 01:03-01:06 UTC: the kick's order; parked

Scratch microkernel images from the sgx530 branch's source (07af02bb), with build switches
added to the scratch copy (not committed): `UK530_VAR` drops a step (`noreset`: no TA soft
reset at a first kick; `nofl`: no TA free-list load; `nodpmload`: no DPM state/control load
and OTPM clear; `noinv`: no TA-side `PDS_INV0` invalidates), `otpm1` writes 1 instead of 9
to the OTPM clear, `kick2` writes `TA_KICK` twice, and `UK530_KICKVAL` writes 3, 5 or 9
there. Unmodified, the build is byte-identical to `sgx530_inv23`. End-only fault-free stream:

| variant | result |
|---|---|
| noreset, nofl, nodpmload, noinv, otpm1, kick2, kick 3, kick 5, kick 9 | the same no-fault hang: no event, no tail pointers, no wait timed out |

None of the kick's steps or their order ends the pass, and leaving out the free list and DPM
loads changes nothing either, consistent with the TA never reaching the DPM.

**Parked.** `docs/porting/cores/sgx530.md` ("The TA stream" and "The kick itself") has the
recipe, the ruled-out list and the tools.
