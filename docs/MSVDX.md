<!-- SPDX-License-Identifier: MIT -->
# Cedarview MSVDX host and video integration

The MSVDX decoder and SGX renderer are separate engines behind the Cedarview
PCI/display owner. The opt-in gma500 host uses a separate register window,
firmware, MMU, RENDEC and owned storage. It does not reset shared SGX/display
state. See the kernel patch's `Documentation/gpu/gma500-msvdx.rst` for interface
and lifetime details and [the companion cedar-video project](https://github.com/thesame/n2800_msvdx/tree/main) for userspace.

## Source provenance

Published source inputs and hardware evidence:

- The DE2 command framing, register definitions, H.264 setup and licensed VLC
  tables come from AOSP `hardware/intel/img/psb_video`, commit
  `fa6f2e9012a890942a47eae9f6f1a0ddcd97e9a4`. The source URLs and file hashes are
  retained in [the source lock](https://github.com/thesame/n2800_msvdx/tree/main/de2-sources.lock.json); original notices remain with tables.
- Register addresses and firmware RAM-transfer sequencing follow the public GPL
  Cedarview DDK `imgv/psb_msvdx.h` and `imgv/psb_msvdxinit.c`, from
  https://github.com/thomas001/cdv/tree/a0f0aa899a67cdd23adfd3a2fc0ab7956eb90cc9/imgv.
- Hardware evidence on Atom N2800 / PCI 8086:0be2 includes exact decoded NV12
  against software, cropped/coded-1088 and multi-slice inputs, sequence wrap,
  imported plane sampling, write rejection, and resource retirement.
- The 18 KiB runtime template contains command layout, patch indices and VLC
  tables. It contains no sample bitstream, reference image or firmware binary.
  Caller input and admitted picture/reference state are patched at submission.

The externally obtained vendor firmware remains non-free. Firmware bytes are unchanged and are not an input to building the host or userspace.

## Runtime and trust boundary

Progressive 8-bit H.264 4:2:0 is admitted through coded 1920x1088, with four
short-term references, reference B pictures, weighted prediction and at most
64 slices. Interlacing, long-term references, custom scaling matrices, 10-bit
video and other codecs fail explicitly. Input pictures are bounded to about
1 MiB including packet overhead. Packets contain codec state and file-local
handles, never caller-selected register addresses or firmware command words.

The root-only private device requires CAP_SYS_RAWIO. The root broker permits
one configured UID/client and validates bounded packets, storage quotas and
sealed large-packet transfers. Root is trusted; this is not a stable mainline
DRM/media API. A standard media/DRM design is separate future work.

Backing has stride 1920 and UV offset 2088960. Completed exports retain image
storage independently of the engine session, prevent output reuse while read,
and allow conventional writable sharing. Consumers must finish external writes
before decoder use, including reference reads. CPU access requires DMA-BUF sync.
Only image pages are exposed; motion/command allocations stay private. Initialized format-probe exports use
separate storage and never mark an undecoded surface ready.

Submission is synchronous with bounded completion polling. Shutdown/suspend
must prove quiescence before DMA mappings are released. Failure quarantines the
engine and retains memory/device/module references until reboot. Only the
unconsumed fresh-intra startup failure has a single bounded isolated-reset
retry. Established chains are not replayed. Live suspend requires a separate
operator request and a retained-reference continuation check.

## Presentation

Direct VA import exposes Y as R8 and UV as GR88 to EGL. Mesa samples the original
NV12 allocation; there is no decoder CPU download or texture upload. mpv's GL2/
GLES2 upload-format metadata must not make it read GR chroma from alpha instead
of green. The independent mpv patch corrects only imported-texture metadata;
CPU luminance/alpha uploads keep their existing behavior.

`SGX_DEBUG=serial` remains necessary in compositor and player. Correct NV12,
correct GPU sampling and identical RGB output are distinct checks. Presentation
can still drop frames; report performance separately from functional passes.
