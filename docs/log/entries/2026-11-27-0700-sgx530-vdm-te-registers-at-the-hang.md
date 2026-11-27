# SGX530: TI's VDM, TE and MTE registers against our TA, and where the VDM stops

**2026-11-27, 04:50 to 05:00 UTC. The OMAP3630 test board, under lock, a fresh software reboot
before each run. Released clean.**

TI's `sgx530defs.h` (L, cleared for reference on 2026-11-27) was read against:
- our TA kick: the TE, RT and PB tables of the microkernel, whose SGX530 core inherits them
  from the SGX545;
- our VDM stream (`tools/gen/sgxgen.c`);
- the stock TA's observed register values (O, the 11-20 capture).

Facts here are in my own words. Nothing from the header is copied into code.

## The microkernel fix (C, sgx530 branch ce7a1b60)

The SGX530's PB load now has three changes:
- it writes the PDS page threshold to 0x614;
- it no longer writes the SGX545's free-list selector (0x664);
- it no longer writes the SGX545's PDS threshold offset (0x668).

The SGX530 has neither 0x664 nor 0x668. The SGX545 golden is unchanged. On the board the
image behaves as before: every wait completes, and 0x614 reads 0x0f00, the descriptor's
value. The TA still hangs.

## The header against what we write (L)

**Registers our TE table writes that the SGX530 doesn't name:**
- 0x2a8, 0x2b0-0x2b4, 0x2d0-0x2d4, 0x324, 0x364-0x374 and 0x74c;
- 0xa04 and 0xaa0, which are named (the USE load redirect and store range).

**SGX545 meanings our tables use, and what the SGX530's registers are:**

| register | what we write (the SGX545 meaning) | the SGX530's register |
|---|---|---|
| 0x230 | the RT flags (0x88) | the OTPM control-stream flush base, an address |
| 0x234 | the tail-pointer base | the OTPM control-stream load base |
| 0x62c, 0x634 | the region-header and control bases | the DPM state-table and control-table bases |
| 0x250 | nothing | the MTE's multisample positions; the stock writes the 0x88 there |

**The named TE/MTE set matches the stock (O).** That covers 0x204-0x220, 0x238-0x248,
`MTE_CTRL` 0x1a2 and the 1e-6 W compare and clamp. The bounding-box registers 0x228/0x22c
read 0x1ff at idle on both: they're the TE's output, an empty box.

**`DMS_CTRL` (0xa74) explains the 11-15 gate.** From the top bit down:
- b25-27: the maximum vertex partitions;
- b22-24: the maximum pixel partitions;
- b16-21: EDM tasks;
- b10-15: vertex tasks;
- b4-9: pixel tasks;
- b0-3: per-data-master disables.

The 0x00208200 that stalled has 0 vertex partitions. 0x05208200, which runs, has 2. The
stock's 0x083f8200 has 4, and 4 was tried on 11-20 and is also fine.

**The VDM's state-update words are named too**, through the context-store copies
0x340/0x344:
- word 0 is the PDS state program's base (b0-21), with complex b28 and terminate b29;
- word 1 is:
  - the USE attribute size (b0-7, in fours);
  - secondary b8, partitions b9-11, partial b12, USE pipe b13-14;
  - SD b15, secondary-exec b16, **MTE emit b17**;
  - the PDS data size b26-31.

The 530 forms we copied from the stock stream fit them:
- 0x2a200 for a state program: MTE emit, SD, pipe 1, one partition;
- 0x16100 for a secondary update: secondary-exec, pipe 3, secondary.

**What `VDM_START` (0x200, a pulse) needs first, as far as the header shows:** only the
control-stream base, 0x238. 0x348 (`VDM_WAIT_FOR_KICK`) reads 1 at idle on both stacks.
0x330-0x358 are the VDM's context-store registers (start, stream, index, status, the two
state words, wrap/fan state), for preempting a TA. Nothing in the header has to be set up
before a kick.

## Read at the hang (C)

The pointer-mode clear-only recipe, with the `probe_read` driver parameter:
- **The VDM stops after the first block.** 0x334 (the stream position) is the stream base +
  8, and the index registers 0x338/0x34c read 3. That one block is the clear's state update.
- 0x340/0x344, the latched state words, read 0. 0x348 reads 1.
- **Nothing reaches the TE.** `MTE_SIG1`/`SIG2` (0x26c/0x270) stay 0. The stock reads 0x44a.
- USE0's vertex service counter (0xb0c) reads 0x00010003: three vertex tasks, then empty.
  The PDS PC is in the state program, and the region-header parser reads idle (0x738 = 1).
- 0x230 reads 0x80: our RT-flags word, masked as an address.

So the stall is between the state update's USSE task and the MTE. The VDM has issued the
first block's task and waits. The MTE never takes in a state word, so the TE sees nothing.

## Only the named registers (C)

The same recipe and the 0x614 fix, plus:
- the TE table cut to the registers the SGX530 names;
- the RT flags in 0x250 instead of 0x230;
- no writes to 0x230/0x234/0x62c/0x634.

Every one of those reads back as written: 0x250 = 0x88, and 0x230/0x234/0x62c/0x634 = 0. The
unnamed registers read 0. **The same hang**: the VDM stops at base + 8, and the TE signatures
stay 0.

## Where it leaves the TA

Every register the header names in the TA, TE, MTE, VDM and DPM blocks is either:
- what the stock writes (O, tried on 11-20 and again here); or
- an output, a status bit or a context-store copy.

The header names no other control for the MTE. So the difference stays where 11-20 put it,
in how the state task hands its words to the MTE. The 530's MTE reads the state task's
first output word as an address (11-15, 11-27). The header names the VDM side of that
(MTE emit b17) but not the USSE side: what the emit instruction must carry for the MTE to
take words, not a pointer. That is in the ISA, which the header doesn't cover.
