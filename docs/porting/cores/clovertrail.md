# Clover Trail (Atom Z2760): the SGX545 behind the display engine

The platform facts for `powervr_sgx` on Intel Clover Trail, as used by the local
`clovertrail` branch. The core is the SGX545 that [kernel.md](../kernel.md) and
[docs/UKERNEL.md](../../UKERNEL.md) already describe; what is new is where its window
lives, who powers its island, and that nothing in mainline drives the display it shares
a PCI function with.

Tags follow [state.md](../state.md)'s method, with this page's own scale:

| Tag | Meaning |
|---|---|
| **V** | Verified on the HP ElitePad 900 G1 (Atom Z2760): `dmesg`, `journal`, or the machine's own ACPI tables |
| **D** | Derived from a V fact, or from code and documentation in this tree |
| **G** | A guess or a single public source, to be confirmed by the first register reads |
| **U** | Unknown: not measured yet |

Nothing on this page was measured with a register read. The branch is prepared so that
the first boot can measure it, not because it was measured.

## Identity

| Fact | Value | Tag |
|---|---|---|
| PCI function | `0000:00:02.0`, `8086:08cf`, class `0x030000` (VGA) | V |
| BAR 0 | `0xff200000`-`0xff2fffff`, 1 MiB | V |
| BAR 1 | `0x1000`-`0x1007`, 8 bytes (I/O) | V |
| BAR 2 | 256 MiB aperture (reassigned by Linux); BAR 3 256 KiB GTT | V |
| Interrupt | GSI 16 (`0x10`), INTx A: `_PRT` of `PCI0`, source `Zero`, `_ADR 0x00020000` | V |
| ACPI device | `\_SB.PCI0.GFX0`; `_PS0` writes `SSID = 0x18DC103C` and `\_SB.GDDS = 0`, `_PS3` writes `GDDS = 3`. No `_PR0`/`_PR3`, no power resource, no GPU register | V |
| CPU | family 6 model `0x35` (`INTEL_ATOM_CLV`, Cloverview) | V |
| `CORE_ID` | expected `0x0115xxxx` (SGX545); `xxxx` is CONFIG | **U** |
| `CORE_REVISION` | **U** — this is what the first boot reads | **U** |
| Core clock | 533 MHz (notebookcheck, "Imagination PowerVR SGX545") | G |

The DSDT is in the work tree at `elitepad-dev/ref/acpi/`. The register facts agree with
the machine's `_CRS` for `PCI0`, which reserves `Memory32Fixed(RW, 0xFF200000, 0x00100000)`
for this function.

## The window

`BAR 0` is one MiB and holds two engines:

| Range | Owner | Tag |
|---|---|---|
| `0xff200000`-`0xff27ffff` | display engine | D |
| `0xff280000`-`0xff2fffff` | SGX545 (`PVRSGX_CLV_SGX_OFFSET`, 0x80000) | G |

The offset is the one Medfield and Cedarview use: `MRST_SGX_OFFSET` in gma500's
`psb_drv.h`, passed by `mdfld_device.c` and `cdv_device.c`. Clover Trail uses the same
display engine design, so the guess is strong, but it is a guess: nothing has read
`0xFF28001C` on this tablet. `sgx-ident` is what turns it into a fact (below).

Offsets in the driver tables are from the start of the window, as on the CE5300. The
registers the driver reads before the reset sequence are the ones
`drm/powervr_sgx/pvrsgx_regs.h` names: `CLKGATECTL` 0x0000, `CLKGATESTATUS` 0x0008,
`CORE_ID` 0x001c, `CORE_REVISION` 0x0020, `SOFT_RESET` 0x0080, `EVENT_STATUS` 0x012c,
`EVENT_HOST_ENABLE` 0x0130, `BIF_CTRL` 0x0c00. All of them fall inside the mapped page.

## Why the driver is a PCI driver here, not gma500's auxiliary device

On Cedarview, mainline gma500 owns the PCI function and publishes the SGX as the
auxiliary device `gma500_gfx.sgx`; `pvrsgx_aux.c` binds to that. On Clover Trail the
display engine has no mainline driver (gma500 never supported Cloverview), so there is
nothing to publish the SGX: the driver claims `8086:08cf` itself, maps `BAR 0` with
`pcim_iomap_region()` and adds the window offset. Nothing else in the probe changes.

Two consequences, both deliberate:

- the driver must stay **render-only**. It doesn't call
  `drm_aperture_remove_conflicting_pci_framebuffers()`, so the firmware's framebuffer
  survives and a KMS driver can be built on it (`simpledrm`, see
  [the screen](#the-screen)). Removing it would take the console away with nothing to
  replace it;
- claiming the function means the display half of `BAR 0` is mapped but never touched.
  No code path in the driver refers to an offset below 0x80000, and `sgx-ident` reads
  only the eight named registers.

A part is data, not a separate driver: `pvrsgx_parts[]` in `pvrsgx_drv.c` carries the
core number, the window offset and the power flags, and the PCI id table's
`driver_data` is an index into it.

## Power: the island is the P-Unit's

`GFX0._PS0` writes a subsystem id and a firmware variable; it powers nothing. The
graphics island is one LSS of the P-Unit's APM block, reached over the message bus in
`00:00.0`'s config space (`0xD0`/`0xD4`/`0xD8`), and nothing in mainline drives it for
Clover Trail. With the GOP having scanned out, the island is almost certainly up (the
display is alive on the same island), and the first `CORE_ID` read confirms it.

Until an APM hand-over exists, the branch takes the conservative route:

| Behaviour | Where |
|---|---|
| Runtime PM is held off with a permanent reference: no idle stop, no microkernel stop, no D3 | `pvrsgx_power_init()` / `pvrsgx_power_fini()` in `pvrsgx_pm.c` |
| Runtime suspend returns `-EBUSY` if it is ever reached | `pvrsgx_runtime_suspend()` |
| System sleep is refused with a warning: resume could not power the island again | `pvrsgx_suspend()` |

That is a bring-up state, not the destination: it costs the tablet its sleep while the
module is loaded, and it is the one thing to remove first when APM support lands. The
APM block is `base + 0x00` (CMD) and `base + 0x04` (STS), two bits per LSS, both bases
read from the P-Unit (`OSPM_APMBA` 0x7a, `OSPM_OSPMBA` 0x78); `APM_GRAPHICS_ISLAND` is
bit 0. `0` is D0I0 (on), `3` is D0I3 (off).

## Register reads: the whitelist

Only these, one at a time, and only reads:

| Offset | Register | What it says |
|---|---|---|
| 0x0c00 | `BIF_CTRL` | 0 when idle; a stuck bit means a previous owner left it mid-operation |
| 0x0130 | `EVENT_HOST_ENABLE` | which events the host listens for; reset state is 0 |
| 0x012c | `EVENT_STATUS` | `SW_EVENT` (bit 14) set means a microkernel is already running |
| 0x0080 | `SOFT_RESET` | 0 when the cores are out of reset |
| 0x0020 | `CORE_REVISION` | `0x0001000d`/`0e` would be Cedarview/CE5300's revision |
| 0x001c | `CORE_ID` | `0x0115xxxx` confirms the SGX545 |
| 0x0008 | `CLKGATESTATUS` | bit 5 (`DPM_CLKS`) up means the DPM's clocks are running |
| 0x0000 | `CLKGATECTL` | gating mode of every unit; the reset value is stable |

Never, on this part: anything below 0xff280000 (the display engine), any **write** to
the SGX window, `0xFF119000` (`RBPS`) and `0xFF13F000` (`GSTS`, panel and backlight
sequencing in the DSDT), `0xFF11C000`-`0xFF11DFFF` (SCU IPC mailbox: a write there can
reset or power off the tablet), and no sweeps of any range. A sweep of a display
register range hung a CE5300 board; an SGX that hangs can hang the bus, and only a cold
boot clears it.

## The screen

There is no mainline KMS driver for Cloverview's display engine, and EGL's fbdev
platform cannot present through `powervr_sgx` (the winsys has no `bo_wrap`, so it can't
wrap `/dev/fb0`; `sgx_frame.c` answers `-ENOTSUP`). The route is the one the project
already uses on its CE5300 board: `simpledrm` on the firmware's framebuffer, and kmsro
pairing it with the SGX's render node (`mesa/patches/0004` lists `simpledrm` and takes
render nodes whose driver is `powervr_sgx`; a platform display finds its render node
that way). Kernel side: `CONFIG_SYSFB_SIMPLEFB`, `CONFIG_DRM_SIMPLEDRM`,
`CONFIG_DRM_FBDEV_EMULATION`; `FB_EFI` may stay, and only has to go if `simpledrm` does
not appear.

The framebuffer is 1280x800, 32 bpp, stride 5120 at `0x7f000000` (4032 KiB), inside the
firmware's reserved `0x7edff000`-`0x7fffffff`. 1280 x 4 = 5120 is a multiple of the
winsys' 128-byte stride requirement.

## What the branch changes

| File | Change |
|---|---|
| `pvrsgx_drv.c` | `pvrsgx_parts[]` (CE5300, Clover Trail), PCI id `8086:08cf`, window offset applied to the mapping, BAR length check against offset + `PVRSGX_REGS_MIN` |
| `pvrsgx_drv.h` | `PVRSGX_CLV_SGX_OFFSET` |
| `pvrsgx_core.h` | `struct pvrsgx_part`, `PVRSGX_PART_ISLAND_ALWAYS_ON`, `PVRSGX_CORE_ID_CORE_MASK` |
| `pvrsgx_core.c` | Clover Trail entry with a `TODO` revision placeholder, `CORE_ID` compared on its core number only, `0`/`~0` revision refused |
| `pvrsgx_device.h` | `sdev->part` |
| `pvrsgx_pm.c` | island-always-on policy (above) |
| `tests/pvrsgx_core_test.c` | CONFIG bits are not identity; an empty window is not a part |
| `Kconfig` | help text: Clover Trail, and what its power policy costs |
| `drm/ci/clovertrail.fragment` | test-kernel config: simpledrm + powervr_sgx, no gma500 |

`CORE_REVISION` is the one value the table cannot hold yet. `PVRSGX_CLV_CORE_REV` is
`0`, which no core reports, so a Clover Trail logs

    SGX core id 0x0115xxxx revision 0x000100xx is not one this driver knows; not touching it.

and stays untouched until the placeholder is replaced. That is the intended first result
on hardware, not a failure.

## What to measure, in order

Use `~/elitepad-dev/tools/sgx-ident/sgx-ident` from the initramfs (busybox has `devmem`
too, but the tool takes one page, checks the PCI identity first and prints the
interpretation). Boot from USB, never flash.

1. **Nothing** (`sgx-ident --dry-run`): confirms the paths, `8086:08cf`, and BAR 0.
2. `sgx-ident`: reads the eight registers once. Expected: `CORE_ID` `0x0115xxxx`,
   `CORE_REVISION` `0x0001000d`, `0x0001000e`, or something new.
   - `0x00000000` or `0xffffffff` on every register: the window is not at +0x80000, or
     the island is gated. Stop; do not try other offsets blind. Check the APM status
     (config space of `00:00.0`, ports 0x7a/0x78, then `APM_STS`) before anything else.
   - `CORE_ID` with a different core number (not `0x0115`): wrong window, or a part that
     isn't an SGX545.
3. With the revision in hand, set `PVRSGX_CLV_CORE_REV` and rebuild. The next boot should
   log `SGX core id ... : Intel Clover Trail (SGX545 1.0.xx)` and then
   `microkernel started` — or fail in `pvrsgx_fw_start()`, which is the honest next
   stage ([kernel.md](../kernel.md) 5).
4. Then the CE5300 stage order: `powervr_sgx_test submit-null-job`,
   `powervr_sgx_frame -n 1 -v`, `powervr_sgx_blit -n 10`, `powervr_sgx_hang -n 5`.
5. `simpledrm` + kmsro: `es2gears` at 1280x800, then `glmark2-es2-drm`.

## Traps, in the order they will bite

- **A wrong offset is a hang, not an error.** Both engines answer on the same BAR; a read
  of a display register can take the machine down. The tool reads exactly eight
  addresses, each once.
- **STRICT_DEVMEM** allows this: the window is MMIO, not RAM. `/dev/mem` needs root, and
  the initramfs runs as root.
- **The interrupt is shared with the display** (INTx A, GSI 16). If the display engine
  keeps the line asserted, the SGX handler will be called for events that aren't its
  own: it returns `IRQ_NONE` when `EVENT_STATUS` has no `SW_EVENT`, and the first boots
  can run without claiming the line at all if there is a storm.
- **D3 is fatal here**: the P-Unit can gate the island with the function's D-state, and
  nothing brings it back. Hence the permanent reference, and no `pci_set_power_state()`.
- **Never write `CLKGATECTL` or `SOFT_RESET` from userspace.** The driver's reset
  sequence is the only writer.
- **The microkernel may simply not start.** Same core, same firmware, but this is a
  different stepping of the SoC, and it is the biggest risk in the plan. A failed start
  logs `INIT_STATUS`, `EVENT_STATUS` and the BIF fault registers; that log is the
  evidence to collect before changing anything.

## Not in this branch

- APM/P-Unit island control, and with it runtime PM and system sleep.
- The display: no KMS driver for Cloverview's display engine, and none attempted.
- Any change to the microkernel, the MMU tables, the init script or the uAPI: the core is
  the same SGX545.
- Upstreaming. `AGENTS.md` puts Clover Trail out of scope because of the display; this
  branch is local, and the driver-side change is small enough to propose on its own once
  hardware confirms the window and the revision.
