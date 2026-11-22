<!-- SPDX-License-Identifier: MIT -->
# Libdrm auxiliary DRM discovery

The two patches target libdrm main at
`33fab8cbca7f7581e955a675597d14412702edc4` and also apply to Debian 13's
libdrm 2.4.124. Patch 1 preserves PCI metadata while separating the display
parent, auxiliary render child and sibling children. Patch 2 adds optional
Linux umockdev tests covering discovery, grouping and disappearance without
DRM hardware or root.

Apply with `git am` from the libdrm checkout, then build normally with Meson.
Install `libumockdev-dev` and `umockdev` on Debian to enable the topology test.
Run `meson test -C BUILD --print-errorlogs`; check that `device-topology` runs.

For the private Debian installation, use `tools/build-libdrm-auxiliary.sh`
from the libdrm/ integration directory. A distribution can apply the same series to its
normal libdrm package. The discovery fix is shared library code; it requires
no mpv or labwc source patch. The SGX serial workaround remains necessary.

These are prepared patches. Follow libdrm's CONTRIBUTING.rst for submission
and shared-code review; no upstream acceptance is implied.
