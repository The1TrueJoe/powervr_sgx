/* SPDX-License-Identifier: MIT */
#ifndef CEDAR_MSVDX_PRIVATE_H
#define CEDAR_MSVDX_PRIVATE_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Private ABI; does not expose commands, pointers or addresses. */
#define CEDAR_CAP_PERSISTENT 1U
#define CEDAR_CAP_SINGLE_PICTURE 2U
#define CEDAR_CAP_PRIVILEGED 4U
#define CEDAR_CAP_H264_RUNTIME 8U
#define CEDAR_CAP_H264_WEIGHT_P 16U
#define CEDAR_CAP_H264_P_REF2 32U
#define CEDAR_CAP_H264_SLICES 64U
#define CEDAR_CAP_H264_STABLE_REFS 128U
#define CEDAR_CAP_H264_DPB4 256U
#define CEDAR_CAP_LAZY_READBACK 2048U
#define CEDAR_CAP_FRAME_DMABUF 4096U
#define CEDAR_CAP_INDEPENDENT_SURFACES 8192U
#define CEDAR_CAP_SURFACE_BINDING 16384U
#define CEDAR_CAP_H264_EXPLICIT_DPB 32768U
#define CEDAR_CAP_H264_FULL_LISTS 65536U
#define CEDAR_CAP_H264_VARIABLE_RESOLUTION 131072U
#define CEDAR_CAP_H264_LARGE_SLICES 262144U
#define CEDAR_CAP_EXPORT_BLANK 524288U
#define CEDAR_H264_MAX_SLICE_BYTES (1024U * 1024U)
#define CEDAR_H264_MAX_WIDTH 1920U
#define CEDAR_H264_MAX_HEIGHT 1088U
#define CEDAR_H264_COLOCATED_BYTES \
	((((1920U / 16U * (1088U / 16U) + 100U) * 128U) + 4095U) & ~4095U)
#define CEDAR_H264_STORAGE_PAGES ((CEDAR_H264_STORAGE_BYTES + 4095U) / 4096U)
#define CEDAR_H264_STORAGE_STRIDE 1920U
#define CEDAR_H264_STORAGE_HEIGHT 1088U
#define CEDAR_H264_STORAGE_UV_OFFSET (CEDAR_H264_STORAGE_STRIDE * CEDAR_H264_STORAGE_HEIGHT)
#define CEDAR_H264_STORAGE_BYTES (CEDAR_H264_STORAGE_UV_OFFSET * 3U / 2U)
#define CEDAR_H264_LIST_ENTRIES 32U
#define CEDAR_H264_FULL_LENGTH_OFFSET 2752U
#define CEDAR_H264_PACKET_LIMIT (CEDAR_H264_FULL_LENGTH_OFFSET + 4U + CEDAR_H264_MAX_SLICE_BYTES)
#define CEDAR_SURFACE_HANDLES 64U
#define CEDAR_H264_MAX_REFS 4U
#define CEDAR_H264_SURFACES 5U
#define CEDAR_H264_MAX_SLICES 64U

static inline int cedar_h264_picture_geometry_valid(unsigned int width, unsigned int height)
{
	return width >= 16 && width <= 1920 && !(width & 15) && height >= 16 && height <= 1088 &&
	       !(height & 15);
}

/* Version10: standard32-byte header with slice count, followed by records
 * { first_mb, packet_bytes, one version7/8/9/11/12 packet }. One complete picture.
 */
#define CEDAR_H264_MULTI_LIMIT (32U + CEDAR_H264_MAX_SLICES * (8U + 548U + 65535U))
#define CEDAR_CONFIG_CABAC 1U
#define CEDAR_CONFIG_TRANSFORM8 2U
#define CEDAR_CONFIG_CONSTRAINED 4U
#define CEDAR_CONFIG_DIRECT8 8U
#define CEDAR_CONFIG_WEIGHT_P 16U
#define CEDAR_CONFIG_WEIGHT_B_SHIFT 8U

struct cedar_msvdx_caps {
	__u32 size, version, codec, width, height, stride, profile_idc, config_flags;
	__s32 chroma_qp_offset, second_chroma_qp_offset;
	__u32 disable_deblock;
	__s32 alpha, beta;
	__u32 features, max_slice_bytes, reserved;
};

#define CEDAR_MSVDX_GET_CAPS _IOR('C', 1, struct cedar_msvdx_caps)

/* Completed, immutable, linear NV12 image. EXPORT input: version1, fd=-1,
 * everything else zero.
 * The id is unique within this loaded driver; dup/SCM_RIGHTS retain identity.
 * Export pins the output slot until the last fd/mapping/display reader closes.
 * This read-only exporter uses fixed-capacity coherent backing storage, with
 * coded dimensions and an explicit fixed UV offset for smaller rectangles.
 */
