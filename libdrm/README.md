<!-- SPDX-License-Identifier: MIT -->
# Auxiliary DRM device discovery

This optional compatibility integration builds libdrm 2.4.124 with two patches:
PCI metadata discovery for auxiliary DRM devices, and hardware-independent
umockdev regression tests. It does not vendor libdrm sources or replace the
system package. The patches have not been submitted to libdrm upstream.

Cedarview's gma500 display and SGX render driver share a PCI parent, but are
separate DRM devices. Unpatched libdrm cannot describe the auxiliary render
child to wlroots. The discovery patch uses the parent's real PCI metadata while
keeping the parent, each child, and sibling children distinct. There is no new
public bus type or ABI. This fix is useful independently of video decoding.

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

The video subtree's `cedar-with-libdrm` launcher applies this selection. Keep
compositor and clients consistent. Installation neither changes `ld.so.conf`
nor restarts a running desktop. Remove the environment selection to return to
the system library.

The discovery fix does not solve producer/consumer synchronization. The
qualified SGX Wayland configuration still uses `SGX_DEBUG=serial` in compositor
and GPU clients to avoid tile tearing and frame reordering.

## Tests and maintenance

Install `libumockdev-dev` and `umockdev` before configuring to build the optional
Linux topology test. Run `meson test -C BUILD --print-errorlogs` and confirm that
`device-topology` runs. CI also stages installation without touching the system.
Tests cover fd/dev_t discovery, enumeration, PCI revision, ordinary PCI and
virtio devices, sibling auxiliary children, node grouping, disappearing sysfs
nodes and rejection of non-PCI auxiliary parents.

The original series also applies to libdrm main commit
`33fab8cbca7f7581e955a675597d14412702edc4`. Preserve attribution and tests when
refreshing it. A future distribution libdrm containing the fix can replace this
private integration; the patch is a shared-library fix and does not require
patching labwc or mpv.
