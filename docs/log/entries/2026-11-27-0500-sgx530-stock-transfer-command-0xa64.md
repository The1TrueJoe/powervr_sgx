# SGX530: the stock transfer command, 0xa64, and what the PBE still lacks

2026-11-27 03:00-05:00 UTC, an OMAP3630 board. One observation-only capture of the stock stack
(Joseph's OK, 2026-11-27), then our own stack, RAM only. Stock facts are tagged **O** and are
register and data values in our own words. No stock code or capture enters the repo.

## The stock transfer command (O)

Method: the capture boot as on 11-26
([2026-11-26-2015](2026-11-26-2015-sgx530-stock-transfer.md)), with one more capture-only hook in
the GPL kernel module. It copies each transfer command from the client's CCB, 0x100 bytes
either side of its shared part, and the kick's command words. No program heap is copied. Our GL
client's `glTexImage2D` and `glCopyTexImage2D` each sent one. Both read back right.

- On this DDK the command starts at the CCB offset 0 and its shared part at +0x70 (the
  SGX545's DDK: +0xac).
- Its first 17 words are the ones our microkernel's `k_transfer` programs into the 3D. Both
  transfers wrote the same values, and they match ours except for:

| word | register | stock | ours |
|---|---|---|---|
| 6 | 0xcac (3D_REQ_BASE) | 0 | the background block's 1 MiB base |
| 7 | 0x42c | **0** | 0x80000000 |
| 11 | 0xa64 | **0x4ffe** | 1 |

- Nothing else in the command looks like PBE state.

## 0xa64 = 0x4ffe makes the end-of-tile task's registers work (C)

A new switch, `probe_xfer=idx,value,...`, replaces transfer command words 0-16 (SGX530 only, off
by default).
- **With word 11 = 0x4ffe, the end-of-tile task's own `limm`s reach its instructions.** An
  end-of-tile program `limm r0, #0x0b8f0000; limm r1, #0xcafef00d; st.a.d r0, #0, r1` writes
  0xcafef00d at 0x0b8f0000. With 1 (ours) it faults at 0x08039000. With 0 it faults at
  0x09039000, so what that task sees in r0 moves with 0xa64. With 0x4fff it writes nothing.
- With 1 or 0 the PBE writes the tile at the fixed wrong place (16 runs of 8 words from
  0x0b85741c). With 0x4ffe or 0x4fff it writes nothing, and the job times out.
- Word 7 = 0 with it changes nothing more.

## The PBE (open)

With 0xa64 = 0x4ffe our values reach the end-of-tile task's emits. No form tried makes the PBE
write. A small set, written down before the runs:
- our SGX545 end-of-tile program;
- the SGX535 word order from a public SGX535 driver (L: stride, address, clip min, clip max,
  12-bit y fields);
- one emit with `imm=512` or `imm=2048`;
- two emits with the first `n=1`;
- two emits in the other order.

The two-emit forms time out. The one-emit forms end in a requestor-b2 fault at 0x0f400000, a
task launched at code base 0 + 0. Nothing is written to the destination or the probe range.

That public driver's end-of-tile program is a kept set of vendor program words, so it wasn't
read.

## Pixel tasks: what reaches the tile, and the b1 read (C)

- **The tile gets the value of register 0, whatever its bank.** A pixel task's
  `limm <bank>0, #0x03456000` puts 0x03456000 in the tile for the r, o, pa and bank 3, 4 and
  7 forms. The PBE writes it with its bytes reversed (0x00604503). The kernel's USSE check
  refuses banks 5 and 6. The pixel task's r0-r3 don't carry into the end-of-tile task (a pixel
  task that preloads them leaves the PBE's writes unchanged).
- **Only an o-bank write is also read as an address**, by requestor b1. That's the 0x0f00f000
  "fixed read" (the clear colour). It matches the TA's state task, whose o0 the MTE reads as an
  address (sgx530.md, "The TA stream").
- `limm o0, #0x0b8f0100` puts 0x0b8f0100 in the tile, not the word at that address. So the
  b1 read isn't where the tile's data comes from.

## Next

- The PBE emit on this core: the TA's lesson (o-bank outputs are read as addresses) might apply
  to how a task hands the PBE its state.
- Back to the TA with the bank result: whether the stock state task's words reach the MTE as
  data because they're written to a bank other than o.
