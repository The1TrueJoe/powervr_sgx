# SGX530: the ISP walks a host-written parameter buffer; 0xa74 isn't the stall

**2026-11-27, our stack on the OMAP3630 test board, RAM only. Released clean.** It follows
[1500](2026-11-27-1500-sgx530-delay-loops-and-a-host-parameter-buffer.md): the host buffer at
`3D_REQ_BASE` (0x01900000), driven through k_transfer with render type 0 (`probe_xfer` word 3).

## 0xa74 left alone (C)

A scratch microkernel switch, not committed, kept k_transfer from writing its SGX545-style
field into 0xa74 b22-26. 0xa74 then holds its init value (`probe_a74`). End-of-tile tasks,
counted, with the plain r0 clear:

| 0xa74 | end-of-tile tasks |
|---|---|
| the stock's idle 0x003f8200, or its TA-time 0x083f8200 (both 0 pixel partitions) | 0 |
| 0x083f8200 with 1 pixel partition | 0 |
| 0x083f8200 with 2, 4 or 7 pixel partitions | 1 |

The pixel-partition field must be at least 2 for anything to run. More doesn't lift the
one-end-of-tile stall. k_transfer's write is what currently puts 7 there.

## 0x408 takes the region headers, not the region array (C)

In the stock's register samples, 0x408 is the region headers' address (0x0c800640) when its
3D starts (O, 11-20). It becomes the `0x10000000 | tile << 12` array (0x0c800000) only
afterwards. With 0x408 at our region header (position, control-stream offset, 0):
- **`ISP_STATUS1` reads 0x1000**: the ISP's control-stream position is our stream.
- **The region header's stream offset is followed.** Moving it to 0x80000 faults at
  0x01980000 (requestor b8).
- **The stream's object offset is followed.** Moving it to 0x20000 dwords faults at
  0x01980000.
- **With the background object off (0x4bc = 0) and b27 in the tile word, the render
  completes**, with no timeout or fault. The end-of-tile task runs once and its PBE emits run.
- **But no pixel task runs.** A pixel program that counts itself stays at 0, and nothing is
  written. With the background object on, no end-of-tile runs and the job times out.

Changing the object's PDS control word to the stock's 0x20030001 (b29) changes nothing, and
neither does the stock's other object form (ISPA 0x01e00000, stream entry 0x4468ffff).

## Where it stands

The 3D, with no TA, reads a parameter buffer the host wrote: region header, control stream,
object. That's the path a host-built frame needs. What's missing is rasterization: the object
gives no pixels. The next things to check are the object's own words, all of them stock values
whose fields are G:
- 0x46124612;
- 0x42 / 0x41;
- the 0x40000000 before the vertices;
- the vertex format (12.4 fixed-point XY, float Z).

Also open: why the background object on makes it hang.
