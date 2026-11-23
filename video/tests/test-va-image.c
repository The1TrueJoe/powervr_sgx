/* SPDX-License-Identifier: MIT */
/* Ownership regression: destroying a surface must not invalidate live images.
 * This is a CPU allocation test; it makes no hardware acceleration claim. */
#include <assert.h>
#include "cedar-broker.h"
#include "../src/va-driver.c"

static void test_runtime_controls(void)
{
    struct cedar_h264_config good = { .size = 64, .version = 1, .width = 1280, .height = 720,
        .profile_idc = 100, .config_flags = CEDAR_CONFIG_CABAC | CEDAR_CONFIG_TRANSFORM8 | (2U << 8),
        .chroma_qp_offset = -12, .second_chroma_qp_offset = 12, .alpha = -6, .beta = 6 };
    _Static_assert(sizeof(good) == 64, "runtime wire controls");
    assert(cedar_h264_config_valid(&good, 1280, 720));
    struct cedar_h264_config slice_change=good;
    slice_change.disable_deblock=1;slice_change.alpha=0;slice_change.beta=0;
    assert(cedar_h264_sequence_equal(&good,&slice_change));
    slice_change.chroma_qp_offset++;
    assert(!cedar_h264_sequence_equal(&good,&slice_change));
#define REJECT(member, value) do { struct cedar_h264_config bad = good; bad.member = (value); assert(!cedar_h264_config_valid(&bad, 1280, 720)); } while (0)
    REJECT(size, 60); REJECT(version, 2); REJECT(width, UINT32_MAX); REJECT(width, 1296);
    REJECT(width, 1279); REJECT(height, 0); REJECT(height, 736); REJECT(profile_idc, 110);
    REJECT(config_flags, 0x80000000U); REJECT(config_flags, 1U << 8);
    REJECT(chroma_qp_offset, -13); REJECT(second_chroma_qp_offset, 13);
    REJECT(disable_deblock, 3); REJECT(alpha, -7); REJECT(beta, 7); REJECT(reserved[4], 1);
    REJECT(profile_idc, 66); REJECT(profile_idc, 77); /* profile tools must match */
#undef REJECT
    good.width = 320; good.height = 240; good.profile_idc = 66;
    good.config_flags = CEDAR_CONFIG_CONSTRAINED; good.disable_deblock = 2;
    assert(cedar_h264_config_valid(&good, 1280, 720));
    good.profile_idc = 77; good.config_flags = CEDAR_CONFIG_CABAC | (2U << 8);
    assert(cedar_h264_config_valid(&good, 1280, 720));
    good.config_flags |= CEDAR_CONFIG_WEIGHT_P;
    assert(cedar_h264_config_valid(&good, 1280, 720));
    good.profile_idc = 66;
    assert(!cedar_h264_config_valid(&good, 1280, 720));
    puts("Runtime H264 control bounds/profile checks passed (no hardware test).");
}

static void test_prediction_weights(void)
{
    struct cedar_h264_weights good = { .size = 64, .version = 1 };
    _Static_assert(sizeof(good) == 64, "prediction weight wire controls");
    assert(cedar_h264_weights_valid(&good, 0));
    good.luma_denom = good.chroma_denom = 7;
    good.luma_weight = good.cb_weight = good.cr_weight = 128;
    assert(cedar_h264_weights_valid(&good, 1));
    assert(!cedar_h264_weights_valid(&good, 0));
#define REJECT(member, value) do { struct cedar_h264_weights bad = good; bad.member = (value); assert(!cedar_h264_weights_valid(&bad, 1)); } while (0)
    REJECT(size, 60); REJECT(version, 2); REJECT(luma_denom, 8); REJECT(chroma_denom, 8);
    REJECT(luma_denom, 6); REJECT(luma_weight, 129); REJECT(cb_weight, -129);
    REJECT(luma_offset, 1); REJECT(cb_offset, 128); REJECT(cr_offset, -129); REJECT(reserved[5], 1);
#undef REJECT
    good.luma_weight = -128; good.luma_offset = -128;
    good.cb_weight = 127; good.cb_offset = 127;
    assert(cedar_h264_weights_valid(&good, 1));
    puts("Prediction weight ranges/default128/inactive controls passed (offline).");
}

