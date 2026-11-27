# SGX530: a full 64×64 clear from a 3D-only render, and what branches need

2026-11-27 02:30-04:00 UTC, an OMAP3630 board, RAM only, our own code. It follows
[2026-11-27-0200](2026-11-27-0200-sgx530-first-pixels-stores.md), where an unrolled store
program cleared 32×32 pixels and loops didn't work.

## Result

**The whole 64×64 destination reads back as the clear colour, 4096/4096 pixels right.** The
background object's pixel program is a store loop:

    phas.i #0, #0 [wait=7]
    limm r0, #0x0180a000            # the destination
    limm r1, #0xff00ff00
    limm r2, #4096
    limm r3, #4
    st.a.d r0, #0, r1 [moe]
    imae r0, r3, #1, r0 [t2=u32]
    test.isubu16.ne p0, r2, r2, #1
    (p0) br #-3
    idf
    wdf
    nop [end]

`tools/first-output/clear_stores.py 0x0180a000 0xff00ff00 64 64 64` writes it, with 16 trailing
`nop`s so that the test gives it a code object of its own (below). (C)

## Branches (C)

- **`br` counts from the branch itself, in instructions.** It's shown by a landing pad: a
  store, `br #N`, then 24 (`limm`, `st`) pairs each writing its own marker to its own slot.
  `br #1` lands every marker, `br #4` lands on slot 2's store (with the earlier register value),
  and `br #9` on slot 5's `limm`. The program then runs to its end.
- **A program that branches must not share its code object.** Inline in the test's shared code
  object (after the context's emit, nop and copy programs, at +0x140 or +0x180), every taken
  branch hangs the task: at most one more instruction runs (`br #1`: B lands, C doesn't), and
  the job times out. The same program in a code object of its own works, at +0x0, +0x40 or
  +0x140 into it. The cause is unknown. These don't fix the inline case:
  - `[syncs]` on the target;
  - DOUTU word 2 = 0x20 (the stock pixel launches' word 2);
  - word 0 bit 21.
- The kernel's USSE check refuses an absolute `ba` whose target it can't place
  (UPLOAD_CODE: -EINVAL). `ba` wasn't needed.

## Temporaries (C)

- A pixel task launched by our background PDS program has **r0-r3 only**:
  - the same column loop works with its counter and step in r2/r3 (48/48 pixels);
  - with them in r4/r5 it doesn't (1 pixel).
- These don't change it:
  - DOUTU word 0 b24-31 (2, 8 or 0x20, `-t`);
  - our microkernel's launch words (word 0 b21, word 1 0x04000000, word 2 1).
- Word 1 with b26 set gives a different failure: a requestor-b2 store fault at 0x01817000.
- How a pixel task gets more temporaries on this core is open. The microkernel's tasks use
  r17, so the EDM's launch gives them more.

## Tools

`powervr_sgx_blit`, new bring-up flags, each off by default:
- `-A off`: an inline clear's offset;
- `-a off`: its offset in a code object of its own;
- `-1 w1`, `-W w2`, `-O bits`: the clear's DOUTU words.

`clear_stores.py` writes the loop for a full-width block, and the unrolled form otherwise.
