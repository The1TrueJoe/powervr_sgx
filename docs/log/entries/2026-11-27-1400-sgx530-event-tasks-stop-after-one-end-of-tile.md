# SGX530: after one end-of-tile task, no event task runs

**2026-11-27, 09:40 to 10:05 UTC. The OMAP3630 test board, our stack, RAM only. Released clean.**
It follows [2026-11-27-1300](2026-11-27-1300-sgx530-big-stock-transfer-end-of-render.md).
New blit knob: `-Y`, the clear as one transfer job per tile, each waited for (off by default).

## The two tries (C)

**One job per tile, with b27** (`-Y`, the tile word's b27 forced, so each job ends its own
render):
- the first job writes its tile;
- then it times out with `EVENT_STATUS` = 0x00040000, the pixel back end's end of render,
  latched and never handled. The driver then cancels the context, so the second job is refused
  (−125).
- A single-tile b27 job completed in one run and timed out in another with the same setup. So
  whether that end of render is handled is a race.

**A pixel task with `phas.i` and memory traffic, and its colour in r0:** pixel tasks get r0-r3
only (11-27), so there's no spare temporary for the memory op. With r1/r2 for it, the
end-of-tile task runs about once per tile, but the PBE writes nothing. The combined form
isn't possible as asked.

## What blocks (C)

The microkernel's timer, which our init doesn't leave running on this core (`EVENT_TIMER`
reads 0), was started from the host (`probe_init=0xacc,0x01030d40`). It ticks:
`EVENT_TIMER` counts. Then:

| clear | end-of-tile program | result |
|---|---|---|
| `limm r0` | a counter, then `nop` | one end-of-tile task. Then **`EVENT_STATUS` = 0x20000000 (the timer event) pending and never handled**: no microkernel event task runs again. Timeout |
| `limm r0` | the PBE emits | one tile written. Then the timer event, and later the end of render too (0x20040000), pending and unhandled |
| `limm r0` | `nop` (`-E -E`) | the same block |
| `limm o0`, its address page mapped | the counter, or the PBE emits | one end-of-tile, one tile (252/256 right), then the same block |
| `limm o0`, no page mapped | `nop` | the b1 read of the colour as an address faults first, and the driver recovers |

So **after the first end-of-tile task is launched, the core issues no further event tasks of
any kind**:
- not the next tile's end of tile;
- not the microkernel's timer;
- not the pixel back end's end of render.

That one stall is behind all three earlier symptoms:
- the single end-of-tile event;
- the lost end of render;
- the timeouts.

When it does complete (11-27's forced-b27 full render, one single-tile run), the end of render
got in before the stall.

## Where it stands

Parked, with the one-tile PBE write as the SGX530's PBE result.

The open question is narrower now: **what the end-of-tile (pixel-event) task must do, or how
it must be launched, for the core to issue event tasks after it.** Not changed by:
- the end-of-tile program: PBE emits, a `nop`, with or without `phas.i`;
- 0xa64's fields;
- the DOUTU word layout;
- the SGX535's DMA of the PBE words;
- the ptoff routing.

The one thing seen to change the count (`phas.i` and memory traffic in the pixel task) points
at the pixel tasks' completion, not the end-of-tile task's.

Also open: the microkernel leaves `EVENT_TIMER` at 0 on this core. The stock runs it at about
1 kHz.