static void test_p_references(void)
{
    struct cedar_h264_p_refs good = { .size = 64, .version = 1, .count = 1, .slots = { 0, UINT32_MAX } };
    _Static_assert(sizeof(good) == 64, "P references wire controls");
    assert(cedar_h264_p_refs_valid(&good));
    good.count = 2; good.slots[1] = 2; good.pocs[0] = 4; good.pocs[1] = 2;
    assert(cedar_h264_p_refs_valid(&good));
#define REJECT(member, value) do { struct cedar_h264_p_refs bad = good; bad.member = (value); assert(!cedar_h264_p_refs_valid(&bad)); } while (0)
    REJECT(size, 60); REJECT(version, 2); REJECT(count, 0); REJECT(count, 3);
    REJECT(slots[0], 3); REJECT(slots[1], 0); REJECT(pocs[0], 65536); REJECT(pocs[1], 1); REJECT(reserved[8], 1);
    REJECT(count, 1); /* unused second entry must be canonical */
#undef REJECT
    Driver d = {0}; VASliceParameterBufferH264 s = { .luma_log2_weight_denom = 3, .chroma_log2_weight_denom = 4,
        .luma_weight_l0_flag = 1, .chroma_weight_l0_flag = 1 };
    s.luma_weight_l0[1] = -7; s.luma_offset_l0[1] = 11;
    s.chroma_weight_l0[1][0] = 19; s.chroma_offset_l0[1][0] = -13;
    s.chroma_weight_l0[1][1] = -16; s.chroma_offset_l0[1][1] = 127;
    struct cedar_h264_weights w = { .size = 64, .version = 1 }; bool changed = false;
    assert(prediction_weights(&d, &s, 1, &w, &changed) == VA_STATUS_SUCCESS);
    assert(changed && w.luma_weight == -7 && w.cb_offset == -13 && w.cr_weight == -16 && w.cr_offset == 127);
    puts("P-reference bounds and per-reference weight selection passed (offline).");
}

