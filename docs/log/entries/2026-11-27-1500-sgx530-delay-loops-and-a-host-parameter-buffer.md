# SGX530: delay loops in the pixel task, and a host-written parameter buffer

**2026-11-27, our stack on the OMAP3630 test board, RAM only. Released clean.** Joseph: "keep
working on the 530". Two leads, in order. New blit knobs, all off by default:
- `-H`: a host parameter buffer for tile (0,0);
- `-I`: its region-array entry's low bits;
- `-J off=val,…`: words poked into it;
- `-D` now also prints the destination's last word.

## 1. Multi-tile via the pixel task's timing (C, a dead end so far)

The end-of-tile count is read from a counter the end-of-tile program increments, with the
microkernel timer started from the host as in
[1430](2026-11-27-1430-sgx530-event-stall-write-retire-null.md).

| pixel task | end-of-tile tasks | PBE |
|---|---|---|
| `limm r0` (the plain clear) | 1 | writes tile 0 |
| `phas.i`, a counted loop (`test.isubu16` + `(p0) br`, on pa1 or r1), 1 to 16384 times, then `limm r0` | 1 (2 once, at 4096) | — |
| `phas.i`, a load/add/store counter on its own cache line, then `limm r0` | **15** | with PBE emits in the end-of-tile task: 3 tasks, nothing written |

- A delay alone doesn't let the end-of-tile events through.
- The counting task's 15 is real: 1300's 13-18 wasn't the two counters sharing a cache line.
- But in that mode the PBE writes nothing, and with emits in the end-of-tile task only 3 run.
  So the event flow and the PBE write pull against each other.
- 1300's search for the last-tile flag (in1 bits → b27) was invalid: only one end-of-tile ran
  then. Repeated with the counting pixel task, no bit completes the job, but there's no PBE
  output to judge it by either.
- `DMS_CTRL`'s EDM-task limit (0x3f, 1) and its pixel-task limits don't change the stall.

Our k_transfer writes the transfer command's "3D field" (the SGX545's meaning) into 0xa74
b22-26. On the SGX530 those bits are the pixel partitions and part of the vertex partitions.

## 2. Geometry without the TA: a host parameter buffer (C, in progress)

The stock frame's parameter buffer, read back on 11-20 (O, register and memory values only):
- **The 3D's region array** (0x408): 12-byte entries `0x10000000 | tile << 12`, 0, 0; b31 on
  the last.
- **The TA's region headers** (0x21c): position (x/8 << 16 | y/8), control-stream offset
  (0x1000 + 0x40 × tile), 0; b31|b28 on the last.
- **Control streams** (page 1): `0x44a8ffff` + the object's dword offset, ended by 0xc0000000.
- **Object blocks** (page 0):
  - the three PDS binding words;
  - the per-vertex TSP data, (1.0, colour);
  - ISPA, then ISPB when ISPA b9 is set;
  - 0x46124612, then 0x42 (0x41 for the second triangle), then 0x40000000 (0);
  - three vertices of XY (12.4 fixed point, +0x4000 bias) and Z (float).
- Every offset is from `3D_REQ_BASE`, which is the buffer's base (0x08800000 there).

Built the same way in our VM, at the 1 MiB base of the background block (`-B 0xff000`: host
buffer 0x01900000 = `3D_REQ_BASE`). One object: the clear's PDS words, the clear colour, a
lower-left triangle of tile (0,0) at Z 0.5. Driven through k_transfer:
- `probe_xfer` set the render type (0x404) to 0 and 0x408 to the region array;
- `probe_init` set 0x21c to the region header;
- the background object was on or off (0x4bc).

What it showed:
- **Render type 0 makes the ISP read the buffer.** Before anything was mapped at the base, it
  faulted at `3D_REQ_BASE`'s first page (requestor b8). With the host buffer there, nothing
  faults. `ISP_STATUS2` reads 0x001f0000: tile (0,0), primitive field 31.
- **Nothing is written, and the job times out.**
- **The ISP doesn't follow our pointers.** Pointing any one of these at unmapped space faults
  nowhere and changes nothing:
  - the region header's stream offset;
  - the region-array entry's low bits;
  - the control stream's object offset;
  - 0x21c itself.

  So the entry format, or what links the region array to the streams on this core, isn't
  what's written here.
- A transfer with the same host buffer but no overrides still writes its tile (236/256 right)
  at the moved destination.

## Next

- **Lead 2:** what the 3D reads in `3D_REQ_BASE`'s first page. Poke page-0 words one at a time
  and watch `ISP_STATUS2` / `ISP_STATUS1` (the control-stream position) for one that moves. Or
  take the field meaning of a region-array entry from an observed stock render with another
  tile count, observation only.
- **Lead 1:** a pixel task that keeps events flowing and leaves the PBE path intact. The
  difference between the counting task and the plain clear is r1/r2 traffic plus a
  read-modify-write.
