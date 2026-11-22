/* SPDX-License-Identifier: MIT */
#ifndef CEDAR_MSVDX_DMA_H
#define CEDAR_MSVDX_DMA_H
#include "cedar-msvdx.h"

/* Published pre-DE3 byte-wide bitstream LLDMA format. Each descriptor is
 * 32 bytes; hardware list pointers are in 16-byte units. Keep extra records
 * beyond the fixed VLC/command/preload records in the owned command page.
 */
#define CEDAR_H264_DMA_CHUNK_BYTES 65535U
#define CEDAR_H264_DMA_FIRST_OFFSET 0xc60U
#define CEDAR_H264_DMA_EXTRA_OFFSET 0xe00U
#define CEDAR_H264_DMA_DESCRIPTORS ((CEDAR_H264_MAX_SLICE_BYTES + 65534U) / 65535U)

static inline unsigned int cedar_h264_dma_offset(unsigned int index)
{
	return index ? CEDAR_H264_DMA_EXTRA_OFFSET + (index - 1) * 32 : CEDAR_H264_DMA_FIRST_OFFSET;
}

/* Return the number of source bytes described, or zero for invalid input.
 * Words are host-endian; the kernel converts each word before publication.
 */
static inline unsigned int cedar_h264_dma_descriptor(__u32 words[8], __u32 command_base,
						     __u32 data_base, unsigned int bytes,
						     unsigned int index)
{
	unsigned int count, offset, chunk, final;

	if (!bytes || bytes > CEDAR_H264_MAX_SLICE_BYTES || data_base > 0xffffffffU - bytes ||
	    (command_base & 15) || command_base > 0xffffffffU - 4096)
		return 0;
	count = (bytes + 65534U) / 65535U;
	if (index >= count)
		return 0;
	offset = index * 65535U;
	chunk = bytes - offset;
	if (chunk > 65535U)
		chunk = 65535U;
	final = index + 1 == count;
	words[0] = 0x20000000U;
	words[1] = (final ? 0x80000000U : 0) | 0x00040000U | chunk;
	words[2] = 0x0820U;
	words[3] = 4U << 26;
	words[5] = 0;
	words[4] = words[5];
	words[6] = data_base + offset;
	words[7] = final ? 0 : (command_base + cedar_h264_dma_offset(index + 1)) >> 4;
	return chunk;
}

#endif
