# Clover Trail (Atom Z2760): the SGX545 behind the display engine

The platform facts for `powervr_sgx` on Intel Clover Trail. The core is the SGX545 that
[kernel.md](../kernel.md) and [docs/UKERNEL.md](../../UKERNEL.md) already describe; what
is new is where its window lives, how its interrupt reaches the CPU, who powers its
island, and that nothing in mainline drives the display it shares a PCI function with.

Everything marked V was measured on one HP ElitePad 900 G1 (Atom Z2760, 2 GiB, Debian
13 i386, Linux 7.3.4). How to boot Linux on that tablet, and the logs behind these
numbers: <https://github.com/servidorjarvis1-svg/elitepad-900-linux>.

Tags follow [state.md](../state.md)'s method, with this page's own scale, and every fact
names its source:

| Tag | Meaning |
|---|---|
| **V** | Verified on the tablet: a register read, `dmesg`, `/proc`, config space, a test run, or the machine's own ACPI tables |
| **D** | Derived from a V fact, or from code and documentation in this tree or in mainline Linux |
| **A** | From the public Clover Trail+ Android kernel (Lenovo K900, `android_kernel_lenovo_redhookbay`); not confirmed on the tablet |
| **G** | A single public source, not measured |

## Identity

| Fact | Value | Tag | Source |
|---|---|---|---|
| PCI function | `0000:00:02.0`, `8086:08cf`, class `0x030000`, revision `0x05` | V | `lspci -nn`, config space |
| PCI command | `0x0007` (I/O, memory, bus master) | V | config space |
| BAR 0 | `0xff200000`-`0xff2fffff`, 1 MiB | V | `lspci -v`; the DSDT's `_CRS` for `PCI0` reserves the same range |
| BAR 1 | `0x1000`-`0x1007`, 8 bytes (I/O) | V | `lspci -v` |
| BAR 2, 3 | 256 MiB aperture (reassigned by Linux), 256 KiB GTT | V | `lspci -v` |
| Interrupt Pin | `0`: no INTx line | V | config space offset `0x3D` |
| MSI | capability present | V | `lspci -vv` |
| `_PRT` entry | INTA → GSI 16, never raised (see [the interrupt](#the-interrupt-the-display-engines-msi)) | V | DSDT, test run |
| ACPI device | `\_SB.PCI0.GFX0`; `_PS0` writes `SSID = 0x18DC103C` and `\_SB.GDDS = 0`, `_PS3` writes `GDDS = 3`; no `_PR0`/`_PR3`, no power resource, no GPU register | V | DSDT |
| CPU | family 6 model `0x35` (`INTEL_ATOM_CLV`) | V | `/proc/cpuinfo` |
| `CORE_ID` | `0x01150000` | V | register read (`sgx-ident`), the driver's log |
| `CORE_REVISION` | `0x0001000e` (1.0.14, the CE5300's) | V | register read, the driver's log |
| `CLKGATESTATUS` before the driver | `0` | V | register read |
| `BIF_CTRL` before the driver | `0x0000ff00` | V | register read |
| Core clock | 533 MHz | G | notebookcheck, "Intel Atom Z2760" |

The revision being the CE5300's is why the clock lives in `struct pvrsgx_part` and not in
the core table: both parts find the same core entry, 1.0.14, and differ in their clock.
The driver logs

    SGX core id 0x01150000 revision 0x0001000e: Intel Clover Trail (SGX545 1.0.14, 533 MHz)

## The window

`BAR 0` is one MiB and holds two engines:

| Range | Owner | Tag | Source |
|---|---|---|---|
| `0xff200000`-`0xff27ffff` | display engine | D | gma500's layout on Medfield and Cedarview |
| `0xff280000`-`0xff2fffff` | SGX545 (`PVRSGX_CLV_SGX_OFFSET`, 0x80000) | V | `CORE_ID` and `CORE_REVISION` read at `+0x1c`/`+0x20` |

The offset is the one Medfield and Cedarview use (`MRST_SGX_OFFSET` in gma500's
`psb_drv.h`). Offsets in the driver tables are from the start of the window, as on the
CE5300.

In the display half the driver touches three registers, and in them only bit 18:
`INT_ENABLE` `0x20A0`, `INT_IDENTITY` `0x20A4` and `INT_MASK` `0x20A8` (the
`PVRSGX_VDC_*` defines; gma500's `PSB_INT_*_R` and `_PSB_IRQ_SGX_FLAG`). Nothing else
below `0x80000` is read or written.

## Why the driver is a PCI driver here, not gma500's auxiliary device

On Cedarview, mainline gma500 owns the PCI function and publishes the SGX as the
auxiliary device `gma500_gfx.sgx`; `pvrsgx_aux.c` binds to that. On Clover Trail the
display engine has no mainline driver (gma500 never supported Cloverview), so there is
nothing to publish the SGX: the driver claims `8086:08cf` itself, maps `BAR 0` with
`pcim_iomap_region()` and adds the window offset.

Two consequences, both deliberate:

- the driver stays **render-only**. It doesn't call
  `drm_aperture_remove_conflicting_pci_framebuffers()`, so the firmware's framebuffer
  survives and `simpledrm` drives the screen ([the screen](#the-screen));
- the display half of `BAR 0` is mapped, and only the three interrupt registers above
  are used.

A part is data: `pvrsgx_parts[]` in `pvrsgx_drv.c` carries the core number, the window
offset, the clock and the flags (`PVRSGX_PART_ISLAND_ALWAYS_ON`,
`PVRSGX_PART_VDC_IRQ`), and the PCI id table's `driver_data` indexes it.

## The interrupt: the display engine's MSI

| Fact | Tag | Source |
|---|---|---|
| The function has no INTx line (Interrupt Pin 0) and an MSI capability | V | config space |
| With a handler on the `_PRT`'s GSI 16 the microkernel ran, jobs completed by polling only, and `EVENT_STATUS` held `0x80004000` with no interrupt taken | V | test run, `/proc/interrupts` |
| The SGX's request is bit 18 of the display's interrupt registers, and the display signals it by MSI | V | uAPI suite 40/40, 794 MSIs on the first run that took it this way |
| The bit is latched in `INT_IDENTITY` (write 1 to clear), and a latch left set blocks every later MSI | D | gma500 (`psb_irq.c`); the stall seen without the clear |

How the driver handles it (`pvrsgx_device.c`):

- the bit is unmasked and enabled only once `request_threaded_irq()` has succeeded, and
  disabled and masked before `free_irq()`;
- the handler clears the source (`EVENT_HOST_CLEAR`), then the latch, and clears the
  latch also on the path where the core is powered off;
- the MSI is edge-signalled, so an event that lands between the two clears sends no MSI
  of its own. The handler reads `EVENT_STATUS` again after clearing the latch, up to four
  times. **It happens:** in one benchmark run, 2 of 39 053 interrupts were found that
  way (debugfs `status`, "irqs found behind the display's latch"). Without the re-read
  those two events would have waited for the next one.

## Power: the island is the P-Unit's

| Fact | Tag | Source |
|---|---|---|
| `GFX0._PS0` writes a subsystem id and a firmware variable, and powers nothing | V | DSDT |
| The island is up at boot: the core answers its id registers before the driver loads | V | register read |
| The P-Unit is reached over the message bus in `00:00.0`'s config space (`0xD0`/`0xD4`/`0xD8`) | D | mainline `iosf_mbi` |
| The graphics island is one LSS of the P-Unit's APM block: APM base `OSPM_APMBA` (0x7a), OSPM base `OSPM_OSPMBA` (0x78), graphics island bit 0, two bits per LSS, 0 = D0i0 and 3 = D0i3 | A | `arch/x86/platform/intel-mid/intel_soc_pmu.[ch]` |
| Nothing in mainline drives that block on Clover Trail | D | mainline tree |

Until an APM hand-over exists, the driver holds the device awake while it is bound
(`PVRSGX_PART_ISLAND_ALWAYS_ON`):

| Behaviour | Where |
|---|---|
| A permanent runtime PM reference: no idle stop, no microkernel stop, no D3 | `pvrsgx_power_init()` / `pvrsgx_power_fini()` |
| Runtime suspend returns `-EBUSY` if it is ever reached | `pvrsgx_runtime_suspend()` |
| System sleep is refused with a warning | `pvrsgx_suspend()` |

Whether the island survives s2idle (the driver never puts the function in D3) is being
measured. If it does, system sleep can be allowed; if not, the driver can let the
machine sleep and report the GPU lost on resume, rather than block suspend.

## The screen

There is no mainline KMS driver for Cloverview's display engine, and EGL's fbdev
platform cannot present through `powervr_sgx` (no `bo_wrap`). The route is the one the
CE5300 board uses: `simpledrm` on the firmware's framebuffer, and kmsro pairing it with
the SGX's render node (`CONFIG_SYSFB_SIMPLEFB`, `CONFIG_DRM_SIMPLEDRM`,
`CONFIG_DRM_FBDEV_EMULATION`; `drm/ci/clovertrail.fragment`).

| Fact | Tag | Source |
|---|---|---|
| Framebuffer 1280x800, 32 bpp, stride 5120 at `0x7f000000`, inside the firmware's reserved `0x7edff000`-`0x7fffffff` | V | `simpledrm` probe, `/proc/iomem` |
| `simpledrm` scans out XRGB8888 only: an ARGB8888 `AddFB` fails with `-EINVAL` (`glmark2-es2-drm` needs `--visual-config a=0`) | V | `glmark2-es2-drm` |
| `simpledrm` advertises only `DRM_FORMAT_MOD_LINEAR`, so GBM's `create_with_modifiers` path (mpv's DRM context) needs the screen's modifier hooks | V | `mpv -v` |
| Xorg's `modesetting` on `simpledrm` runs glamor on the SGX545 | V | `Xorg.0.log`, `glxinfo` |

Measured with this driver and release 0.2.1's Mesa: `glxgears` 62 FPS in X11, mpv
720p30 for 30 s with 3 dropped frames in X11 and 3 on DRM, `glmark2-es2-drm` 69, uAPI
suite 50/50, `powervr_sgx_frame` 100/100.

## On a 32-bit kernel

The tablet runs i386 without PAE: there is no `ZONE_DMA32`, and `__GFP_DMA32` means
lowmem, about 830 MiB of the 2 GiB. With the default allocation a browser took
`LowFree` from 483 MiB to 7 MiB and the kernel OOM-killed Xorg inside
`pvrsgx_rtds_create()`. The `highmem` module parameter lets buffers come from HIGHMEM;
it needs two x86 fixes that are not in mainline (the platform repo above carries them),
so it is off by default.

## Traps

- **D3 is fatal here**: nothing powers the island again. Hence the permanent reference,
  and no `pci_set_power_state()`.
- **Do not sweep MMIO on this SoC.** A read of a register that waits for a handshake
  hangs the CPU with no recovery but a cold boot; seen in the audio block's window
  (`0xFFAE0000`). The driver touches only the registers named on this page.
- **The SCU IPC mailbox (`0xFF11C000`)** resets or powers off the tablet on a write.
- **The core table alone finds the CE5300.** Clover Trail reports the CE5300's
  revision, so without the part's clock the microkernel's timer would assume 400 MHz on
  a 533 MHz core.

## Not in this driver

- APM/P-Unit island control, and with it runtime PM.
- The display: no KMS driver for Cloverview's display engine, and none attempted.
- Any change to the microkernel, the MMU tables or the uAPI: the core is the same SGX545.