struct cedar_frame_export {
	__u32 version, flags;
	__s32 fd;
	__u32 reserved;
	__aligned_u64 id;
	__u32 width, height, stride, uv_offset, image_bytes, mapping_bytes;
	__u32 fourcc, slot, verified_bytes, reserved2;
};

#define CEDAR_FRAME_NV12 0x3231564eU
#define CEDAR_MSVDX_EXPORT_FRAME _IOWR('C', 2, struct cedar_frame_export)
/* Allocate a separate immutable, initialized NV12 surface for format probing.
 * Input: version=1, fd=-1, even width/height in 16..1920/1088, rest zero.
 * No decoder state is created or changed. It never aliases a decoded picture.
 */
#define CEDAR_MSVDX_EXPORT_BLANK _IOWR('C', 5, struct cedar_frame_export)
static inline int cedar_frame_blank_valid(const struct cedar_frame_export *r)
{
	return r->version == 1 && !r->flags && r->fd == -1 && !r->reserved && !r->id &&
	       r->width >= 16 && r->width <= CEDAR_H264_MAX_WIDTH && !(r->width & 1) &&
	       r->height >= 16 && r->height <= CEDAR_H264_MAX_HEIGHT && !(r->height & 1) &&
	       !r->stride && !r->uv_offset && !r->image_bytes && !r->mapping_bytes &&
	       !r->fourcc && !r->slot && !r->verified_bytes && !r->reserved2;
}

/* File-local storage handles, additionally guarded by a loaded-driver storage
 * id to reject stale handle reuse. No addresses. CREATE/CANCEL use handle=id=0;
 * other operations require both. slot is UINT32_MAX except for BIND.
 * All layout/reserved fields are input zero. BIND stages the next valid write;
 * malformed/mismatched writes do not consume it. Destroying a pending handle is
 * busy. Handles own NV12 plus private H.264 colocated motion storage.
 */
#define CEDAR_SURFACE_CREATE 1U
#define CEDAR_SURFACE_BIND 2U
#define CEDAR_SURFACE_DESTROY 3U
#define CEDAR_SURFACE_CANCEL 4U
#define CEDAR_SURFACE_INFO 5U
struct cedar_surface_request {
	__u32 version, operation, handle, slot;
	__aligned_u64 id;
	__u32 image_bytes, auxiliary_bytes, reserved[2];
};

#define CEDAR_MSVDX_SURFACE _IOWR('C', 4, struct cedar_surface_request)

/* Version-7 packet: the 64-byte picture descriptor, these 64 bytes encoded
 * little-endian, then a NAL length and NAL. No addresses/register words.
 */
struct cedar_h264_config {
	__u32 size, version, width, height, profile_idc, config_flags;
	__s32 chroma_qp_offset, second_chroma_qp_offset;
	__u32 disable_deblock;
	__s32 alpha, beta;
	__u32 reserved[5];
};

/* Slice deblocking controls may change while reference/exported pictures
 * remain live. Only sequence/PPS identity controls admission across pictures.
 */
static inline int cedar_h264_sequence_equal(const struct cedar_h264_config *a,
					    const struct cedar_h264_config *b)
{
	return a->width == b->width && a->height == b->height && a->profile_idc == b->profile_idc &&
	       a->config_flags == b->config_flags && a->chroma_qp_offset == b->chroma_qp_offset &&
	       a->second_chroma_qp_offset == b->second_chroma_qp_offset;
}

/* Pure bounded validation, shared with the native frontend and offline tests.
 * Progressive 8-bit 4:2:0, bounded short-term references, flat scaling matrices.
 */
static inline int cedar_h264_config_valid(const struct cedar_h264_config *c, unsigned int max_width,
					  unsigned int max_height)
{
	unsigned int i, weight = (c->config_flags >> CEDAR_CONFIG_WEIGHT_B_SHIFT) & 3U;

	if (c->size != sizeof(*c) || c->version != 1 || c->width < 16 || c->width > max_width ||
	    (c->width & 15) || c->height < 16 || c->height > max_height || (c->height & 15) ||
	    (c->profile_idc != 66 && c->profile_idc != 77 && c->profile_idc != 100) ||
	    (c->config_flags & ~0x31fU) || (weight != 0 && weight != 2) ||
	    (c->profile_idc != 100 && (c->config_flags & CEDAR_CONFIG_TRANSFORM8)) ||
	    (c->profile_idc == 66 &&
	     ((c->config_flags & (CEDAR_CONFIG_CABAC | CEDAR_CONFIG_WEIGHT_P)) || weight)) ||
	    c->chroma_qp_offset < -12 || c->chroma_qp_offset > 12 ||
	    c->second_chroma_qp_offset < -12 || c->second_chroma_qp_offset > 12 ||
	    c->disable_deblock > 2 || c->alpha < -6 || c->alpha > 6 || c->beta < -6 || c->beta > 6)
		return 0;
	for (i = 0; i < 5; i++)
		if (c->reserved[i])
			return 0;
	return 1;
}

