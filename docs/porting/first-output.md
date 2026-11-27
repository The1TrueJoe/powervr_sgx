# First output without the TA

How the SGX530 put its first pixels in memory while its TA still never finishes, written so a
port to another USSE core can follow it with only that core's register table. The SGX530's
values are examples. Every one of them is a fact about the SGX530 only, confirmed on an
OMAP3630 board ([cores/sgx530.md](cores/sgx530.md), "3D only"), and another core has to find
its own.

The idea: a **transfer** is a render with no TA. One background object covers the destination,
its pixel program runs a shader, and the end-of-tile program hands the tile to the PBE
([UKERNEL.md](../UKERNEL.md), "Transfer"). If the PBE path doesn't work yet, the pixel shader
can store to memory itself.

## Before you start

- The driver binds and reads the core's ID and revision. The microkernel starts, and the
  driver reports "microkernel started".
- The core's transfer command def is enabled (`docs/uapi/cmd/pvrsgx_cmd_defs.c`).
- A board you can recover without a power cycle when possible: every step below ends in a
  driver reset, not a wedge, on the SGX530, but a new core may differ. Keep runs bounded (the
  driver's 2 s job timeout) and RAM only.

## What a core needs to get right

Each of these was wrong for the SGX530 when it started from the SGX545's values. Where to look
for each, and the SGX530's answer:

| What | Where it shows | SGX530 |
|---|---|---|
| PDS data layout | the PDS reads launch words from the wrong slots, so tasks launch at code base 0 | 8-word blocks (`PVRSGX_FEATURE_PDS_DATA_INTERLEAVE_2DWORDS` unset) |
| DOUTU word 0's code offset | a task faults on its code fetch (requestor b2 on the 530) at the wrong offset | bytes at b4, in a 32 KiB window (`DRM_POWERVR_SGX_CAP_DOUTU_OFFSET_BYTES`) |
| Tile width | the transfer's first/last tile words | 16 pixels (`sgxtex_core.tile_w_log2` 4) |
| The pixel-event PDS address (0xa5c) | the stock 3D's value against `PDS_EXEC_BASE` | an offset from `PDS_EXEC_BASE` (`PVRSGX_FEATURE_PDS_EVENT_OFFSET`) |
| The 3D-side invalidate-done bits | a bounded wait times out: control block +0xa0 names it, +0xa4 counts | 0x45 (PDS_INV1 b2+b6, CSC b0; the 545's 0x4b) |
| The `USE_CODE_BASE` address unit | the microkernel doesn't start | 256 bytes |
| `imae` operand widths | pointer arithmetic truncates | multiplicands 16 bits, addend 32 bits |

The 3D registers `k_transfer` programs (the SGX545's `T3_CMD` table) held their values and
started the 3D on the SGX530 with no change. Check that with `probe_read` on a new core.

## Step 1: run the clear and read the hang

    powervr_sgx_blit -v -c

With the driver loaded `allow_unisolated=1 edm_init=1 read_ukregs=1`. `-v` prints what sgxtex
was set up for (the PDS layout, the code window, the DOUTU form, the tile width). Then read the
devcoredump:
- **A bounded wait timed out** (control block words 0x28/0x29): the microkernel waited for
  done bits the core doesn't set. Fix that first: it's a microkernel constant.
- **`CLKGATESTATUS` shows the 3D units on** (ISP b4 and TSP b8 on the SGX530): the 3D started.
- **A fault at your clear colour's address** (0xff00ff00 masked to the address width): the
  pixel task ran and its o0 was read as an address. That's progress, not a bug in your shader
  words. `-K` changes the colour to 0x12345678, which moves the fault to 0x02345000 and proves
  it.

## Step 2: follow a fault with pages

A fault address that nothing seems to move is usually a value of yours read as an address.
Map a page there and see what happens next:
- `probe_kpage=ADDR` maps one page inside a firmware-shared range (kernel code, data, sync),
  fills it with a pattern, and the hang dump lists the words that changed. With bit 0 of the
  address set, every word holds `probe_kpage_fill` instead.
- `probe_va0=PATTERN probe_va0_addr=ADDR probe_va0_pages=N` maps N pages in every client VM, and
  the kernel log lists every changed run of words when the VM goes. That's how the PBE's writes
  showed up: 16 runs of 8 words.

Then move one input by a known amount and see whether the fault or the writes move with it.
That's the delta method: the destination (`-p bytes`), the background block (`-B bytes`), the
tile count (`-w`), the kernel data heap (`probe_kdata_base`), one PBE word (`-P i=v`). Change one
thing a run, and write down what didn't move as carefully as what did.

## Step 3: store from the pixel task

The shader is yours: `-S w0,w1,...` or `-S @file` (whitespace-separated hex words, low word
first) replaces the clear shader. Assemble with `sgx-asm -L` (`ukernel/sgx-asm.sh` builds it).
1. One known word to a probe page, to prove a task's store lands:

       phas.i #0, #0 [wait=7]
       limm r0, #ADDR
       limm r1, #0xcafef00d
       st.a.d r0, #0, r1 [moe]
       idf
       wdf
       nop [end]

2. The clear as stores into the destination. `tools/first-output/clear_stores.py ADDR COLOUR W H
   PITCH` writes the shader. A full-width block (PITCH = W) is a loop; anything else is one
   store per pixel. On the SGX530, at the test's destination 0x0180a000:

       python3 tools/first-output/clear_stores.py 0x0180a000 0xff00ff00 64 64 64 > clear.s
       sgx-asm -L clear.s | awk '{print $2, $3}' > clear.hex
       powervr_sgx_blit -v -c -S @clear.hex -t 8

   `clear done, 4096/4096 pixels right` is the result. Every pixel instance writes the whole
   block, which is slow but harmless.

   Three things the SGX530 needed, so check them on another core:
   - A program with a taken branch hangs when it shares a code object with other programs. The
     generator pads the loop with `nop`s so the test gives it an object of its own.
   - A pixel task launched this way has r0-r3 only. The loop uses nothing else.
   - `br` targets count from the branch, in instructions.

## Step 4: the PBE (open on the SGX530)

On the SGX530 the end-of-tile program's `emit.t0` does reach the PBE: without it nothing is
written. But the PBE writes at an address that nothing in the end-of-tile task moves, and the
task's `limm`s don't reach the values the emits send. The open questions are in the
[build log](../log/entries/2026-11-27-0200-sgx530-first-pixels-stores.md).

## Observing the stock stack (only with the owner's approval)

What the stock driver does for the same operation is the best oracle, and it can be observed
without its code entering anything:
- Boot your own kernel with the GPL kernel module and load the stock user space into RAM at run
  time. Nothing goes on flash, and none of it goes into a repo or a CI artifact.
- Drive it with your own GL client. On the SGX530, `glTexImage2D` and `glCopyTexImage2D`
  each ran one transfer.
- Sample the register window with `tools/first-output/sgxsnap.c`. Set `SGXSNAP_BASE` to your
  SoC's GPU base. Read only while the GPU's power and clocks are held on (runtime PM
  `control=on` and `runtime_status` active): an unclocked read hung a board.
- Record register values and GPU memory output only, in your own words, tagged **O**. Don't
  read, keep or describe the stock programs' code.

## Another core

Keep each core's facts on its own fact sheet. A value that the SGX530 needed is a lead for
another core, not a fact about it.
