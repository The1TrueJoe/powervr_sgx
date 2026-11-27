# SGX530: the event stall against launch context and pixel-task timing (null)

**2026-11-27, 10:10 to 10:30 UTC. The OMAP3630 test board, our stack, RAM only. Released clean.**
It follows [1400](2026-11-27-1400-sgx530-event-tasks-stop-after-one-end-of-tile.md). The
setup:
- every run starts the microkernel timer from the host (`probe_init=0xacc,0x01030d40`);
- the end-of-tile program counts its runs into the destination's last word (`-G`, then a
  counter and `nop [end]`).

A stall shows as the timer event left pending (`EVENT_STATUS` 0x20000000) and the job timing
out. New knob: `powervr_sgx_blit -U bits`, ORed into the end-of-tile task's DOUTU word 0
(off by default).

From `docs/porting/cores/sgx530-state.md` on the sgx530 branch (11-14, O): the
stock 530 sets DOUTU word 0 bit 21 only on launches that DMA first, and its pixel DOUTUs carry
word 2 = 0x20.

| try | result |
|---|---|
| (a) no write-retire bit on the pixel and end-of-tile launches | that's how ours already are (`sgxtex_doutu` sets no b21). Stall after 1 end-of-tile |
| (b) b21 on the end-of-tile launch (`-U 0x200000`), the program ending in `idf; wdf` | stall after 1 |
| the stock's pixel DOUTU word 2 = 0x20 (`-W 0x20`) | stall after 1 |
| (c) a pixel task of `phas.i`, `limm pa1, #addr`, `ld.a.d pa2, pa1, #0`, `wdf`, then the colour in r0 | stall after **2** |
| the same with the colour in o0 (its address page mapped) | stall after 2 |
| (c) with a store through pa1 (`st.a.d pa1, #0, pa1`, `idf; wdf`) instead of the load | the store lands. Stall after 1 |
| `DMS_CTRL` (0xa74) with 1-4 pixel tasks (b4-9) and 1-7 pixel partitions (b22-24) | no end-of-tile at all, and the same stall. Our k_transfer rewrites 0xa74's upper fields: it reads back 0x07e08xx0 |

So:
- the launch context doesn't move the stall: bit 21 and word 2 = 0x20;
- neither does fencing the end-of-tile task;
- a load in the pixel task lets one more end-of-tile through;
- the longer counter task of 1300 lets about one per tile through.

That fits **timing** better than a write-retire count: the longer the pixel tasks run, the more
end-of-tile tasks get issued before the stall. Fewer pixel task slots stop even the first one.

## Parked

The SGX530's PBE result is one correct 16×16 tile per job. What's left, with the evidence in
1300, 1400 and here:
- why the core stops issuing event tasks after the first end-of-tile task, when pixel tasks are
  short;
- the last-tile flag's source;
- the o0 address read.
