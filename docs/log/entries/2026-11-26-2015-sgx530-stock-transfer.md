# SGX530: observing a stock transfer, and what applying it to ours did

2026-11-26 20:14-20:25 UTC, an OMAP3630 board. Joseph approved one observation-only run of
the stock stack, sampling a transfer the way the 11-20 render was sampled
([2026-11-20-0740](2026-11-20-0740-sgx530-stock-observation.md)). No stock-stack code, binary,
disassembly or capture enters any repo. The facts below are register values, in our own
words, tagged **O** (observed on the stock stack) or **C** (on ours).

Why: our 3D-only render (a transfer) starts the 3D and then reads fixed addresses, 0x0f00f000
and then 0x0b860000, and never ends
([2026-11-26-1000](2026-11-26-1000-sgx530-3d-only-transfer.md)). The 11-20 sampling saw
only a render, never a transfer.

## Method

- The same capture boot as 11-20: our kernel, the GPL kernel module, and the stock user space
  served to the board's RAM at run time. Booted once through the SD image slot and the boot
  loader's one-shot, by software reboot. No NAND writes. The normal SD boot was put back
  afterwards and checked (same kernel, same image md5).
- Our own GL client, loading the stock EGL/GLES2 at run time. It clears a 64×64 pbuffer and
  draws the 11-20 triangle, then:
  - `upload`: `glTexImage2D` of a 64×64 RGBA texture;
  - `copy`: `glCopyTexImage2D` of the frame into a texture.

  Each then draws the texture over the target and reads it back. Each step is fenced with
  `glFinish` and a 0.5 s pause.
- Our own sampler reads the SGX register window through `/dev/mem` (every word but the 2D FIFO
  pair and `BIF_MEM_REQ_STAT`, about every 265 µs) while the GPU's module is held on. It
  records the changes.

All three readbacks were right: the triangle, the green/yellow texture, and the copied frame.

## Observed (O): a stock transfer

Both the upload and the copy ran a transfer: one register write burst with 0x404 = 3, and no
TA kick of its own. They wrote the same values:

| register | stock transfer | ours (k_transfer) |
|---|---|---|
| 0x404 | 3 | 3 |
| 0x408 | 0 | 0 |
| 0x40c / 0x410 | 0 / 0x00030003 | the same |
| 0x414 | 0x100 | 0x100 |
| 0x4bc | 0x200 | 0x200 |
| 0x4c4 | 0x02002200 | 0x02000100 |
| 0xcac (3D_REQ_BASE) | **0**, written just before | the background block's 1 MiB base |
| 0xa5c (pixel event) | **0** | 0x1040 |
| 0x63c | 0 | 0 |
| 0x4d4, 0x4d8 | **0x31db3dc7, 0x2341b117** | not written |
| 0x4e4 | **0x303** | not written |

- Not changed by the transfer, so these hold the render's values. The sampler can't tell
  whether the transfer writes them: 0x400 0x80, 0x418 1, 0x41c/0x420 0x1e3ce500, 0x424 0xed,
  0x48c/0x490 0x80000000, 0x4b8 1.0, 0x4c8 0x88, 0xa60 4, 0xa64 0x4ffe, `PDS_EXEC_BASE`
  0x0dc00000.
- The next render puts 0x404, 0x408, 0x4c4, 0xa5c and 0xcac back to its own values, and
  0x4d4 back to 0.
- The upload and the copy wrote different pixels but left the same 0x4d4/0x4d8/0x4e4.
- 0x630 was 2 at the upload's transfer (5, then 7, at the copy's, which follows a render).

## Applied to ours (C)

Our clear transfer (`powervr_sgx_blit -c`, the 0x45 microkernel image), with the stock values
written once at init through `probe_init`. The transfer doesn't write them.

| run | result |
|---|---|
| 0x4d4 = 0x31db3dc7, 0x4d8 = 0x2341b117, 0x4e4 = 0x303 | fault at 0x0f00f000, as before |
| those, and 0x630 = 2, 0x418 = 1, 0x41c = 0x420 = 0x1e3ce500, 0x424 = 0xed, 0x48c = 0x490 = 0x80000000 | the same fault |

At the fault (the second run, `probe_read`), 0x630, 0x418-0x424 and 0x48c/0x490 hold what was
written. **0x4d4, 0x4d8 and 0x4e4 don't: they read 0xcf93eb13, 0x24f486cc and 0x202.** So the
3D writes those three during a pass (C).
- The stock transfer finishes with 0x4e4 = 0x303. Ours stops with 0x202. Two 8-bit fields that
  read 3 at the end of a pass, and 2 where ours stops, may be per-stage progress (G).
- 0x4d4/0x4d8 match for two stock transfers of different pixels, so they aren't a checksum of
  the pixels. They might depend on the background object's geometry (G).

## What's left

One more run on ours: the pixel-event program at the PDS exec base + 0, as the stock's
(`powervr_sgx_blit -Z`, 0xa5c = 0). The same fault at 0x0f00f000, and the same 0x4d4, 0x4d8
and 0x4e4 as the run before (0xcf93eb13, 0x24f486cc, 0x202), so those are deterministic for
our transfer (C).

The one difference from the stock transfer left untested is the address layout: 3D_REQ_BASE
0, with the background block in the first MiB (device address 0x22000). Our heaps start at
0x01800000. The first fixed read didn't move when 3D_REQ_BASE moved by 1 MiB (11-26 morning,
run 5), so the base alone is unlikely to be it.

So the stock transfer's registers don't explain our fixed reads. What's left is what the
3D reads from memory: the background block and the PDS programs. The stock 0x4d4/0x4d8,
written by the 3D and identical for two transfers of different pixels, would let us check a
background block against the stock's without reading the stock's code. I stopped here, at
the agreed few runs.
