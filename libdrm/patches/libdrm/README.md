<!-- SPDX-License-Identifier: MIT -->
# Libdrm auxiliary DRM discovery

The two libdrm patches apply to main commit
`33fab8cbca7f7581e955a675597d14412702edc4` and Debian 13 libdrm 2.4.124.
Auxiliary children use a distinct public `DRM_BUS_AUXILIARY` identity containing
the parent's PCI address and child name. Parent PCI IDs remain available in
`deviceinfo.aux`; ordinary PCI identity and comparisons are unchanged.

This follows the explicit auxiliary model proposed by Alistair Popple for Nova:
https://www.mail-archive.com/dri-devel@lists.freedesktop.org/msg625002.html.
Stored child identities remain comparable after sysfs nodes disappear.

Apply the two patches here to libdrm. Mesa’s side is
`mesa/patches/local/0011-libdrm-auxiliary-loader.patch`, applied
to Mesa 26.2.5 with powervr_sgx's overlay patches. It builds with stock libdrm
headers and normalizes either runtime discovery path to an internal
platform-shaped descriptor. Stored child identities produce the same distinct
auxiliary DRI_PRIME tags with either library; parent PCI IDs select the driver.
Existing PCI-only applications may need equivalent handling; a new bus type is not transparent to every client.

The topology test requires umockdev >= 0.19.9 for fstat interception. Debian 13
ships an older version. `tools/build-test-umockdev.sh` builds the pinned upstream
release tarball locally; select its bin/lib/pkgconfig directories before Meson.
Run `meson test -C BUILD --print-errorlogs` and require `device-topology`.
On hardware-free CI, drmdevice skips: report 4 passes and 1 skip, not 5 passes.

The build helper installs only into an explicit private prefix. Selecting it
for a compositor through LD_LIBRARY_PATH also selects it for inherited session
clients. Keep that scope visible and rebuild the matching Mesa first. No patch
has been submitted to libdrm or mpv upstream.
