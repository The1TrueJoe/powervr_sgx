# Agent brief: powervr_sgx, an out-of-tree GPU driver for Cedarview and CE5300

**Read this first if you are a session working on `The1TrueJoe/powervr_sgx` (branch `dev`; `main` is the stable branch).**
It says what the project is for, where it stands, the standards the work has to meet,
and what comes next. It does not replace `docs/`. `docs/` says how the hardware works;
this says what we build with that knowledge and the rules that build follows.

---

## 1. The goal

Old netbooks and nettops with **Intel Cedarview (GMA 3600/3650: Atom D2500/D2550/D2700/N2600/N2800)**,
and CE5300/CE5310 boards, get GPU acceleration from this **out-of-tree** driver stack: a
kernel package or a DKMS module, the open microkernel, and Mesa with the sgx driver, from
this repository's releases and apt repository. The project is maintained for the people who
use it.

The work is held to the standards of an upstream DRM and Mesa driver (§3), because that is
what makes it reliable and maintainable, not because it is waiting for a merge. Mesa
realistically won't take this driver. Kernel upstreaming is being discussed with the
maintainers; don't plan work around either outcome.

Next goals: **OpenGL ES 3.0**, then desktop **OpenGL 3.1** (`docs/GL3.md`). OpenGL 3.2 is
not planned, since no hardware path for geometry shaders is known.

---

## 2. Where the project is now (v0.2.2, 2026-11-22)

| Piece | Where | State |
|---|---|---|
| Kernel driver | `drm/powervr_sgx/` | `powervr_sgx`: GEM shmem, drm_gpuvm, drm_sched, syncobj, runtime PM, devcoredump, hardware recovery. On Cedarview it binds to an auxiliary device that `gma500` publishes (`drm/gma500/`). Shipped as a Debian kernel package and as DKMS (`drm/dkms/`). |
| Microkernel | `ukernel/` | The open microkernel, in USSE assembly, built by our own assembler. It is the only microkernel the stack uses; it ships as `sgx545-firmware`. |
| Mesa driver | `mesa/` | `sgx`, a gallium driver with a NIR to USSE compiler, as a patch series on Mesa 26.2.5. OpenGL 2.1, OpenGL ES 2.0, EGL, GBM, GLX and DRI3. dEQP-GLES2: 16,587 passes. |
| GLES 3.0 | `mesa/`, `docs/GL3.md` | In progress, much of it in a contributor's series (issue #16). New features stay behind `SGX_DEBUG` switches until they pass on hardware. |
| Video decode | PRs #37 to #40 | Hardware H.264 decoding for Cedarview through VA-API, being contributed. Not merged yet. |
| Test sticks | `kit/` | A live Debian USB image for Cedarview testers (`docs/TESTING-CEDARVIEW.md`). |
| Earlier path | `kernel/`, `libpvr/` | Intel's GPL DDK kernel driver (`pvrsrvkm`) ported to modern Linux, and its client library. The current stack doesn't use them. Don't extend them. |

Other cores: the **SGX530** (TI OMAP3) is in bring-up and paused: the kernel driver powers
the core and reads its ID, but the microkernel's first frame doesn't finish yet. Clover Trail
(Atom Z2760) runs for a contributor (`docs/porting/cores/clovertrail.md`).

### Hardware facts that drive the design

- **CE5300/CE5310:** the SGX545 is its own PCI function, `01:02.0 8086:089b`. The display is a separate function, driven outside this repository. Core rev **1014**.
- **Cedarview:** the SGX sits **behind the same PCI function and BAR as the display engine**, which `gma500` owns. Core rev **10131**.

---

## 3. The standards the work must meet

### 3.1 Kernel uAPI

Follow the conventions of `include/uapi/drm/pvr_drm.h`:

- No pointers in structs. User pointers are `__u64`. All members type-aligned, structs padded to 64 bits, explicit padding, **identical layout on i386 and x86_64**, so no compat layer is needed.
- Every `flags`/`pad`/reserved field is checked to be zero, so it can be extended later.
- Arrays are passed as `{stride, count, __u64 ptr}` objects, so structs can grow.
- Render-node ioctls are marked `DRM_RENDER_ALLOW`. Nothing global or modeset-related goes on the render node.
- Header license: `(GPL-2.0-only WITH Linux-syscall-note) OR MIT`, so Mesa can carry it.
- Full kerneldoc on every struct and ioctl. `docs/UAPI.md` is the design record; change it with the code.

### 3.2 Standard kernel helpers

Use the standard helpers, not hand-rolled equivalents: GEM shmem and PRIME, `drm_gpuvm` with `drm_exec`/`dma_resv`, `drm_sched` with job timeouts feeding recovery, `dma_fence` from the completion IRQ, syncobj (including timeline), runtime PM, debugfs, devcoredump, and `request_firmware()` with a version check. Fences must signal in bounded time, with no allocation or blocking in the signaling path (`dma_fence_begin/end_signalling`). The upstream Rogue driver (`drivers/gpu/drm/imagination`) is the model for the shape, not the code.

### 3.3 Isolation and security

The kernel **owns every firmware-visible structure** (host control block, kernel CCB, render and transfer contexts, RT data, parameter buffers, free lists, sync words). Userspace submits only the client part of a command, as a validated stream, with GPU addresses **inside its own VM**. One client's commands must never make the microkernel read or write another client's memory. Treat every userspace-supplied address as hostile. `docs/UAPI.md` has the threat model; keep it current.

