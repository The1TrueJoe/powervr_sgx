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