/* Version8 adds this 64-byte LE block after the version7 configuration.
 * One explicit list0 reference; Y, Cb, Cr are plain VA weights/offsets.
 */
struct cedar_h264_weights {
	__u32 size, version, luma_denom, chroma_denom;
	__s32 luma_weight, luma_offset, cb_weight, cb_offset, cr_weight, cr_offset;
	__u32 reserved[6];
};

/* Version9 (P pictures): config, weights0, this block, weights1, NAL length
 * and data at offsets96,160,224,288,352,356. Slots address owned surfaces.
 */
struct cedar_h264_p_refs {
	__u32 size, version, count, slots[2], pocs[2], reserved[9];
};

/* Version11 retains version9 offsets, replacing p_refs by this block.
 * Physical owned slots are stable DPB indices, including colocated metadata.
 * P: 1..2 list0 entries, no list1. B: 1..2 entries in each list.
 * Only the two retained short-term anchors may appear in either list.
 */
struct cedar_h264_refs {
	__u32 size, version, l0_count, l1_count, l0_slots[2], l1_slots[2];
	__u32 l0_pocs[2], l1_pocs[2], reserved[4];
};

/* Version12: config at96, DPB controls at160, four weight blocks at288,
 * length/data at544/548. Sliding-window short-term I/P and non-reference B.
 * max_refs is fixed until IDR; output may not overwrite any retained entry.
 */
struct cedar_h264_dpb {
	__u32 size, version, max_refs, l0_count, l1_count;
	__u32 l0_slots[4], l1_slots[4], l0_pocs[4], l1_pocs[4], reserved[11];
};

/* Version14: config at96, these controls at160, 32 weight blocks at704,
 * length/data at2752/2756. List entries may repeat a physical reference with
 * different weights. Type6 is a non-IDR I picture and does not reset the DPB.
 * Five backing slots still bound memory; lists do not allocate more surfaces.
 */
struct cedar_h264_dpb32 {
	__u32 size, version, max_refs, l0_count, l1_count, retained_mask, reference, reserved;
	__u32 l0_slots[32], l1_slots[32], l0_pocs[32], l1_pocs[32];
};

static inline int cedar_h264_dpb32_valid(const struct cedar_h264_dpb32 *r, unsigned int type)
{
	unsigned int i, list, count = 0;
	int intra = type == 5 || type == 6;

	if (r->size != sizeof(*r) || r->version != 1 || !r->max_refs || r->max_refs > 4 ||
	    (type != 1 && type != 2 && !intra) || r->l0_count > 32 || r->l1_count > 32 ||
	    (intra ? (r->l0_count || r->l1_count) : !r->l0_count) ||
	    (type == 2 ? !r->l1_count : r->l1_count != 0) || r->reserved ||
	    (r->retained_mask & ~31U) || r->reference > 1 ||
	    (type == 5 && (r->retained_mask || !r->reference)))
		return 0;
	for (i = 0; i < 5; i++)
		count += !!(r->retained_mask & (1U << i));
	if (count > r->max_refs)
		return 0;
	for (list = 0; list < 2; list++) {
		unsigned int n = list ? r->l1_count : r->l0_count;
		const __u32 *slots = list ? r->l1_slots : r->l0_slots;
		const __u32 *pocs = list ? r->l1_pocs : r->l0_pocs;

		for (i = 0; i < 32; i++) {
			if (i < n) {
				if (slots[i] >= 5 || !(r->retained_mask & (1U << slots[i])) ||
				    pocs[i] > 0x7ffffffeU || (pocs[i] & 1))
					return 0;
			} else if (slots[i] != (__u32)-1 || pocs[i]) {
				return 0;
			}
		}
	}
	return 1;
}

static inline int cedar_h264_dpb_valid(const struct cedar_h264_dpb *r, unsigned int type)
{
	unsigned int i, j, list;

	if (r->size != sizeof(*r) || r->version != 1 || !r->max_refs ||
	    r->max_refs > CEDAR_H264_MAX_REFS || (type != 1 && type != 2 && type != 5) ||
	    r->l0_count > r->max_refs || r->l1_count > r->max_refs ||
	    (type == 5 ? (r->l0_count || r->l1_count) : !r->l0_count) ||
	    (type == 2 ? !r->l1_count : r->l1_count != 0))
		return 0;
	for (i = 0; i < 11; i++)
		if (r->reserved[i])
			return 0;
	for (list = 0; list < 2; list++) {
		unsigned int count = list ? r->l1_count : r->l0_count;
		const __u32 *slots = list ? r->l1_slots : r->l0_slots;
		const __u32 *pocs = list ? r->l1_pocs : r->l0_pocs;

		for (i = 0; i < CEDAR_H264_MAX_REFS; i++) {
			if (i < count ? (slots[i] >= CEDAR_H264_SURFACES || pocs[i] > 65534 ||
					 (pocs[i] & 1)) :
					(slots[i] != (__u32)-1 || pocs[i]))
				return 0;
			for (j = 0; j < i && i < count; j++)
				if (slots[i] == slots[j])
					return 0;
		}
	}
	return 1;
}