### 3.4 Firmware

The open microkernel is the firmware. It needs a permissive license, source in-tree, a **reproducible build** from `ukernel/build.py`, and a versioned host interface that the kernel checks at load. Validate changes on **both core revs** (1014 and 10131) when they touch shared paths.

### 3.5 Licensing boundaries

- `drm/` is **GPL-2.0-only**. Everything else is **MIT** unless the file says otherwise (`LICENSE`).
- `kernel/src/` is GPL-2.0 (Imagination via Intel). Nothing copied or mechanically derived from it goes into Mesa: no headers, tables or struct layouts. Mesa-side hardware definitions are our own, written from `docs/`.
- **Never commit** vendor binaries, the vendor microkernel, firmware we may not redistribute, or decompiler output.

### 3.6 Clean room

Follow `docs/CLEANROOM.md`. Shipping code is written from `docs/`. Before code relies on a hardware fact, the fact goes into `docs/` with how it was established, and it is confirmed by a hardware test.

### 3.7 Mesa driver standards

- Mesa style, meson options, and debug variables in the driver's debug-option table. No environment-variable-only features in default paths.
- Linear scanout first. Implicit sync through `dma_resv` for X/DRI3, explicit syncobj for Wayland.
- **Testing:** dEQP through `deqp-runner` with expectation files, and piglit for desktop GL. Run `meson test --suite sgx` before merging anything under `mesa/src`; a green CI build alone isn't enough.

---

## 4. What desktops need from the driver

| Consumer | Needs | Our status |
|---|---|---|
| Wayland compositors (wlroots, Weston, Mutter), Xorg modesetting with glamor | KMS + GBM + dma-buf + render node + fences, GLES 2.0 | Works |
| Qt 6 / LXQt | GLES 2.0 | Works |
| GTK4 GPU renderers | GL 3.3 or GLES 3.0 | Needs GLES 3.0 |
| Firefox WebRender, Chromium GPU | GLES 3.0 or GL 3.2 | Needs GLES 3.0 |
| Minecraft Java 1.17+ | GL 3.2 core | Not planned |
| Video playback | VA-API decode, frames imported as dma-bufs | Being contributed (#37 to #40) |

---

## 5. The work

Done:

- **M0–M3:** GLES 2.0, the open microkernel on hardware, the new kernel driver and uAPI, and the Mesa winsys on it. dEQP-GLES2 is at least as good as on the vendor's stack.
- **M4:** Cedarview through `gma500`'s auxiliary device, released as packages, and tested on testers' machines.
- **M5:** the upstream submission plan (RFC to dri-devel, a Mesa merge request, firmware to linux-firmware) is on hold. The record stays in `docs/DRIVER-UPSTREAM.md`.

Next:

- **M6, desktop GL 2.1 completeness:** fix what piglit's `gpu` profile and testers' games find.
- **M7, GLES 3.0:** integer ALU, texelFetch/textureSize, integer and float textures, float render targets, MRT, sRGB, transform feedback, texture arrays, MSAA, UBOs, instancing. The hardware questions still open are in `docs/GL3.md`. Gate: dEQP-GLES3 triaged.
- **M8, GL 3.1:** TBOs, primitive restart, rect textures, sync objects.
- **Video decode:** review and land #37 to #40.
- **Other cores:** the SGX530 when there's new information on its first frame; new cores through `docs/porting/`.

The point of GL3 on this GPU is that apps start and run correctly. It won't be fast.

---

## 6. Rules for sessions on this repo

- Follow `CONTRIBUTING.md`: branch from `dev`, PRs to `dev`, SPDX lines, sign-off, American English, short comments.
- Say which board and core revision a hardware result comes from. Run tests that may hang the GPU only where the board can be power-cycled, and serialize GPU runs on a shared board.
- Log hardware milestones in `docs/log/`.
- Nothing in the default build or packages may need the vendor's closed stack.
- When a decision closes off options (uAPI shape, firmware host interface, driver names), write it into `docs/` with the reasoning and check it with Joseph first.

---

## 7. Open questions

1. GLES 3.0: the hardware questions listed as open in `docs/GL3.md`.
2. Some Cedarview panels stay dark with `gma500`; the test stick records why.
3. SGX530: how the microkernel's state task hands its words to the MTE, so the first object reaches the parameter buffer.
4. Video decode: how the decoder's frames and the GPU's imports fit together on Cedarview (#37 to #40, issue #15).

---

## 8. References

- Linux DRM uAPI docs, "Open-Source Userspace Requirements" and render nodes: https://docs.kernel.org/gpu/drm-uapi.html
- The upstream Rogue driver, the design model: https://lkml.indiana.edu/2311.2/07062.html ; uAPI `include/uapi/drm/pvr_drm.h`
- Imagination's SGX545 announcement: https://us.design-reuse.com/news/22405/graphics-ip-core-directx-opengl-opencl.html
- SGX545 Windows GL 3.0 capability report: https://feedback.wildfiregames.com/report/opengl/device/PowerVR%20SGX545
- In-repo: `README.md`, `CONTRIBUTING.md`, `docs/README.md`, `docs/CLEANROOM.md`, `docs/UAPI.md`, `docs/GL3.md`, `docs/UKERNEL.md`, `docs/STATE.md`, `docs/ISA.md`, `docs/log/`
