# SGX530 board: the registers the microkernel reads, read by the host

**2026-11-14 21:53-22:05 UTC. Our OMAP3630 test board, kernel 7.3.3. Under lock. Read only,
approved 2026-11-14 ("Yes, read 0x138"). Clean, recovered by HWR; modules unloaded.**

`read_ukregs=1` (off by default, SGX530 only) adds to the `status` file and the coredump
every register the 530 microkernel reads that the 530's GPL header doesn't name: 0x138,
0x218, 0x274, 0x408, 0x624, 0x628, 0x630, 0x700, 0x710, 0x724, 0x72c, 0x734, 0x804 and
0xa74. The host never writes them.

The run was the 64x64 kick with every client form on (`probe.sh`). Values in the coredump
match the mid-hang ones.

| reg | idle | mid-hang | note |
|---|---|---|---|
| 0x138 | 0 | 0x000001d8 | the TA-side invalidate wait polls for 0x27 here: none of those bits ever set |
| 0x218 | 0 | 0x01000000 | the microkernel writes TA flags 2 (0x00400000) here, but it reads back differently |
| 0x274 | 0 | 0 | |
| 0x408 | 0 | 0 | |
| 0x624 | 0 | 0x00001000 | the PB's ZLS threshold, as written |
| 0x628 | 0 | 0x00011000 | the global threshold, with the 0x10000 mode bit, as written |
| 0x630 | 0 | 0 | |
| 0x700 | 0 | 0x10210000 | the PB free-list head (last page << 16 \| first) |
| 0x710 | 0 | 0x00001020 | |
| 0x724, 0x72c, 0x734 | 0 | 0 | |
| 0x804 | 0 | 0x10000000 | the MADD invalidate bit stays set |
| 0xa74 | 0x00208200 | 0x00208200 | as the init script writes it |

**0x138 is the SGX530's invalidate-done register too.** With the GPU idle, the host's
writes of 1 to `PDS_INV0`-`3` and `PDS_INV_CSC` take 0x138 from 0 to **0x000001ff**, the
same on a second pass (the bits stay set). The microkernel's TA-side wait expects the
SGX545's 0x27 and its 3D-side wait 0x4b. On this core the bits are laid out differently.

**Per invalidate (read only, cumulative).** `inval_probe` now prints 0x138 after each write:
`PDS_INV0` → 0x022, `PDS_INV1` → 0x066, `PDS_INV2` → 0x0ee, `PDS_INV3` → 0x1fe,
`PDS_INV_CSC` → 0x1ff. So `PDS_INVn` sets b(n+1) and b(n+5), and the CSC sets b0.

**The TA-side wait, fixed.** The microkernel's TA side requests `PDS_INV0` and the CSC, so on
this core it should wait for 0x23, not the SGX545's 0x27 (b2 comes only from `PDS_INV1`).
I rebuilt the image with `INV_DONE_TA = 0x23` (sgx530 07af02bb, golden c2b94105) and packed
it with mkukfw (features 0x8, pdtable patch, core rev 0x10205). Then I ran the kick with every
client form on. Control-block words 0x28 and 0x29 stay 0: no bounded wait times out. 0x138
reads 0x1dc mid-kick, which is 0x1ff with 0x23 acknowledged. The TA still stalls the same
way: TA clock on, region headers 0, no events, no BIF fault.
