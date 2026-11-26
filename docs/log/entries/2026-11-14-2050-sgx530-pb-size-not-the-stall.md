# SGX530 board: the PB size isn't what stalls the TA

**2026-11-14 20:48 UTC. Our OMAP3630 test board, kernel 7.3.3. Under lock. Clean, recovered
by HWR; modules unloaded.**

The vendor stack's PB descriptor (read from the kick snapshot) has 0x3dd pages, so every page
number fits in 12 bits. Ours has 0x1022, with page numbers up to 0x1021. Its thresholds also
differ from our formulas: TA 0x3ed (pages + 0x10, the same as ours), ZLS 0x200, global
0x3ed, PDS 0x27; and descriptor word 5 is 2. Its RT data has the same shape as ours (scratch,
TA-data and tail-pointer words line up one for one).

`SGXGEN_PB_PAGES` (CREATE_CONTEXT's `param_buffer_pages`), with every vendor form on the
client and the driver's 530 TA command values:

| request | PB pages | result |
|---|---|---|
| 0x3bb | 0x3dd (the vendor's) | render timeout, region headers 0 |
| 0xfde | 0x1000 (last page 0xfff) | same |
| 0xfdf | 0x1001 (last page 0x1000) | same |

The TA clock is on and the microkernel's wait 0xd gives up twice in each run. So neither the
PB's size nor 12-bit page numbers explain the stall.
