#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
#
# The driver targets drm-misc-next. For an older kernel, patch a copy
# of its sources with what that kernel still needs (never upstream):
#
#   compat.sh <kernel source or headers> <driver dir>
set -eu
K=$1
D=$2
HERE=$(cd "$(dirname "$0")" && pwd)

if grep -q DRIVER_GEM_GPUVA "$K/include/drm/drm_drv.h" &&
   ! grep -q DRIVER_GEM_GPUVA "$D/pvrsgx_drv.c"; then
	echo ">> compat: DRIVER_GEM_GPUVA (kernel before 7.3)"
	patch -d "$D" -p5 --forward --batch < "$HERE/compat-gem-gpuva.patch"
fi

if ! sed -n '/^void dma_fence_init(/,/;/p' "$K/include/linux/dma-fence.h" | grep -q flags &&
   grep -q 'fence_seqno, 0);' "$D/pvrsgx_job.c"; then
	echo ">> compat: dma_fence_init() without flags (kernel before drm-misc-next 2026-11-19)"
	patch -d "$D" -p5 --forward --batch < "$HERE/compat-fence-flags.patch"
fi
