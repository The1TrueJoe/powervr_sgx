# powervr_sgx

> **Status:** an out-of-tree driver.

Open-source graphics for the PowerVR **SGX** GPUs: a Linux kernel driver, the GPU's firmware
and a Mesa OpenGL driver, all built from this repository. It brings hardware 3D to old Intel
Atom netbooks and nettops with **Cedarview** graphics (GMA 3600/3650) and to **Intel Atom
CE5300** boards. No Intel or Imagination binary is needed anywhere: not in the kernel, the
firmware or userspace.

## What works

| GPU | Hardware | Status |
|---|---|---|
| SGX545 | Intel Cedarview: Atom N2600, N2800, D2500, D2550, D2700 | **Supported.** Debian 13 packages |
| SGX545 | Intel Atom CE5300 / CE5310 | **Supported** |
| SGX545 | Intel Clover Trail: Atom Z2760 | **Works**, tested on an HP ElitePad 900; manual setup ([notes](https://github.com/The1TrueJoe/powervr_sgx/blob/dev/docs/porting/cores/clovertrail.md)) |
| SGX530 | TI OMAP3 (OMAP3430, OMAP3630, DM3730), AM335x, AM3517 | **In bring-up:** the kernel driver powers the GPU and reads its ID; no frames yet |
| SGX535, SGX540, SGX543, SGX544, SGX554 | Intel Poulsbo, TI OMAP4/5, Allwinner A31/A83T, and others | Wanted: see [Contributing](#contributing) |

On the SGX545:

- **OpenGL 2.1 and OpenGL ES 2.0**, with EGL, GBM, GLX and DRI3, in 64-bit and 32-bit programs.
- **Desktops:** Xorg with glamor and Xfce, on the GPU.
  When one program hangs the GPU, the driver recovers it and the desktop keeps running.
- **Coming:** OpenGL ES 3.0, then OpenGL 3.1 ([docs/GL3.md](https://github.com/The1TrueJoe/powervr_sgx/blob/dev/docs/GL3.md)), and hardware
  H.264 video decoding on Cedarview.

## Results

Measured on a CE5300 board, against Intel's own Linux driver for this GPU:

| | This driver | Intel's driver |
|---|---|---|
| dEQP-GLES2 tests passed (the same Mesa on each kernel driver and microkernel) | **16,587**, with 0 GPU hangs | 16,566 |
| One render job, submit to finish | **315 µs**, one interrupt | 344 µs, three interrupts |
| Minecraft 1.7.10, standing in a world | **12.6 to 13.4 FPS**, 64-bit Java on desktop OpenGL | 12.8 FPS, 32-bit Java through a GL-to-GLES translator |
| Desktop OpenGL and X11 | **Yes** | No: OpenGL ES on a bare framebuffer only |

Minecraft also runs in its world for 15 minutes without a single GPU recovery.

### Games

On Atom N2600 netbooks:

| Game | Result |
|---|---|
| Doom 3 (dhewm3) | About 30 FPS, and smoother than Intel's Windows driver on the same netbook |
| ioquake3, `opengl1` renderer | 60 to 65 FPS, held by vsync |
| OpenArena, `opengl1` renderer | About 45 FPS in the timedemo |
| Counter-Strike 1.6 | 37 to 40 FPS on de_dust2 |
| Quake (QuakeSpasm) | 37 to 40 FPS |
| Yamagi Quake II | 40 to 70 FPS |
| Geometry Dash (Wine) | 58 to 60 FPS |
| Super Mario 64 PC port | A steady 30 FPS, the game's own cap, at 1024x600 |
| Minecraft 1.12.2 | 25 to 30 FPS |
| Minecraft 1.16.5 with OptiFine | Runs |
| SuperTuxKart | 15 to 20 FPS |
| Call of Duty (Wine), PCSX-R with its OpenGL plugin | Run |

Something wrong in a game? [Open an issue](https://github.com/The1TrueJoe/powervr_sgx/issues)
with what you ran and what you saw.

## Installing

On Debian 13 with a Cedarview computer, the packages come from our apt repository and stay up
to date with `apt upgrade`. [docs/INSTALL.md](docs/INSTALL.md) has every step: checking your
GPU, adding the repository and its key, the CE5300 steps, and how to remove it all again.
Debian's own kernel and Mesa stay installed, so the boot menu always has a way back.

The packages are also on the [Releases page](https://github.com/The1TrueJoe/powervr_sgx/releases)
as `.deb` files.

## Trying it without installing

[docs/TESTING-CEDARVIEW.md](docs/TESTING-CEDARVIEW.md) walks you through the test USB stick:
write it, boot your Cedarview computer from it, and attach the results file to
[issue #5](https://github.com/The1TrueJoe/powervr_sgx/issues/5). Nothing is installed.

## Known issues

- Some Cedarview panels stay dark with `gma500`, Debian's kernel as well as ours. The test
  stick records why.
- Half-Life: Source and Portal show a black screen, and GTA Vice City doesn't render yet.
- PPSSPP, DuckStation, and GPU acceleration in Firefox and Chrome need OpenGL ES 3.0 or
  OpenGL 3, which are in progress. Minecraft 1.17 and newer need OpenGL 3.2, which isn't
  planned.
- The full list is in [docs/MAIN-MERGE.md](docs/MAIN-MERGE.md#known-issues-to-state-with-the-merge).

## Contributing

Contributions are welcome, including support for other PowerVR SGX cores. Start with
[CONTRIBUTING.md](CONTRIBUTING.md) and the porting guide, [docs/porting/](docs/porting/README.md).

## For developers

| Part | Where | What it is |
|---|---|---|
| Kernel driver | [`drm/`](drm/) | `powervr_sgx`, a new DRM driver (GEM, VM, a `drm_sched` job queue, runtime PM, devcoredump). On Cedarview it takes the GPU over from `gma500` through an auxiliary device. |
| Firmware | [`ukernel/`](ukernel/) | An open microkernel for the GPU's embedded processor, written in USSE assembly and built with our own assembler. It replaces Imagination's microkernel. |
| GL driver | [`mesa/`](mesa/) | `sgx`, a Mesa gallium driver with a NIR to USSE compiler, kept as a patch series on Mesa 26.2.5. |
| Packages | [`.github/workflows/`](.github/workflows/) | Debian 13 packages built in CI: `mesa-sgx`, `sgx545-firmware`, and a kernel for Cedarview. |
| Test sticks | [`kit/`](kit/) | The live Debian USB image for Cedarview testers. |
| Tools | [`tools/`](tools/) | The ISA assembler and disassembler, the offline microkernel emulator, decoders, frame and blit generators, and hardware tests. |
| Documentation | [`docs/`](docs/) | The hardware as we understand it ([ISA](docs/ISA.md), [state](docs/STATE.md), [microkernel](docs/UKERNEL.md), [uAPI](docs/UAPI.md)), [performance](docs/PERF.md), and the [build log](docs/log/BUILDLOG.md). |

The cores marked USSE in [docs/porting/](docs/porting/README.md) share an instruction set with
the SGX545, so most of a port is that core's facts and its platform glue. One kernel driver,
one Mesa driver and one microkernel source cover the family: a new core is added as data, not
as a copy of the code.

`kernel/` and `libpvr/` are the earlier path: Intel's GPL DDK kernel driver (`pvrsrvkm`)
ported to modern Linux, and our own client library. The current stack doesn't use them.

## Licensing

- `drm/` (the `powervr_sgx` kernel driver): GPL-2.0-only; its uAPI header is
  GPL-2.0-only WITH Linux-syscall-note, or MIT.
- `kernel/src/`: GPL-2.0, Imagination Technologies, as published by Intel.
- `libpvr/`: GPL-2.0-only (built against and generated from `kernel/src`'s headers).
- Everything else here (the microkernel, the Mesa driver, the tools, the test sticks): MIT.

Compatibility patches under `video/patches/` retain their upstream licenses.

## Cedarview H.264 decoding

The optional [VAAPI driver and broker](video/README.md) use the separately
proposed gma500 MSVDX host. They support CPU-copy playback; direct import
additionally needs the SGX read-only import and player compatibility changes.
Firmware is supplied separately and the private kernel interface is opt-in.
