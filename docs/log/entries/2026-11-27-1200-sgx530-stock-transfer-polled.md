# SGX530: the stock transfer polled from the kernel, and TRIG_3D on ours

**2026-11-27, 08:45 to 09:25 UTC. The OMAP3630 test board. Two one-shot capture boots of the
stock stack (observation only, Joseph's standing go), then our own stack. The normal SD boot was
restored after each. Released clean.**

## Method (O)

The capture kernel's GPL services module got a capture-only hook (in the capture image's
build, not in this repository). Right after the stock driver queues each of its first four
transfers, it reads a few SGX registers in a tight loop for 20 ms and keeps every change:
- **Pass 1:** 11 registers per loop, stamped with the kernel clock. That clock moves in
  30.5 µs steps on this board.
- **Pass 2:** five registers, every 1.73 µs:
  - `EVENT_STATUS` (0x12c) and `EVENT_STATUS2` (0x118), with the OR of every value each
    took;
  - `CLKGATESTATUS`;
  - `ISP_STATUS2` (0x4e4);
  - `PIXELBE_EMIT` (0x4ec).

  Changes are stamped with the loop iteration.

Our own GL client drove the stock stack: `glTexImage2D` and `glCopyTexImage2D`, two of each,
on a 64×64 target, 16 tiles. Only register values were kept, and the data stays outside the
repository.

## What the stock transfer shows (O)

- **The 3D runs in a burst too short to sample.** Each transfer appears as one to three single
  samples of `CLKGATESTATUS` = 0x00111110 (every unit's clock on), 6-18 ms after the queue,
  each shorter than one 1.73 µs loop. In between, the clocks read 0x00100100 or 0.
- **No 3D event is ever seen set.** Over all four transfers:
  - the OR of every `EVENT_STATUS` value is 0x20000000 (the timer only). No end of render
    (b18), no ISP end of tile (b23), no DPM free (b0);
  - the OR of every `EVENT_STATUS2` value is 0: no `TRIG_3D` (b3).
- **`ISP_STATUS2` stays at tile (3,3)** from before the first transfer to after the last.
  Every transfer here ends on that tile with the same signatures (0x4d4/0x4d8), so this
  register can't show progress.
- **`PIXELBE_EMIT` stays at 3.**

So on the stock stack, a 16-tile transfer finishes, and its events are raised and handled,
in about the time of one register read of these five. The order of tile advances against
b18/b23 can't be resolved by host polling. It would need the GPU to record it, or a hardware
trace.

## The contrast with ours (C)

At our hang, `EVENT_STATUS2` holds `TRIG_3D` (0x8) for seconds, and `EVENT_STATUS` stays 0.
The stock never shows `TRIG_3D` at all. Our microkernel never acknowledges `EVENT_STATUS2` on
this core: its acknowledge is the SGX545's DCU-invalidate path, off here.

A scratch microkernel switch wrote `EVENT_HOST_CLEAR2` = 0x1f right after the 0x428 kick
(not committed). `TRIG_3D` is still set at the hang, and the run is otherwise the same: one
tile, a timeout. So `TRIG_3D` reads as the 3D's own "running" state rather than an event left
unacknowledged. The stock's 3D just finishes before it can be sampled.

## Where it stands

- The stock's 3D pass is over in microseconds, and ours never ends.
- Host-side observation has reached its limit: nothing it can sample separates the two.
- Open, unchanged: why the PBE takes only one end-of-tile, and why no end-of-render event
  comes.
