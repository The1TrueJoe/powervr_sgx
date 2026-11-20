# SGX530: observing the stock stack's registers around a TA, and applying them to ours

2026-11-20 07:16-07:59 UTC, an OMAP3630 board. Joseph approved observing the stock stack
on this board's SGX530. Observation only: no stock-stack code,
binary, disassembly or capture enters any repo. The facts below are register values, in our
own words. They're summarised in `docs/porting/cores/sgx530.md`, "Observed on the stock stack".

## Method

- **Capture boot.** Our capture image (a separate GPL-only build harness, rebuilt for the
  host machine's new address): our kernel, the GPL kernel module, and the stock user space served
  to the board's RAM at run time. RAM boot through the SD image slot and the boot loader's
  one-shot. No NAND writes. The board's remote power plug turned out not to power it any
  more, so the reboots were software reboots (Joseph's OK).
- **Sampler.** Our own tool reads the SGX register window (all of 0x000-0xffc except the 2D
  FIFO pair and `BIF_MEM_REQ_STAT`) through `/dev/mem` in a loop and records the changes.
  The GPU's ti-sysc module is held on with runtime PM `control=on`, and nothing is read
  unless `runtime_status` is `active`. It was tried first on our own stack.
- **Runs.** An idle sample after the stock init, a full-window sample across a 64×64 render,
  and a fast sample of the event/status registers across a second. Both renders read back
  correctly (red triangle on the 0x000080 clear).

## Observed (O)

| register | stock | ours |
|---|---|---|
| 0x208-0x248 TE/VDM layout | 0x01004004, 0x01004004, 0x3003, 0x10, 0x01000000, 0x1a2, 1e-6, 1e-6, 0x0003f03f | the same |
| 0x614 | 0x2000 at idle, **0x27** at the TA | 0x2000 |
| 0x250 | 0x88 | 0 |
| 0x26c | 0x44a (written by the hardware) | 0 |
| 0x630 | 5, then 7 | 0 |
| 0xa74 | 0x003f8200 at idle, 0x083f8200 at the TA, then 0x09bf8200 | 0x05208200 |
| 0x600/0x604/0x64c | the 3D free list, loaded with the TA's | loaded at 3D start |
| 0x700/0x724/0x72c | head + 2, 0x00020002, 0x00010001 | unmoved, 0, 0 |
| 0x230/0x234/0x62c/0x634 | 0 | written from the SGX545's tables |
| 0x308/0x30c | set once, after the TA | 0 |
| 0xabc / 0xb30 / 0x040 / 0x804 at idle | 0x40 / 0x100 / 0x1ff / 0x1000ffff | 0 / 0 / 0x1e00 / 0 |
| `USE_CODE_BASE` spacing | 0x800 | 0x80 |

The second render programmed the same values, so its fast sample shows few changes. The TA's
events are acknowledged faster than the sampler's roughly 30 µs period.

## Applied to ours (C)

Scratch microkernel images from the sgx530 branch's source (byte-identical to
`sgx530_inv23` with no switch set), writing the stock values just before `TA_KICK`, and
`probe_init` for the idle values. Fault-free end-only stream and the clear with its draw:

| variant | result |
|---|---|
| 0x614 = 0x27 | hang |
| + 0x250 = 0x88, 0x274 = 0x30000 | hang (0x26c reads 0: read only) |
| + 0x230/0x234/0x62c/0x634 = 0 | hang |
| + 0xa74 = 0x083f8200 | hang |
| + 0x630 = 5, 0x218 = 0x01400000 | hang |
| 0xa74 alone | hang |
| the 3D free list loaded at TA time, alone and with all of the above | hang (it loads: 0x600/0x604/0x64c/0x70c set) |
| idle values 0xabc, 0xb30, 0x040 (each, and with 0xa58, `CLKGATECTL`, 0xa00) | hang |

**TA finished still doesn't fire on ours.** In every run the DPM hands out no page, where the
stock TA gets two at once. So the difference isn't in the registers the kick leaves. It's
upstream, in what the MTE receives from the state tasks.
