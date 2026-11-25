<!-- SPDX-License-Identifier: MIT -->
# NV12 DMA-BUF channel metadata in mpv

mpv 0.40 chooses luminance/alpha upload formats on GL2/GLES2. Its EGL
DMA-BUF importer reused that metadata for GR88 imports, causing the renderer
to read chroma from `.ra` instead of `.rg`. V then came from constant alpha,
producing a strong red cast. This patch gives imported textures their own
format metadata and corrects GR88/GR1616 channel semantics. CPU uploads keep
their luminance/alpha handling. No driver-specific test is involved.

The intended upstream is mpv. This compatibility patch is independent of the
kernel/VA decoder and SGX read-only import changes. It fixes presentation;
it cannot make an unsupported buffer import succeed.

Build on a matching Debian 13 host/container with Meson, Ninja, pkg-config,
FFmpeg development libraries, libass-dev, libplacebo-dev, libva-dev,
libegl-dev, libgles-dev, libdrm-dev, libdisplay-info-dev, libwayland-dev,
wayland-protocols, libxkbcommon-dev and the desired audio backend development
package (for example libasound2-dev):

```sh
tools/build-mpv-nv12.sh
sudo tools/build-mpv-nv12.sh --install
```

The archive is pinned by SHA256; source and output stay under ignored build/.
Installation adds only `/opt/cedar-mpv/bin/mpv`. The system mpv is preserved.
For cross compilation, `CEDAR_MPV_MESON_ARGS` accepts a Meson cross file and
feature settings. Keep target headers/libraries and native build tools distinct.

Hardware validation on Cedarview SGX545: native Wayland GLES2 compositor
captures of a fixed H.264 frame differ substantially with unpatched mpv.
With this patch, direct VA import and VA copy match every RGB component, and
the direct path reports zero decoder CPU downloads. Reproduce with
`tools/test-player-colors.py`; its screen capture avoids mpv's software
screenshot fallback. Run long playback checks separately.