static void test_render_abort(struct VADriverContext *ctx, VASurfaceID sid)
{
    Driver *d = ctx->pDriverData;
    VAPictureParameterBufferH264 picture = { .CurrPic.picture_id = sid,
        .picture_width_in_mbs_minus1 = 3, .picture_height_in_mbs_minus1 = 1 };
    picture.seq_fields.bits.chroma_format_idc = 1;
    picture.seq_fields.bits.frame_mbs_only_flag = 1;
    VASliceParameterBufferH264 slice = { .slice_type = 2, .slice_data_size = 3, .slice_data_bit_offset = 8 };
    unsigned char nal[] = { 0x65, 0x80, 0 };
    VABufferID ids[3], extra;
    d->context = true; d->width = 64; d->height = 32;
    assert(cdv_CreateBuffer(ctx, CONTEXT_ID, VAPictureParameterBufferType, sizeof(picture), 1, &picture, &ids[0]) == VA_STATUS_SUCCESS);
    assert(cdv_CreateBuffer(ctx, CONTEXT_ID, VASliceParameterBufferType, sizeof(slice), 1, &slice, &ids[1]) == VA_STATUS_SUCCESS);
    assert(cdv_CreateBuffer(ctx, CONTEXT_ID, VASliceDataBufferType, sizeof(nal), 1, nal, &ids[2]) == VA_STATUS_SUCCESS);
    assert(cdv_CreateBuffer(ctx, CONTEXT_ID, VASliceParameterBufferType, sizeof(slice), 1, &slice, &extra) == VA_STATUS_SUCCESS);
    assert(cdv_BeginPicture(ctx, CONTEXT_ID, sid) == VA_STATUS_SUCCESS);
    assert(cdv_RenderPicture(ctx, CONTEXT_ID, ids, 3) == VA_STATUS_SUCCESS);
    /* A second slice is unsupported. The later EndPicture must abort rather
     * than processing the valid first slice staged above. No device is open. */
    assert(cdv_RenderPicture(ctx, CONTEXT_ID, &extra, 1) == VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
    assert(cdv_EndPicture(ctx, CONTEXT_ID) == VA_STATUS_ERROR_DECODING_ERROR);
    assert(d->fd == -1 && !d->begun && !d->bitstream);
    for (unsigned i = 0; i < 3; i++) assert(cdv_DestroyBuffer(ctx, ids[i]) == VA_STATUS_SUCCESS);
    assert(cdv_DestroyBuffer(ctx, extra) == VA_STATUS_SUCCESS);
    assert(cdv_DestroyContext(ctx, CONTEXT_ID) == VA_STATUS_SUCCESS);
    puts("Render failure aborts staged picture before device access (offline).");
}

static unsigned packet_word(const unsigned char *p,unsigned off) {
    return p[off] | (unsigned)p[off+1]<<8 | (unsigned)p[off+2]<<16 | (unsigned)p[off+3]<<24;
}

static void test_stable_references(void)
{
    Driver d = { .fd = -1, .width = 64, .height = 32, .profile = VAProfileH264Main,
        .have_picture = true, .have_slice = true, .target = 12, .last = 11, .previous = 10,
        .slots = {10, 11, BAD}, .poc_origin = 100 };
    d.caps.width = 64; d.caps.height = 32; d.caps.max_slice_bytes = 65535;
    d.caps.features = CEDAR_CAP_H264_RUNTIME | CEDAR_CAP_H264_P_REF2 | CEDAR_CAP_H264_STABLE_REFS;
    for (unsigned i = 0; i < 3; i++) d.surfaces[i] = (Surface){ .id = 10+i, .width = 64, .height = 32, .ready = true, .poc = i == 1 ? 8 : 0 };
    unsigned char nal[] = {1, 0x80, 0}; d.bitstream = nal; d.bitstream_bytes = sizeof(nal);
    d.picture = (VAPictureParameterBufferH264){ .CurrPic = { .picture_id = 12, .TopFieldOrderCnt = 104, .BottomFieldOrderCnt = 104 },
        .picture_width_in_mbs_minus1 = 3, .picture_height_in_mbs_minus1 = 1, .num_ref_frames = 2 };
    d.picture.seq_fields.bits.frame_mbs_only_flag = 1; d.picture.seq_fields.bits.chroma_format_idc = 1;
    VASliceParameterBufferH264 s = { .slice_type = 1, .slice_data_size = sizeof(nal), .slice_data_bit_offset = 8,
        .num_ref_idx_l0_active_minus1 = 1, .num_ref_idx_l1_active_minus1 = 1 };
    for (unsigned i = 0; i < 2; i++) {
        s.RefPicList0[i] = (VAPictureH264){ .picture_id = 10+i, .TopFieldOrderCnt = 100+8*(int)i, .BottomFieldOrderCnt = 100+8*(int)i };
        s.RefPicList1[1-i] = s.RefPicList0[i];
    }
    PreparedSlice prepared;
    assert(prepare_slice(&d, &s, false, &prepared) == VA_STATUS_SUCCESS);
    assert(packet_word(prepared.packet, 4) == 11 && prepared.out == 2);
    assert(packet_word(prepared.packet, 232) == 2 && packet_word(prepared.packet, 236) == 2);
    assert(packet_word(prepared.packet, 240) == 0 && packet_word(prepared.packet, 244) == 1);
    assert(packet_word(prepared.packet, 248) == 1 && packet_word(prepared.packet, 252) == 0);
    struct cedar_h264_refs r; memcpy(&r, prepared.packet+224, sizeof(r)); free(prepared.packet);
    assert(cedar_h264_refs_valid(&r, 1));
#define REJECT(member, value) do { struct cedar_h264_refs bad = r; bad.member = (value); assert(!cedar_h264_refs_valid(&bad, 1)); } while (0)
    REJECT(size, 60); REJECT(version, 2); REJECT(l0_count, 0); REJECT(l1_count, 3);
    REJECT(l0_slots[0], 3); REJECT(l1_slots[1], 1); REJECT(l0_pocs[0], 1); REJECT(l1_pocs[0], 65536); REJECT(reserved[3], 1);
#undef REJECT
    s.RefPicList1[0].TopFieldOrderCnt++;
    assert(prepare_slice(&d, &s, false, &prepared) == VA_STATUS_ERROR_INVALID_SURFACE);
    s.RefPicList1[0].TopFieldOrderCnt--;
    d.caps.features &= ~CEDAR_CAP_H264_STABLE_REFS;
    assert(prepare_slice(&d, &s, false, &prepared) == VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
    puts("Stable DPB/list packing, B-list bounds and VA reference identity passed (offline).");
}

static void test_dpb4(void)
{
    Driver d = { .fd = -1, .width = 64, .height = 32, .profile = VAProfileH264Main,
        .have_picture = true, .have_slice = true, .target = 14, .last = 13, .previous = 12,
        .slots = {10, 11, 12, 13, BAD}, .history = {10, 11, 12, 13}, .ref_count = 4, .ref_limit = 4, .poc_origin = 100 };
    d.caps.width = 64; d.caps.height = 32; d.caps.max_slice_bytes = 65535;
    d.caps.features = CEDAR_CAP_H264_RUNTIME | CEDAR_CAP_H264_P_REF2 | CEDAR_CAP_H264_DPB4;
    for (unsigned i = 0; i < 6; i++) d.surfaces[i] = (Surface){ .id = 10+i, .width = 64, .height = 32, .ready = true, .poc = 2*(int)i };
    unsigned char nal[] = {0x41, 0x80, 0}; d.bitstream = nal; d.bitstream_bytes = sizeof(nal);
    d.picture = (VAPictureParameterBufferH264){ .CurrPic = { .picture_id = 14, .TopFieldOrderCnt = 108, .BottomFieldOrderCnt = 108 },
        .picture_width_in_mbs_minus1 = 3, .picture_height_in_mbs_minus1 = 1, .num_ref_frames = 4 };
    d.picture.seq_fields.bits.frame_mbs_only_flag = 1; d.picture.seq_fields.bits.chroma_format_idc = 1;
    VASliceParameterBufferH264 s = { .slice_type = 0, .slice_data_size = sizeof(nal), .slice_data_bit_offset = 8,
        .num_ref_idx_l0_active_minus1 = 3 };
    for (unsigned i = 0; i < 4; i++) s.RefPicList0[i] = (VAPictureH264){ .picture_id = 13-i,
        .TopFieldOrderCnt = 106-2*(int)i, .BottomFieldOrderCnt = 106-2*(int)i };
    PreparedSlice prepared;
    assert(prepare_slice(&d, &s, false, &prepared) == VA_STATUS_SUCCESS);
    assert(prepared.dpb_packet && prepared.out == 4 && packet_word(prepared.packet, 4) == 12);
    assert(packet_word(prepared.packet, 168) == 4 && packet_word(prepared.packet, 172) == 4);
    struct cedar_h264_dpb r; memcpy(&r, prepared.packet+160, sizeof(r)); free(prepared.packet);
    assert(sizeof(r) == 128 && cedar_h264_dpb_valid(&r, 1));
#define REJECT(member, value) do { struct cedar_h264_dpb bad = r; bad.member = (value); assert(!cedar_h264_dpb_valid(&bad, 1)); } while (0)
    REJECT(size, 124); REJECT(version, 2); REJECT(max_refs, 0); REJECT(max_refs, 5);
    REJECT(l0_count, 5); REJECT(l1_count, 1); REJECT(l0_slots[3], 5); REJECT(l0_slots[3], 3);
    REJECT(l0_pocs[3], 1); REJECT(l0_pocs[3], 65536); REJECT(reserved[10], 1);
#undef REJECT
    commit_references(&d, &prepared);
    assert(d.ref_count == 4 && d.history[0] == 11 && d.history[3] == 14 && d.slots[4] == 14);
    /* The oldest cached surface still has CPU pixels and a stale slot, but
     * was evicted and must not be resurrected as a decoder reference. */
    d.target = 15; d.picture.CurrPic.picture_id = 15;
    d.picture.CurrPic.TopFieldOrderCnt = d.picture.CurrPic.BottomFieldOrderCnt = 110;
    assert(prepare_slice(&d, &s, false, &prepared) == VA_STATUS_ERROR_INVALID_SURFACE);
    for (unsigned i = 0; i < 4; i++) s.RefPicList0[i] = (VAPictureH264){ .picture_id = 14-i,
        .TopFieldOrderCnt = 108-2*(int)i, .BottomFieldOrderCnt = 108-2*(int)i };
    assert(prepare_slice(&d, &s, false, &prepared) == VA_STATUS_SUCCESS);
    assert(prepared.out == 0); free(prepared.packet);
    s.RefPicList0[3].TopFieldOrderCnt++;
    assert(prepare_slice(&d, &s, false, &prepared) == VA_STATUS_ERROR_INVALID_SURFACE);
    s.RefPicList0[3].TopFieldOrderCnt--;
    d.target = 11; d.picture.CurrPic.picture_id = 11;
    assert(prepare_slice(&d, &s, false, &prepared) == VA_STATUS_ERROR_INVALID_SURFACE);
    assert(d.fd == -1);
    puts("Four-reference packing, output ownership, eviction and stale-reference rejection passed (offline).");
}

static void test_explicit_dpb(void)
{
    Driver d = { .fd=-1, .width=64, .height=32, .profile=VAProfileH264Main,
        .have_picture=true, .have_slice=true, .target=12, .last=11,
        .slots={10,11,BAD,BAD,BAD}, .history={10,11}, .ref_count=2, .ref_limit=3 };
    d.caps.width=64; d.caps.height=32; d.caps.max_slice_bytes=65535;
    d.caps.features=CEDAR_CAP_H264_RUNTIME|CEDAR_CAP_H264_P_REF2|CEDAR_CAP_H264_DPB4|CEDAR_CAP_H264_EXPLICIT_DPB;
    for (unsigned i=0;i<4;i++) d.surfaces[i]=(Surface){.id=10+i,.width=64,.height=32,.ready=true,.poc=i==1?8:0};
    unsigned char nal[]={0x21,0x80,0}; d.bitstream=nal; d.bitstream_bytes=sizeof(nal);
    d.picture=(VAPictureParameterBufferH264){ .CurrPic={.picture_id=12,.TopFieldOrderCnt=4,.BottomFieldOrderCnt=4},
        .picture_width_in_mbs_minus1=3,.picture_height_in_mbs_minus1=1,.num_ref_frames=3 };
    d.picture.seq_fields.bits.frame_mbs_only_flag=1; d.picture.seq_fields.bits.chroma_format_idc=1;
    for(unsigned i=0;i<16;i++) d.picture.ReferenceFrames[i].flags=VA_PICTURE_H264_INVALID;
    VAPictureH264 first={.picture_id=10}, second={.picture_id=11,.TopFieldOrderCnt=8,.BottomFieldOrderCnt=8};
    d.picture.ReferenceFrames[0]=first;d.picture.ReferenceFrames[1]=second;
    VASliceParameterBufferH264 s={.slice_type=1,.slice_data_size=3,.slice_data_bit_offset=8};
    s.RefPicList0[0]=first;s.RefPicList1[0]=second;
    PreparedSlice p;
    assert(prepare_slice(&d,&s,false,&p)==VA_STATUS_SUCCESS);
    assert(packet_word(p.packet,4)==13 && p.out==2 && p.reference && p.retained_count==2);
    assert(packet_word(p.packet,244)==3 && packet_word(p.packet,248)==1);
    commit_references(&d,&p);free(p.packet);
    d.surfaces[2].poc=4;
    assert(d.ref_count==3 && d.history[2]==12);
    /* Retire the old anchors and predict from the completed reference B.
     * The output may now reuse a retired anchor's hardware slot. */
    d.target=13;d.picture.CurrPic=(VAPictureH264){.picture_id=13,.TopFieldOrderCnt=10,.BottomFieldOrderCnt=10};
    d.picture.ReferenceFrames[0]=(VAPictureH264){.picture_id=12,.TopFieldOrderCnt=4,.BottomFieldOrderCnt=4};
    d.picture.ReferenceFrames[1].flags=VA_PICTURE_H264_INVALID;
    nal[0]=0x41;s.slice_type=0;s.RefPicList0[0]=d.picture.ReferenceFrames[0];
    assert(prepare_slice(&d,&s,false,&p)==VA_STATUS_SUCCESS);
    assert(p.out==0 && p.retained_count==1);
    commit_references(&d,&p);free(p.packet);
    assert(d.ref_count==2 && d.history[0]==12 && d.history[1]==13);
    d.picture.ReferenceFrames[0]=first;
    assert(prepare_slice(&d,&s,false,&p)==VA_STATUS_ERROR_INVALID_SURFACE);
    assert(d.fd==-1);
    puts("Reference-B retention, explicit retirement and stale-DPB rejection passed (offline).");
}

/* Oversized DPBs used to overwrite retained_ids before late packet validation. */
static void test_oversized_dpb(void)
{
    Driver d = {.fd=-1,.width=64,.height=32,.profile=VAProfileH264Main,
        .have_picture=true,.have_slice=true,.target=15,.ref_count=5,.ref_limit=5};
    d.caps.width=64;d.caps.height=32;d.caps.max_slice_bytes=65535;
    d.caps.features=CEDAR_CAP_H264_RUNTIME|CEDAR_CAP_H264_DPB4|
        CEDAR_CAP_H264_EXPLICIT_DPB|CEDAR_CAP_H264_FULL_LISTS;
    unsigned char nal[]={0x41,0x80,0};d.bitstream=nal;d.bitstream_bytes=sizeof(nal);
    d.picture=(VAPictureParameterBufferH264){.CurrPic={.picture_id=15,
        .TopFieldOrderCnt=10,.BottomFieldOrderCnt=10},.num_ref_frames=5,
        .picture_width_in_mbs_minus1=3,.picture_height_in_mbs_minus1=1};
    d.picture.seq_fields.bits.frame_mbs_only_flag=1;
    d.picture.seq_fields.bits.chroma_format_idc=1;
    for(unsigned i=0;i<16;i++)d.picture.ReferenceFrames[i].flags=VA_PICTURE_H264_INVALID;
    for(unsigned i=0;i<6;i++)d.surfaces[i]=(Surface){.id=10+i,.width=64,.height=32,.ready=true,.poc=2*(int)i};
    for(unsigned i=0;i<5;i++) {
        d.history[i]=d.slots[i]=10+i;
        d.picture.ReferenceFrames[i]=(VAPictureH264){.picture_id=10+i,
            .TopFieldOrderCnt=2*(int)i,.BottomFieldOrderCnt=2*(int)i};
    }
    VASliceParameterBufferH264 slice={.slice_type=0,.slice_data_size=sizeof(nal),.slice_data_bit_offset=8};
    slice.RefPicList0[0]=d.picture.ReferenceFrames[0];
    PreparedSlice prepared;
    assert(prepare_slice(&d,&slice,false,&prepared)==VA_STATUS_ERROR_INVALID_PARAMETER);
    assert(d.fd==-1);
    puts("Five-reference DPB rejected before retaining into the four-entry array (offline).");
}

static void test_full_lists(void)
{
    Driver d = {.fd=-1,.width=64,.height=32,.profile=VAProfileH264Main,.have_picture=true,.have_slice=true,
        .target=11,.last=10,.slots={10,BAD,BAD,BAD,BAD},.history={10},.ref_count=1,.ref_limit=3};
    d.caps.width=64;d.caps.height=32;d.caps.max_slice_bytes=65535;
    d.caps.features=CEDAR_CAP_H264_RUNTIME|CEDAR_CAP_H264_WEIGHT_P|CEDAR_CAP_H264_P_REF2|
        CEDAR_CAP_H264_DPB4|CEDAR_CAP_H264_EXPLICIT_DPB|CEDAR_CAP_H264_FULL_LISTS;
    for(unsigned i=0;i<3;i++)d.surfaces[i]=(Surface){.id=10+i,.width=64,.height=32,.ready=true,.poc=2*(int)i};
    unsigned char nal[]={0x41,0x80,0};d.bitstream=nal;d.bitstream_bytes=3;
    d.surfaces[0].poc=65536;d.surfaces[1].poc=65538;
    d.picture=(VAPictureParameterBufferH264){.CurrPic={.picture_id=11,.TopFieldOrderCnt=65538,.BottomFieldOrderCnt=65538},
        .picture_width_in_mbs_minus1=3,.picture_height_in_mbs_minus1=1,.num_ref_frames=3};
    d.picture.seq_fields.bits.frame_mbs_only_flag=1;d.picture.seq_fields.bits.chroma_format_idc=1;
    d.picture.pic_fields.bits.weighted_pred_flag=1;
    for(unsigned i=0;i<16;i++)d.picture.ReferenceFrames[i].flags=VA_PICTURE_H264_INVALID;
    d.picture.ReferenceFrames[0]=(VAPictureH264){.picture_id=10,.TopFieldOrderCnt=65536,.BottomFieldOrderCnt=65536};
    VASliceParameterBufferH264 s={.slice_type=0,.slice_data_size=3,.slice_data_bit_offset=8,
        .num_ref_idx_l0_active_minus1=31,.luma_log2_weight_denom=3,.chroma_log2_weight_denom=3,.luma_weight_l0_flag=1};
    for(unsigned i=0;i<32;i++) {s.RefPicList0[i]=d.picture.ReferenceFrames[0];s.luma_weight_l0[i]=8+(int)i;}
    PreparedSlice p;
    assert(prepare_slice(&d,&s,false,&p)==VA_STATUS_SUCCESS);
    assert(packet_word(p.packet,4)==14 && p.out==1 && p.l0_count==32 && p.nonidentity);
    struct cedar_h264_dpb32 full;memcpy(&full,p.packet+160,sizeof(full));
    assert(sizeof(full)==544 && cedar_h264_dpb32_valid(&full,1) && full.l0_count==32 && full.retained_mask==1);
    struct cedar_h264_weights w;memcpy(&w,p.packet+704+31*64,sizeof(w));
    assert(w.luma_weight==39 && packet_word(p.packet,2752)==3 && !memcmp(p.packet+2756,nal,3));
    commit_references(&d,&p);free(p.packet);
    d.target=12;d.picture.CurrPic=(VAPictureH264){.picture_id=12,.TopFieldOrderCnt=65540,.BottomFieldOrderCnt=65540};
    d.picture.ReferenceFrames[1]=(VAPictureH264){.picture_id=11,.TopFieldOrderCnt=65538,.BottomFieldOrderCnt=65538};
    s.slice_type=2;
    assert(prepare_slice(&d,&s,false,&p)==VA_STATUS_SUCCESS);
    assert(!p.idr && p.origin==0 && p.poc==65540 && p.out==2 && packet_word(p.packet,44)==6);
    commit_references(&d,&p);free(p.packet);
    assert(d.ref_count==3 && d.history[0]==10 && d.slots[0]==10 && d.poc_origin==0);
    d.caps.features &= ~CEDAR_CAP_H264_FULL_LISTS;
    assert(prepare_slice(&d,&s,false,&p)==VA_STATUS_ERROR_INVALID_PARAMETER);
    d.caps.features |= CEDAR_CAP_H264_FULL_LISTS;
    reset_references(&d);
    for(unsigned i=0;i<16;i++)d.picture.ReferenceFrames[i].flags=VA_PICTURE_H264_INVALID;
    d.picture.ReferenceFrames[0]=(VAPictureH264){.picture_id=999};
    assert(prepare_slice(&d,&s,false,&p)==VA_STATUS_SUCCESS);
    assert(!p.idr && p.poc==0 && p.origin==65540);
    commit_references(&d,&p);free(p.packet);
    assert(d.poc_origin==65540 && d.ref_count==1);
    d.target=11;d.picture.CurrPic=(VAPictureH264){.picture_id=11,.TopFieldOrderCnt=65542,.BottomFieldOrderCnt=65542};
    s.slice_type=0;s.num_ref_idx_l0_active_minus1=0;s.RefPicList0[0]=d.picture.ReferenceFrames[0];
    assert(prepare_slice(&d,&s,false,&p)==VA_STATUS_ERROR_INVALID_SURFACE);
    /* Leading B pictures before a non-IDR recovery point never become
     * readable output or usable references, and must not reset the good DPB. */
    d.picture.CurrPic=(VAPictureH264){.picture_id=11,.TopFieldOrderCnt=65538,.BottomFieldOrderCnt=65538};
    nal[0]=1;s.slice_type=1;d.slice_count=d.unbound_slice=1;
    d.slices[0]=(StagedSlice){.param=s,.bytes=3};
    assert(decode_picture(&d)==VA_STATUS_SUCCESS);
    assert(d.surfaces[1].preroll && !d.surfaces[1].ready && d.discarded_preroll==1 && d.fd==-1);
    assert(d.ref_count==1 && d.history[0]==12);
    struct VADriverContext ctx={.pDriverData=&d};assert(!pthread_mutex_init(&d.mutex,NULL));
    assert(cdv_SyncSurface(&ctx,11)==VA_STATUS_ERROR_DECODING_ERROR);
    assert(cdv_DeriveImage(&ctx,11,NULL)==VA_STATUS_ERROR_DECODING_ERROR);
    pthread_mutex_destroy(&d.mutex);
    d.recovery_preroll=false;
    assert(prepare_slice(&d,&s,false,&p)==VA_STATUS_ERROR_INVALID_PARAMETER);
    puts("32 weighted aliases, large POCs and non-IDR intra startup/retention passed (offline).");
}

static void test_variable_geometry(void)
{
    Driver d={.dmabuf=true};d.caps.width=1920;d.caps.height=1088;
    d.caps.features=CEDAR_CAP_H264_RUNTIME;
    VASurfaceID id;
    assert(create_surfaces(&d,VA_RT_FORMAT_YUV420,640,368,&id,1,NULL,0)==VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
    d.caps.features|=CEDAR_CAP_H264_VARIABLE_RESOLUTION;
    assert(create_surfaces(&d,VA_RT_FORMAT_YUV420,640,368,&id,1,NULL,0)==VA_STATUS_SUCCESS);
    assert(create_surfaces(&d,VA_RT_FORMAT_YUV420,1936,1088,&id,1,NULL,0)==VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
    assert(create_surfaces(&d,VA_RT_FORMAT_YUV420,1920,1104,&id,1,NULL,0)==VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
    assert(create_surfaces(&d,VA_RT_FORMAT_YUV420,639,368,&id,1,NULL,0)==VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
    assert(cedar_h264_picture_geometry_valid(864,480));
    assert(!cedar_h264_picture_geometry_valid(854,480));
    assert(cedar_h264_picture_geometry_valid(1920,1088));
    struct VADriverContext ctx={.pDriverData=&d};VASurfaceAttrib attrs[6];unsigned count=6;
    assert(cdv_QuerySurfaceAttributes(&ctx,CONFIG_ID,attrs,&count)==VA_STATUS_SUCCESS);
    assert(attrs[2].value.value.i==16 && attrs[3].value.value.i==16 && attrs[5].value.value.i==1088);
    d.width=640;d.height=368;
    struct cedar_frame_export frame={.width=640,.height=368,.stride=CEDAR_H264_STORAGE_STRIDE,.uv_offset=CEDAR_H264_STORAGE_UV_OFFSET,
        .image_bytes=640*368*3/2,.mapping_bytes=CEDAR_H264_STORAGE_PAGES*4096,.fourcc=CEDAR_FRAME_NV12,.slot=2,.id=1,.fd=0};
    assert(frame_layout_valid(&d,&frame,2));
    frame.uv_offset=1280*368;assert(!frame_layout_valid(&d,&frame,2));frame.uv_offset=CEDAR_H264_STORAGE_UV_OFFSET;
    frame.mapping_bytes=640*368*3/2;assert(!frame_layout_valid(&d,&frame,2));
    unsigned char *src=malloc(CEDAR_H264_STORAGE_BYTES), *dst=calloc(1,640*384*3/2);
    assert(src && dst);memset(src,0x11,CEDAR_H264_STORAGE_UV_OFFSET);
    memset(src+CEDAR_H264_STORAGE_UV_OFFSET,0x81,CEDAR_H264_STORAGE_BYTES-CEDAR_H264_STORAGE_UV_OFFSET);
    copy_nv12(dst,640,384,src,CEDAR_H264_STORAGE_STRIDE,CEDAR_H264_STORAGE_UV_OFFSET,640,368);
    assert(dst[0]==0x11 && dst[640*368-1]==0x11 && dst[640*384]==0x81 && dst[640*384+640*184-1]==0x81);
    free(src);free(dst);
    puts("Variable geometry bounds, fixed backing UV layout and padded CPU download passed (offline).");
}

static void test_slice_staging(struct VADriverContext *ctx, VASurfaceID sid)
{
    Driver *d=ctx->pDriverData;
    d->caps.features=CEDAR_CAP_H264_RUNTIME|CEDAR_CAP_H264_SLICES;
    d->context=true;d->width=64;d->height=32;
    VAPictureParameterBufferH264 picture={ .CurrPic.picture_id=sid,
        .picture_width_in_mbs_minus1=3,.picture_height_in_mbs_minus1=1 };
    picture.seq_fields.bits.chroma_format_idc=1;picture.seq_fields.bits.frame_mbs_only_flag=1;
    VASliceParameterBufferH264 slices[2]={{ .slice_type=2,.slice_data_size=3,.slice_data_bit_offset=8 },
        { .slice_type=2,.first_mb_in_slice=4,.slice_data_offset=3,.slice_data_size=3,.slice_data_bit_offset=8 }};
    unsigned char nals[]={0x65,0x80,0,0x65,0x80,0};VABufferID ids[3];
    assert(cdv_CreateBuffer(ctx,CONTEXT_ID,VAPictureParameterBufferType,sizeof(picture),1,&picture,&ids[0])==VA_STATUS_SUCCESS);
    assert(cdv_CreateBuffer(ctx,CONTEXT_ID,VASliceParameterBufferType,sizeof(slices[0]),2,slices,&ids[1])==VA_STATUS_SUCCESS);
    assert(cdv_CreateBuffer(ctx,CONTEXT_ID,VASliceDataBufferType,sizeof(nals),1,nals,&ids[2])==VA_STATUS_SUCCESS);
    assert(cdv_BeginPicture(ctx,CONTEXT_ID,sid)==VA_STATUS_SUCCESS);
    assert(cdv_RenderPicture(ctx,CONTEXT_ID,ids,3)==VA_STATUS_SUCCESS);
    PreparedSlice result;
    assert(prepare_picture(d,&result)==VA_STATUS_SUCCESS);
    assert(packet_word(result.packet,4)==10 && packet_word(result.packet,16)==2);
    unsigned second=32+8+packet_word(result.packet,36);
    assert(packet_word(result.packet,32)==0 && packet_word(result.packet,second)==4);
    assert(!memcmp(result.packet+second+8+packet_word(result.packet,second+4)-3,nals+3,3));
    free(result.packet);
    /* A grouped data buffer may exceed the per-slice limit; each individual
     * payload must still fit, even if the aggregate staging limit permits it. */
    d->caps.max_slice_bytes=2;
    assert(prepare_picture(d,&result)==VA_STATUS_ERROR_INVALID_PARAMETER);
    assert(d->fd==-1);
    d->caps.max_slice_bytes=3;
    assert(prepare_picture(d,&result)==VA_STATUS_SUCCESS);
    free(result.packet);
    d->caps.max_slice_bytes=65535;
    /* The installed full-list path must produce an envelope accepted by the
     * broker, retaining per-slice QP/deblock controls and one output slot. */
    d->caps.features |= CEDAR_CAP_H264_DPB4|CEDAR_CAP_H264_EXPLICIT_DPB|CEDAR_CAP_H264_FULL_LISTS;
    d->caps.width=1280;d->caps.height=720;
    d->picture.pic_fields.bits.reference_pic_flag=1;
    d->slices[1].param.disable_deblocking_filter_idc=2;
    d->slices[1].param.slice_qp_delta=3;
    assert(prepare_picture(d,&result)==VA_STATUS_SUCCESS);
    assert(cedar_broker_packet_valid(result.packet,result.bytes));
    free(result.packet);
    /* Failure while preparing the later slice must free the first packet and
     * return before any decoder FD is opened or command is submitted. */
    d->pool_count=STORAGE_POOL_LIMIT;
    for(unsigned i=0;i<STORAGE_POOL_LIMIT;i++)d->pool[i].handle=i+1;
    d->history[CEDAR_H264_SURFACES-1]=sid;
    d->slices[1].param.slice_data_size=4;
    assert(cdv_EndPicture(ctx,CONTEXT_ID)==VA_STATUS_ERROR_INVALID_PARAMETER);
    assert(d->fd==-1 && !d->bitstream && d->context && !d->pool_count);
    for(unsigned i=0;i<STORAGE_POOL_LIMIT;i++)assert(!d->pool[i].handle);
    for(unsigned i=0;i<CEDAR_H264_SURFACES;i++)assert(d->history[i]==BAD);
    for(unsigned i=0;i<3;i++)assert(cdv_DestroyBuffer(ctx,ids[i])==VA_STATUS_SUCCESS);
    assert(cdv_DestroyContext(ctx,CONTEXT_ID)==VA_STATUS_SUCCESS);
    puts("Multislice grouping/packing and late-error cleanup passed (offline).");
}

int main(void)
{
    test_runtime_controls();
    test_prediction_weights();
    test_p_references();
    test_stable_references();
    test_dpb4();
    test_explicit_dpb();
    test_oversized_dpb();
    test_full_lists();
    test_variable_geometry();
    struct VADriverContext ctx = {0};
    Driver *d = calloc(1, sizeof(*d));
    assert(d);
    d->fd = -1;
    d->caps.width = 64; d->caps.height = 32; d->caps.max_slice_bytes = 65535;
    assert(!pthread_mutex_init(&d->mutex, NULL));
    ctx.pDriverData = d;
    VASurfaceID sid, second;
    VAImage a, b;
    assert(cdv_CreateSurfaces2(&ctx, VA_RT_FORMAT_YUV420, 64, 32, &sid, 1, NULL, 0) == VA_STATUS_SUCCESS);
    Surface *original = surface(d, sid);
    assert(original);
    assert(cdv_DeriveImage(&ctx, sid, &a) == VA_STATUS_SUCCESS);
    assert(cdv_DeriveImage(&ctx, sid, &b) == VA_STATUS_SUCCESS);
    void *first, *other;
    assert(cdv_MapBuffer(&ctx, a.buf, &first) == VA_STATUS_SUCCESS);
    assert(cdv_MapBuffer(&ctx, b.buf, &other) == VA_STATUS_SUCCESS);
    assert(first == other && first == original->data);
    memset(first, 0x5a, a.data_size);
    assert(cdv_DestroySurfaces(&ctx, &sid, 1) == VA_STATUS_SUCCESS);
    assert(!surface(d, sid));
    assert(original->data == first && original->image_refs == 2);
    assert(cdv_CreateSurfaces2(&ctx, VA_RT_FORMAT_YUV420, 64, 32, &second, 1, NULL, 0) == VA_STATUS_SUCCESS);
    assert(surface(d, second) != original);
    assert(cdv_DestroyImage(&ctx, a.image_id) == VA_STATUS_SUCCESS);
    assert(original->data == other && original->image_refs == 1);
    for (unsigned i = 0; i < b.data_size; i++) assert(((unsigned char *)other)[i] == 0x5a);
    assert(cdv_UnmapBuffer(&ctx, b.buf) == VA_STATUS_SUCCESS);
    assert(cdv_DestroyImage(&ctx, b.image_id) == VA_STATUS_SUCCESS);
    assert(!original->data && !original->image_refs);
    /* Termination also handles a surface and a derived image still alive. */
    assert(cdv_DeriveImage(&ctx, second, &a) == VA_STATUS_SUCCESS);
    test_render_abort(&ctx, second);
    test_slice_staging(&ctx, second);
    assert(cdv_Terminate(&ctx) == VA_STATUS_SUCCESS);
    assert(!ctx.pDriverData);
    puts("VA CPU-image ownership checks passed (no hardware test).");
    return 0;
}