/* Version13 keeps the v12 layout, with DPB version2. reserved[0] is the
 * retained slot mask before decoding; reserved[1] says this output is a
 * reference. All other reserved words stay zero. Reference B pictures and
 * explicit retirement are supported; long-term/field pictures are not.
 * The kernel validates retained entries against previously completed output.
 * It holds up to five candidates until the next explicit retirement.
 */
static inline int cedar_h264_explicit_dpb_valid(const struct cedar_h264_dpb *r, unsigned int type)
{
	struct cedar_h264_dpb legacy = *r;
	unsigned int i, list, count = 0;

	if (r->version != 2 || (r->reserved[0] & ~31U) || r->reserved[1] > 1 ||
	    (type == 5 && (r->reserved[0] || !r->reserved[1])))
		return 0;
	for (i = 0; i < 5; i++)
		count += !!(r->reserved[0] & (1U << i));
	if (count > r->max_refs)
		return 0;
	legacy.version = 1;
	legacy.reserved[1] = 0;
	legacy.reserved[0] = legacy.reserved[1];
	if (!cedar_h264_dpb_valid(&legacy, type))
		return 0;
	for (list = 0; list < 2; list++) {
		unsigned int n = list ? r->l1_count : r->l0_count;
		const __u32 *slots = list ? r->l1_slots : r->l0_slots;

		for (i = 0; i < n; i++)
			if (!(r->reserved[0] & (1U << slots[i])))
				return 0;
	}
	return 1;
}

static inline int cedar_h264_refs_valid(const struct cedar_h264_refs *r, int b_picture)
{
	unsigned int i, list;

	if (r->size != sizeof(*r) || r->version != 1 || r->l0_count < 1 || r->l0_count > 2 ||
	    (b_picture ? (r->l1_count < 1 || r->l1_count > 2) : r->l1_count != 0))
		return 0;
	for (i = 0; i < 4; i++)
		if (r->reserved[i])
			return 0;
	for (list = 0; list < 2; list++) {
		unsigned int count = list ? r->l1_count : r->l0_count;
		const __u32 *slots = list ? r->l1_slots : r->l0_slots;
		const __u32 *pocs = list ? r->l1_pocs : r->l0_pocs;

		for (i = 0; i < 2; i++)
			if (i < count ? (slots[i] > 2 || pocs[i] > 65534 || (pocs[i] & 1)) :
					(slots[i] != (__u32)-1 || pocs[i]))
				return 0;
		if (count == 2 && slots[0] == slots[1])
			return 0;
	}
	return 1;
}

static inline int cedar_h264_p_refs_valid(const struct cedar_h264_p_refs *r)
{
	unsigned int i;

	if (r->size != sizeof(*r) || r->version != 1 || r->count < 1 || r->count > 2)
		return 0;
	for (i = 0; i < 9; i++)
		if (r->reserved[i])
			return 0;
	for (i = 0; i < r->count; i++)
		if (r->slots[i] > 2 || r->pocs[i] > 65534 || (r->pocs[i] & 1))
			return 0;
	if (r->count == 1)
		return r->slots[1] == (__u32)-1 && r->pocs[1] == 0;
	return r->slots[0] != r->slots[1];
}

static inline int cedar_h264_weight_valid(int weight, int offset, unsigned int denom)
{
	return offset >= -128 && offset <= 127 && weight >= -128 &&
	       (weight <= 127 || (weight == 128 && denom == 7 && offset == 0));
}

static inline int cedar_h264_weights_valid(const struct cedar_h264_weights *w, int active)
{
	unsigned int i;

	if (w->size != sizeof(*w) || w->version != 1 || w->luma_denom > 7 || w->chroma_denom > 7)
		return 0;
	for (i = 0; i < 6; i++)
		if (w->reserved[i])
			return 0;
	if (!active)
		return !(w->luma_denom || w->chroma_denom || w->luma_weight || w->luma_offset ||
			 w->cb_weight || w->cb_offset || w->cr_weight || w->cr_offset);
	return cedar_h264_weight_valid(w->luma_weight, w->luma_offset, w->luma_denom) &&
	       cedar_h264_weight_valid(w->cb_weight, w->cb_offset, w->chroma_denom) &&
	       cedar_h264_weight_valid(w->cr_weight, w->cr_offset, w->chroma_denom);
}

#endif
