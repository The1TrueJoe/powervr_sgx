<!-- SPDX-License-Identifier: MIT -->
# Cedarview H.264 video decoding

This subtree provides the MSVDX VA-API frontend, a root broker, playback
launchers and reproducible tests. It supports progressive 8-bit H.264 through
coded 1920x1088 on Atom N2800. See [MSVDX architecture and provenance](../docs/MSVDX-USERSPACE.md)
for the kernel/private ABI, admission limits, storage lifetime and firmware.

The gma500 host is opt-in (`gma500_gfx.msvdx=1`). Its private device is root-only;
the broker authorizes one configured desktop UID. This is a downstream prototype
interface, not a mainline media/DRM ABI or a general multi-user decoder service.

## Build and stage userspace

On a fast matching Debian 13 build host/container:

```sh
sudo apt install build-essential pkg-config libva-dev libdrm-dev python3
make -C video -j8 check
make -C video DESTDIR="$PWD/build/video-package" install
```

Install the staged `/opt/cedar-msvdx` and `/usr/local/bin` trees on the target.
Cross builds require target-compatible headers, startup objects and libraries;
do not copy an arbitrary host-native shared library to the laptop.
`make install` does not install firmware/modules, activate services or reboot.

Configure `video/packaging/cedar-decoder.service.in` with the desktop user's
numeric UID in place of `@UID@`, install it as `cedar-decoder.service`, then
reload systemd and enable/start the service after the decoder device exists.
Keep the socket directory root-owned. Keep the private decoder node mode 0600.

## Firmware

No vendor binary is included in this subtree or the kernel patch. Obtain the
unchanged vendor image independently under its original redistribution license:

- request path: `/lib/firmware/cedarview-test/msvdx_fw_mfld_DE2.0.bin`
- version 0x496, size 32788 bytes
- SHA256 `6d073592a1f10bc7da269f11fd8f15b6ca3e69c07d9efde4cdaa10f4af6be27b`

The original [companion project](https://github.com/thesame/n2800_msvdx)
documents its origin and license. Neither this subtree nor a source-only CI build
fetches or installs firmware. Preserve a bootable fallback when replacing
kernel modules or boot configuration.

## Native Wayland playback

Libdrm must discover the SGX auxiliary render child correctly; the independent
`libdrm/` integration is a separate contribution, carrying temporary discovery
patches and a build helper for an isolated `/opt/cedar-libdrm` library. It is not
required to compile this VA backend; it is needed by the qualified Wayland stack. Start `cedar-labwc -s alacritty` from a TTY.

Start with CPU-copy playback:

```sh
cedar-mpv-wayland-gpu --fullscreen /path/to/video.mp4
```

Direct import additionally requires Mesa R8/GR88 sampling and the separate SGX
kernel/Mesa read-only import changes. The VAAPI and gma500 changes alone do not
enable zero-copy. mpv also needs the independent GR channel-metadata fix in
`video/patches/mpv/`; see its README and `tools/build-mpv-nv12.sh`. The patched
player installs into `/opt/cedar-mpv`, leaving the system player intact.

Inside the Wayland session:

```sh
cedar-mpv-wayland-zero-copy --fullscreen /path/to/video.mp4
```

`CEDAR_SGX_PREFIX` selects an isolated Mesa prefix; its default is `/opt/sgx`.
For example, use `/opt/cedar-sgx-nv12` to preserve the normal Mesa installation.
The launcher requires hardware decoding and never silently falls back. It starts
muted; add `--mute=no` for sound. `cedar-mpv-wayland-gpu` retains the `vaapi-copy`
path as the default and as a comparison. Both compositor and player need `SGX_DEBUG=serial`
to avoid tile tearing/reordering. This is still a graphics synchronization
workaround; successful decoding does not imply flawless presentation.

## Tests

CPU CI runs `make -C video check` without a GPU, decoder or firmware. Optional
hardware binaries are separate Make targets. Run hardware checks serially on an
authorized machine and keep generated media/captures under ignored build output.

- `tools/verify-decode.py`: bounded full NV12 comparison with software, hardware
  progress and resource retirement, including explicit rejection controls.
- `tools/test-h264-stream.py`: per-frame hashes and sequence rollover; live
  suspend only when explicitly requested by the operator.
- `build/test-va-export`: initialized probe exports, write rejection, retained
  descriptor lifetime and optional cross-device PRIME import.
- `build/test-va-export-frames` plus `build/test-dmabuf-planes`: real decoded
  R8/GR88 planes sampled directly in SGX; compare all coded texels with the
  immutable CPU mapping. This complements the software-reference check.
- `tools/test-player-colors.py`: same-frame direct/copy compositor RGB capture,
  catching the mpv `.ra` versus `.rg` chroma bug. Requires `grim` and an active
  Wayland session; mpv's software screenshot fallback would hide the bug.
- `tools/test-player.py`: explicit `--hwdec vaapi` or `vaapi-copy`, seek/reload,
  sustained progress, presentation drops and retirement. Use `--launcher` for
  the direct player and keep scenes/output geometry equal in comparisons.

The corpus generators require FFmpeg/FFprobe. Cover profiles, crops, coded
1088 lines, B references, 64 slices, 720p regression and paired oversized
rejection. GPU sampling and RGB comparisons must pass before performance
claims. Zero reported drops cannot establish absence of visual corruption.

Original licenses and attribution are retained in `LICENSE`, per-file notices
and `patches/`. No media samples, vendor binaries or machine-specific run logs
are included here.
