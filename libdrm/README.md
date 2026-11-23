<!-- SPDX-License-Identifier: MIT -->
# Auxiliary DRM device discovery

This optional compatibility integration builds libdrm 2.4.124 with two patches:
PCI metadata discovery for auxiliary DRM devices, and hardware-independent
umockdev regression tests. It does not vendor libdrm sources or replace the
system package. The patches have not been submitted to libdrm upstream.

Cedarview's gma500 display and SGX render driver share a PCI parent, but are
separate DRM devices. Unpatched libdrm cannot describe the auxiliary render
child to wlroots. The discovery patch exposes a distinct `DRM_BUS_AUXILIARY` with stored parent
PCI address and child name; parent hardware IDs are in `deviceinfo.aux`.
Ordinary PCI comparisons remain unchanged and do not access live sysfs.
This follows the explicit auxiliary model proposed by Alistair Popple for Nova:
https://www.mail-archive.com/dri-devel@lists.freedesktop.org/msg625002.html. This fix is useful independently of video decoding.

## Build and use

On a native Debian 13 build machine or container:

```sh
sudo apt install ca-certificates curl xz-utils patch build-essential meson ninja-build pkg-config
sh libdrm/tools/build-libdrm-auxiliary.sh
```

The helper verifies the pinned archive SHA256 and builds under `libdrm/build/`.
Use a matching target ABI; compiling on an arbitrary newer host can create an
unusable library. `--install` explicitly installs into `/opt/cedar-libdrm` only.
For packaging, stage with `DESTDIR=... meson install -C BUILD --no-rebuild`,
where BUILD is the build directory printed by the helper.

Select the private library for both compositor and clients:

```sh
LD_LIBRARY_PATH=/opt/cedar-libdrm/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} command arguments
```

The new bus type requires matching client handling. Rebuild Mesa against these
libdrm headers, with `mesa/patches/local/0011-libdrm-auxiliary-loader.patch`
applied by the normal Mesa build helper. Set PKG_CONFIG_PATH to the installed
private libdrm's lib/pkgconfig directory before configuring a fresh Mesa build.
The loader patch recognizes auxiliary render nodes and constructs distinct
auxiliary DRI_PRIME tags. Applications with their own PCI-only switch need
corresponding support; do not assume an enum addition updates them automatically.

The video subtree's `cedar-with-libdrm` launcher applies this selection. Keep
compositor and clients consistent. `cedar-labwc` passes LD_LIBRARY_PATH to its
whole session: every inherited application receives the private library, not
just the video player. This is opt-in and is not restricted to video. Installation neither changes `ld.so.conf`
nor restarts a running desktop. Remove the environment selection to return to
the system library.

The discovery fix does not solve producer/consumer synchronization. The
qualified SGX Wayland configuration still uses `SGX_DEBUG=serial` in compositor
and GPU clients to avoid tile tearing and frame reordering.

## Tests and maintenance

The optional topology test requires umockdev >= 0.19.9, which intercepts
fstat on mock device descriptors. Debian 13 ships an older release. CI uses
`libdrm/tools/build-test-umockdev.sh` to build the pinned test dependency locally
with valac, libglib2.0-dev, libudev-dev and libpcap-dev. Select its install/bin
in PATH, install/lib in LD_LIBRARY_PATH and install/lib/pkgconfig in
PKG_CONFIG_PATH before configuring libdrm. No system libraries are replaced. Run `meson test -C BUILD --print-errorlogs` and confirm that
`device-topology` runs. CI also stages installation without touching the system.
On a CI runner with no DRM device, expect four passes and one `drmdevice` skip.
Tests cover fd/dev_t discovery, enumeration, PCI revision, ordinary PCI and
virtio devices, sibling auxiliary children, node grouping, disappearing sysfs
nodes and rejection of non-PCI auxiliary parents.

The original series also applies to libdrm main commit
`33fab8cbca7f7581e955a675597d14412702edc4`. Preserve attribution and tests when
refreshing it. A future distribution libdrm containing an accepted equivalent can replace this
private integration; the patch is a shared-library fix and does not require
patching labwc or mpv.
