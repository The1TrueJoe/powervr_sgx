/* SPDX-License-Identifier: MIT
 * Restricted native VA backend for the root-only Cedarview MSVDX prototype.
 * Decoding is performed by the kernel/firmware; completed NV12 can be exported
 * read-only or explicitly downloaded for CPU consumers.
 */
#include <va/va_backend.h>
#include <va/va_version.h>
#include <va/va_drmcommon.h>
#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <sys/mman.h>
#include "cedar-msvdx.h"
#include "decoder-transport.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define NSURF 64
#define NBUF 256
#define NIMAGE 64
#define STORAGE_POOL_LIMIT 16
#define CONFIG_ID 1U
#define CONTEXT_ID 1U
#define BAD VA_INVALID_ID

typedef struct {
    unsigned id, width, height, image_refs;
    unsigned char *data;
    int poc;
    bool ready, error, destroyed, preroll;
    struct cedar_surface_request storage;
    struct cedar_frame_export frame;
} Surface;
typedef struct { unsigned id, element, count; size_t capacity; VABufferType type; unsigned char *data; Surface *owner; } Buffer;
typedef struct { unsigned id; VAImage image; } Image;
typedef struct { VASliceParameterBufferH264 param; size_t base, bytes; } StagedSlice;
typedef struct {
    pthread_mutex_t mutex;
    struct cedar_msvdx_caps caps;
    VAProfile profile;
    unsigned width, height;
    unsigned next_id;
    bool configured, context, begun, have_picture, have_slice, render_failed, disable_submission;
    bool identity_weights;
    bool dmabuf, broker, recovery_preroll;
    unsigned discarded_preroll;
    unsigned frame_exports, storage_replacements, storage_reuses, storage_allocations, cpu_downloads;
    unsigned pool_count;
    struct cedar_surface_request pool[STORAGE_POOL_LIMIT];
    bool packet_dumped;
    unsigned explicit_p_frames, nonidentity_p_frames;
    bool p_refs;
    unsigned p_ref_frames, two_ref_frames, older_first_frames, b_ref_frames, b_two_ref_frames;
    int fd;
    int poc_origin;
    VASurfaceID target, last, previous, slots[CEDAR_H264_SURFACES], history[CEDAR_H264_SURFACES];
    unsigned ref_count, ref_limit, dpb_frames, max_l0, max_l1, b_max_l0, b_max_l1;
    VAPictureParameterBufferH264 picture;
    VASliceParameterBufferH264 slice;
    StagedSlice slices[CEDAR_H264_MAX_SLICES];
    unsigned slice_count, unbound_slice;
    unsigned char *bitstream;
    size_t bitstream_bytes;
    Surface surfaces[NSURF];
    Buffer buffers[NBUF];
    Image images[NIMAGE];
} Driver;

#define LOCK Driver *d = ctx->pDriverData; pthread_mutex_lock(&d->mutex)
#define DONE(value) do { VAStatus cdv_result = (value); pthread_mutex_unlock(&d->mutex); return cdv_result; } while (0)

static size_t bytes_for(unsigned w, unsigned h) { return (size_t)w * h * 3 / 2; }
static bool runtime_config(Driver *d) { return !!(d->caps.features & CEDAR_CAP_H264_RUNTIME); }
static bool variable_resolution(Driver *d) { return !!(d->caps.features & CEDAR_CAP_H264_VARIABLE_RESOLUTION); }
static bool supported_profile(Driver *d, VAProfile profile) {
    return runtime_config(d) ? (profile == VAProfileH264ConstrainedBaseline || profile == VAProfileH264Main || profile == VAProfileH264High) : profile == d->profile;
}
static unsigned identifier(Driver *d) { return d->next_id < BAD - 1 ? ++d->next_id : 0; }
static Surface *surface(Driver *d, unsigned id) {
    for (unsigned i = 0; i < NSURF; i++) if (id && d->surfaces[i].id == id) return &d->surfaces[i];
    return NULL;
}
static Buffer *buffer(Driver *d, unsigned id) {
    for (unsigned i = 0; i < NBUF; i++) if (id && d->buffers[i].id == id) return &d->buffers[i];
    return NULL;
}
static Image *image(Driver *d, unsigned id) {
    for (unsigned i = 0; i < NIMAGE; i++) if (id && d->images[i].id == id) return &d->images[i];
    return NULL;
}
static void drop_frame(Surface *s) {
    if (s->frame.id) close(s->frame.fd);
    memset(&s->frame, 0, sizeof(s->frame));
}
static int storage_operation(Driver *d, Surface *s, unsigned operation, unsigned slot) {
    struct cedar_surface_request r = { .version = 1, .operation = operation, .slot = slot };
    if (s && operation != CEDAR_SURFACE_CREATE) { r.handle = s->storage.handle; r.id = s->storage.id; }
    int result = cedar_decoder_surface(d->fd, d->broker, &r);
    if (!result && s) {
        if (operation == CEDAR_SURFACE_CREATE) { s->storage = r; d->storage_allocations++; }
        if (operation == CEDAR_SURFACE_DESTROY) memset(&s->storage, 0, sizeof(s->storage));
    }
    return result;
}
static int release_storage(Driver *d, Surface *s) {
    if (s->storage.handle && d->fd >= 0) {
        if (storage_operation(d, NULL, CEDAR_SURFACE_CANCEL, UINT32_MAX) ||
            storage_operation(d, s, CEDAR_SURFACE_DESTROY, UINT32_MAX)) return -1;
    }
    memset(&s->storage, 0, sizeof(s->storage));
    drop_frame(s);
    return 0;
}
static void drop_buffer(Buffer *b) {
    if (b->owner) {
        Surface *s = b->owner;
        if (--s->image_refs == 0 && s->destroyed) { free(s->data); memset(s, 0, sizeof(*s)); }
    } else free(b->data);
    memset(b, 0, sizeof(*b));
}
static Buffer *new_buffer(Driver *d, VABufferType type, unsigned size, unsigned count, const void *data) {
    if (!size || !count || (uint64_t)size * count > 4 * 1024 * 1024) return NULL;
    for (unsigned i = 0; i < NBUF; i++) if (!d->buffers[i].id) {
        Buffer *b = &d->buffers[i];
        b->data = calloc(count, size);
        b->id = identifier(d);
        if (!b->data || !b->id) { drop_buffer(b); return NULL; }
        b->type = type; b->element = size; b->count = count; b->capacity = (size_t)size * count;
        if (data) memcpy(b->data, data, b->capacity);
        return b;
    }
    return NULL;
}
static void reset_references(Driver *d) { d->last = d->previous = BAD; d->poc_origin = 0; d->p_refs = false; d->recovery_preroll = false; d->ref_count = d->ref_limit = 0; for (unsigned i = 0; i < CEDAR_H264_SURFACES; i++) d->slots[i] = BAD; for (unsigned i = 0; i < CEDAR_H264_MAX_REFS; i++) d->history[i] = BAD; }
static void close_decoder(Driver *d) {
    if (d->fd >= 0) close(d->fd);
    d->fd = -1; d->context = d->begun = false;
    for (unsigned i = 0; i < NSURF; i++) memset(&d->surfaces[i].storage, 0, sizeof(d->surfaces[i].storage));
    d->pool_count = 0; memset(d->pool, 0, sizeof(d->pool));
    free(d->bitstream); d->bitstream = NULL; d->bitstream_bytes = 0;
    reset_references(d);
}
static VAStatus cdv_Terminate(VADriverContextP ctx) {
    Driver *d = ctx->pDriverData;
    if (!d) return VA_STATUS_SUCCESS;
    if (d->explicit_p_frames) fprintf(stderr, "cedar-va: explicit_P=%u nonidentity_requested=%u identity_override=%u\n",
        d->explicit_p_frames, d->nonidentity_p_frames, d->identity_weights);
    if (d->dpb_frames) fprintf(stderr, "cedar-va: DPB_frames=%u max_l0=%u max_l1=%u\n", d->dpb_frames, d->max_l0, d->max_l1);
    if (d->b_ref_frames) fprintf(stderr, "cedar-va: B_refs=%u two_active=%u max_l0=%u max_l1=%u\n", d->b_ref_frames, d->b_two_ref_frames, d->b_max_l0, d->b_max_l1);
    if (d->discarded_preroll) fprintf(stderr, "cedar-va: discarded_unavailable_preroll=%u (surfaces unreadable)\n", d->discarded_preroll);
    if (d->p_ref_frames) fprintf(stderr, "cedar-va: P_refs=%u two_active=%u older_first=%u\n",
        d->p_ref_frames, d->two_ref_frames, d->older_first_frames);
    close_decoder(d);
    if (d->dmabuf) fprintf(stderr, "cedar-va: DMA_exports=%u storage_replacements=%u storage_reuses=%u storage_allocations=%u explicit_CPU_downloads=%u\n",
        d->frame_exports, d->storage_replacements, d->storage_reuses, d->storage_allocations, d->cpu_downloads);
    for (unsigned i = 0; i < NBUF; i++) drop_buffer(&d->buffers[i]);
    for (unsigned i = 0; i < NSURF; i++) { drop_frame(&d->surfaces[i]); free(d->surfaces[i].data); }
    pthread_mutex_destroy(&d->mutex); free(d); ctx->pDriverData = NULL;
    return VA_STATUS_SUCCESS;
}
static VAStatus cdv_QueryConfigProfiles(VADriverContextP ctx, VAProfile *list, int *count) {
    Driver *d = ctx->pDriverData;
    if (runtime_config(d)) { list[0] = VAProfileH264ConstrainedBaseline; list[1] = VAProfileH264Main; list[2] = VAProfileH264High; *count = 3; }
    else { list[0] = d->profile; *count = 1; }
    return VA_STATUS_SUCCESS;
}
static VAStatus cdv_QueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile, VAEntrypoint *list, int *count) {
    Driver *d = ctx->pDriverData;
    if (!supported_profile(d, profile)) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    list[0] = VAEntrypointVLD; *count = 1; return VA_STATUS_SUCCESS;
}
static VAStatus cdv_GetConfigAttributes(VADriverContextP ctx, VAProfile profile, VAEntrypoint entry, VAConfigAttrib *attrs, int count) {
    Driver *d = ctx->pDriverData;
    if (!supported_profile(d, profile)) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (entry != VAEntrypointVLD) return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    for (int i = 0; i < count; i++) {
        attrs[i].value = VA_ATTRIB_NOT_SUPPORTED;
        switch (attrs[i].type) {
        case VAConfigAttribRTFormat: attrs[i].value = VA_RT_FORMAT_YUV420; break;
        case VAConfigAttribDecSliceMode: attrs[i].value = VA_DEC_SLICE_MODE_NORMAL; break;
        case VAConfigAttribDecProcessing: attrs[i].value = VA_DEC_PROCESSING_NONE; break;
        case VAConfigAttribMaxPictureWidth: attrs[i].value = d->caps.width; break;
        case VAConfigAttribMaxPictureHeight: attrs[i].value = d->caps.height; break;
        default: break;
        }
    }
    return VA_STATUS_SUCCESS;
}
static VAStatus cdv_CreateConfig(VADriverContextP ctx, VAProfile profile, VAEntrypoint entry, VAConfigAttrib *attrs, int count, VAConfigID *id) {
    LOCK;
    if (!supported_profile(d, profile) || entry != VAEntrypointVLD) DONE(VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
    if (d->configured) DONE(VA_STATUS_ERROR_MAX_NUM_EXCEEDED);
    for (int i = 0; i < count; i++) if (attrs[i].type == VAConfigAttribRTFormat && attrs[i].value != VA_RT_FORMAT_YUV420) DONE(VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    d->profile = profile; d->configured = true; *id = CONFIG_ID; DONE(VA_STATUS_SUCCESS);
}
static VAStatus cdv_DestroyConfig(VADriverContextP ctx, VAConfigID id) {
    LOCK; if (id != CONFIG_ID || !d->configured) DONE(VA_STATUS_ERROR_INVALID_CONFIG);
    d->configured = false; DONE(VA_STATUS_SUCCESS);
}
static VAStatus cdv_QueryConfigAttributes(VADriverContextP ctx, VAConfigID id, VAProfile *profile, VAEntrypoint *entry, VAConfigAttrib *attrs, int *count) {
    LOCK; if (id != CONFIG_ID || !d->configured) DONE(VA_STATUS_ERROR_INVALID_CONFIG);
    *profile = d->profile; *entry = VAEntrypointVLD; *count = 1;
    attrs[0] = (VAConfigAttrib){ .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 }; DONE(VA_STATUS_SUCCESS);
}
static VAStatus create_surfaces(Driver *d, unsigned format, unsigned width, unsigned height, VASurfaceID *ids, unsigned count, VASurfaceAttrib *attrs, unsigned nattrs) {
    if (d->dmabuf && !variable_resolution(d) && (width != 1280 || height != 720)) return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    if (d->dmabuf && (width > CEDAR_H264_MAX_WIDTH || height > CEDAR_H264_MAX_HEIGHT)) return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    if (format != VA_RT_FORMAT_YUV420) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (width < (runtime_config(d) ? 16 : d->caps.width) || height < (runtime_config(d) ? 16 : d->caps.height) || width > ((d->caps.width + 63) & ~63U) ||
        height > ((d->caps.height + 63) & ~63U) || (width & 1) || (height & 1) || count > NSURF) return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    for (unsigned i = 0; i < nattrs; i++) {
        if (attrs[i].type == VASurfaceAttribPixelFormat && attrs[i].value.value.i != (int)VA_FOURCC_NV12) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        if (attrs[i].type == VASurfaceAttribMemoryType && attrs[i].value.value.i != VA_SURFACE_ATTRIB_MEM_TYPE_VA) return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        if (attrs[i].type == VASurfaceAttribExternalBufferDescriptor) return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }
    unsigned done = 0;
    for (unsigned i = 0; i < NSURF && done < count; i++) if (!d->surfaces[i].id && !d->surfaces[i].data) {
        Surface *s = &d->surfaces[i]; s->data = d->dmabuf ? NULL : calloc(1, bytes_for(width, height)); s->id = identifier(d);
        if ((!d->dmabuf && !s->data) || !s->id) { free(s->data); memset(s, 0, sizeof(*s)); break; }
        s->width = width; s->height = height; ids[done++] = s->id;
    }
    if (done == count) return VA_STATUS_SUCCESS;
    for (unsigned i = 0; i < done; i++) { Surface *s = surface(d, ids[i]); free(s->data); memset(s, 0, sizeof(*s)); }
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
}
static VAStatus cdv_CreateSurfaces2(VADriverContextP ctx, unsigned format, unsigned width, unsigned height, VASurfaceID *ids, unsigned count, VASurfaceAttrib *attrs, unsigned nattrs) {
    LOCK; VAStatus result = create_surfaces(d, format, width, height, ids, count, attrs, nattrs); DONE(result);
}
static VAStatus cdv_CreateSurfaces(VADriverContextP ctx, int width, int height, int format, int count, VASurfaceID *ids) {
    return cdv_CreateSurfaces2(ctx, (unsigned)format, (unsigned)width, (unsigned)height, ids, (unsigned)count, NULL, 0);
}
static VAStatus cdv_DestroySurfaces(VADriverContextP ctx, VASurfaceID *ids, int count) {
    LOCK;
    for (int i = 0; i < count; i++) if (!surface(d, ids[i])) DONE(VA_STATUS_ERROR_INVALID_SURFACE);
    for (int i = 0; i < count; i++) {
        Surface *s = surface(d, ids[i]);
        if (!s) continue;
        if (d->dmabuf && release_storage(d, s)) DONE(VA_STATUS_ERROR_OPERATION_FAILED);
        if (s->image_refs) { s->id = 0; s->destroyed = true; }
        else { free(s->data); memset(s, 0, sizeof(*s)); }
    }
    DONE(VA_STATUS_SUCCESS);
}
static VAStatus cdv_QuerySurfaceAttributes(VADriverContextP ctx, VAConfigID config, VASurfaceAttrib *attrs, unsigned *count) {
    Driver *d = ctx->pDriverData;
    const unsigned needed = 6;
    if (!attrs) { *count = needed; return VA_STATUS_SUCCESS; }
    if (*count < needed) { *count = needed; return VA_STATUS_ERROR_MAX_NUM_EXCEEDED; }
    const VASurfaceAttribType types[] = { VASurfaceAttribPixelFormat, VASurfaceAttribMemoryType, VASurfaceAttribMinWidth, VASurfaceAttribMinHeight, VASurfaceAttribMaxWidth, VASurfaceAttribMaxHeight };
    const int values[] = { VA_FOURCC_NV12, VA_SURFACE_ATTRIB_MEM_TYPE_VA, runtime_config(d) ? 16 : (int)d->caps.width,
        runtime_config(d) ? 16 : (int)d->caps.height, (int)((d->caps.width + 63) & ~63U), (int)((d->caps.height + 63) & ~63U) };
    for (unsigned i = 0; i < needed; i++) attrs[i] = (VASurfaceAttrib){ .type = types[i], .flags = VA_SURFACE_ATTRIB_GETTABLE | (i < 2 ? VA_SURFACE_ATTRIB_SETTABLE : 0), .value = { .type = VAGenericValueTypeInteger, .value.i = values[i] } };
    if (d->dmabuf) {
        attrs[2].value.value.i = variable_resolution(d) ? 16 : 1280;
        attrs[3].value.value.i = variable_resolution(d) ? 16 : 720;
        attrs[4].value.value.i = CEDAR_H264_MAX_WIDTH; attrs[5].value.value.i = CEDAR_H264_MAX_HEIGHT;
    }
    *count = needed; return VA_STATUS_SUCCESS;
}
static VAStatus cdv_CreateContext(VADriverContextP ctx, VAConfigID config, int width, int height, int flags, VASurfaceID *targets, int count, VAContextID *id) {
    LOCK;
    if (!d->configured || config != CONFIG_ID) DONE(VA_STATUS_ERROR_INVALID_CONFIG);
    if (d->context) DONE(VA_STATUS_ERROR_MAX_NUM_EXCEEDED);
    if (d->dmabuf && !variable_resolution(d) && (width != 1280 || height != 720)) DONE(VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
    if (runtime_config(d)) {
        if (width < 16 || height < 16 || (unsigned)width > d->caps.width || (unsigned)height > d->caps.height) DONE(VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
        d->width = ((unsigned)width + 15) & ~15U; d->height = ((unsigned)height + 15) & ~15U;
    } else {
        if (width != (int)d->caps.width || height != (int)d->caps.height) DONE(VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
        d->width = d->caps.width; d->height = d->caps.height;
    }
    d->fd = cedar_decoder_open(&d->broker);
    if (d->fd < 0) DONE(VA_STATUS_ERROR_OPERATION_FAILED);
    d->context = true; reset_references(d); *id = CONTEXT_ID; DONE(VA_STATUS_SUCCESS);
}
static VAStatus cdv_DestroyContext(VADriverContextP ctx, VAContextID id) {
    LOCK; if (!d->context || id != CONTEXT_ID) DONE(VA_STATUS_ERROR_INVALID_CONTEXT);
    close_decoder(d); DONE(VA_STATUS_SUCCESS);
}
static VAStatus cdv_CreateBuffer(VADriverContextP ctx, VAContextID context, VABufferType type, unsigned size, unsigned count, void *data, VABufferID *id) {
    LOCK; if (!d->context || context != CONTEXT_ID) DONE(VA_STATUS_ERROR_INVALID_CONTEXT);
    Buffer *b = new_buffer(d, type, size, count, data);
    if (!b) DONE(VA_STATUS_ERROR_ALLOCATION_FAILED);
    *id = b->id; DONE(VA_STATUS_SUCCESS);
}
static VAStatus cdv_BufferSetNumElements(VADriverContextP ctx, VABufferID id, unsigned count) {
    LOCK; Buffer *b = buffer(d, id); if (!b) DONE(VA_STATUS_ERROR_INVALID_BUFFER);
    if (!count || (uint64_t)count * b->element > b->capacity) DONE(VA_STATUS_ERROR_INVALID_PARAMETER);
    b->count = count; DONE(VA_STATUS_SUCCESS);
}
static VAStatus cdv_MapBuffer(VADriverContextP ctx, VABufferID id, void **data) {
    LOCK; Buffer *b = buffer(d, id); if (!b) DONE(VA_STATUS_ERROR_INVALID_BUFFER);
    *data = b->data; DONE(VA_STATUS_SUCCESS);
}
static VAStatus cdv_UnmapBuffer(VADriverContextP ctx, VABufferID id) {
    LOCK; DONE(buffer(d, id) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER);
}
static VAStatus cdv_DestroyBuffer(VADriverContextP ctx, VABufferID id) {
    LOCK; Buffer *b = buffer(d, id); if (!b) DONE(VA_STATUS_ERROR_INVALID_BUFFER);
    drop_buffer(b); DONE(VA_STATUS_SUCCESS);
}
static VAStatus cdv_BufferInfo(VADriverContextP ctx, VABufferID id, VABufferType *type, unsigned *size, unsigned *count) {
    LOCK; Buffer *b = buffer(d, id); if (!b) DONE(VA_STATUS_ERROR_INVALID_BUFFER);
    *type = b->type; *size = b->element; *count = b->count; DONE(VA_STATUS_SUCCESS);
}
static const VAImageFormat nv12 = { .fourcc = VA_FOURCC_NV12, .byte_order = VA_LSB_FIRST, .bits_per_pixel = 12 };
static VAStatus cdv_QueryImageFormats(VADriverContextP ctx, VAImageFormat *formats, int *count) {
    *count = 1; formats[0] = nv12; return VA_STATUS_SUCCESS;
}
static VAStatus cdv_CreateImage(VADriverContextP ctx, VAImageFormat *format, int width, int height, VAImage *out) {
    LOCK;
    if (format->fourcc != VA_FOURCC_NV12 || width <= 0 || height <= 0 || (width & 1) || (height & 1) ||
        (unsigned)width > ((d->caps.width + 63) & ~63U) || (unsigned)height > ((d->caps.height + 63) & ~63U)) DONE(VA_STATUS_ERROR_INVALID_IMAGE_FORMAT);
    for (unsigned i = 0; i < NIMAGE; i++) if (!d->images[i].id) {
        Buffer *b = new_buffer(d, VAImageBufferType, (unsigned)bytes_for((unsigned)width, (unsigned)height), 1, NULL);
        if (!b) DONE(VA_STATUS_ERROR_ALLOCATION_FAILED);
        Image *im = &d->images[i]; im->id = identifier(d);
        if (!im->id) { drop_buffer(b); DONE(VA_STATUS_ERROR_ALLOCATION_FAILED); }
        im->image = (VAImage){ .image_id = im->id, .format = nv12, .buf = b->id, .width = (unsigned short)width, .height = (unsigned short)height,
                              .data_size = (unsigned)bytes_for((unsigned)width, (unsigned)height), .num_planes = 2,
                              .pitches = { (unsigned)width, (unsigned)width, 0 }, .offsets = { 0, (unsigned)(width * height), 0 } };
        *out = im->image; DONE(VA_STATUS_SUCCESS);
    }
    DONE(VA_STATUS_ERROR_MAX_NUM_EXCEEDED);
}
static VAStatus cdv_DestroyImage(VADriverContextP ctx, VAImageID id) {
    LOCK; Image *im = image(d, id); if (!im) DONE(VA_STATUS_ERROR_INVALID_IMAGE);
    Buffer *b = buffer(d, im->image.buf); if (b) drop_buffer(b);
    memset(im, 0, sizeof(*im)); DONE(VA_STATUS_SUCCESS);
}
static void copy_nv12(unsigned char *dst, unsigned dw, unsigned dh, const unsigned char *src, unsigned sw, unsigned src_uv_offset, unsigned width, unsigned height) {
    for (unsigned y = 0; y < height; y++) memcpy(dst + (size_t)y * dw, src + (size_t)y * sw, width);
    for (unsigned y = 0; y < height / 2; y++) memcpy(dst + (size_t)dw * dh + (size_t)y * dw, src + src_uv_offset + (size_t)y * sw, width);
}
static VAStatus download_surface(Driver *d, Surface *s, unsigned char *dst,
                                unsigned dw, unsigned dh, unsigned width, unsigned height) {
    if (!d->dmabuf) { copy_nv12(dst, dw, dh, s->data, s->width, s->width*s->height, width, height); return VA_STATUS_SUCCESS; }
    if (!s->ready || !s->frame.id) return VA_STATUS_ERROR_OPERATION_FAILED;
    void *pixels = mmap(NULL, s->frame.mapping_bytes, PROT_READ, MAP_SHARED, s->frame.fd, 0);
    if (pixels == MAP_FAILED) return VA_STATUS_ERROR_OPERATION_FAILED;
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START };
    int result = ioctl(s->frame.fd, DMA_BUF_IOCTL_SYNC, &sync);
    if (!result) {
        copy_nv12(dst, dw, dh, pixels, s->frame.stride, s->frame.uv_offset, width, height);
        sync.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_END;
        result = ioctl(s->frame.fd, DMA_BUF_IOCTL_SYNC, &sync);
    }
    munmap(pixels, s->frame.mapping_bytes);
    if (result) return VA_STATUS_ERROR_OPERATION_FAILED;
    d->cpu_downloads++;
    return VA_STATUS_SUCCESS;
}
static VAStatus cdv_GetImage(VADriverContextP ctx, VASurfaceID id, int x, int y, unsigned width, unsigned height, VAImageID iid) {
    LOCK; Surface *s = surface(d, id); Image *im = image(d, iid);
    if (!s) DONE(VA_STATUS_ERROR_INVALID_SURFACE);
    if (!im) DONE(VA_STATUS_ERROR_INVALID_IMAGE);
    Buffer *b = buffer(d, im->image.buf); if (!b) DONE(VA_STATUS_ERROR_INVALID_BUFFER);
    if (s->error || s->preroll) DONE(VA_STATUS_ERROR_DECODING_ERROR);
    if (x || y || width > s->width || height > s->height || width > im->image.width || height > im->image.height || (width & 1) || (height & 1)) DONE(VA_STATUS_ERROR_INVALID_PARAMETER);
    VAStatus result = download_surface(d, s, b->data, im->image.width, im->image.height, width, height); DONE(result);
}
static VAStatus cdv_SyncSurface(VADriverContextP ctx, VASurfaceID id) {
    LOCK; Surface *s = surface(d, id); if (!s) DONE(VA_STATUS_ERROR_INVALID_SURFACE);
    DONE(s->error || s->preroll ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS);
}
static VAStatus cdv_QuerySurfaceStatus(VADriverContextP ctx, VASurfaceID id, VASurfaceStatus *status) {
    LOCK; if (!surface(d, id)) DONE(VA_STATUS_ERROR_INVALID_SURFACE);
    *status = d->begun && d->target == id ? VASurfaceRendering : VASurfaceReady; DONE(VA_STATUS_SUCCESS);
}
static VAStatus cdv_QuerySubpictureFormats(VADriverContextP ctx, VAImageFormat *formats, unsigned *flags, unsigned *count) { *count = 0; return VA_STATUS_SUCCESS; }
static VAStatus cdv_QueryDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *attrs, int *count) { *count = 0; return VA_STATUS_SUCCESS; }

/* Decode submission and ABI initialization follow below. Unsupported image
 * derivation/export/presentation methods return errors; no software decoder. */
static VAStatus cdv_BeginPicture(VADriverContextP ctx, VAContextID context, VASurfaceID target) {
    LOCK;
    if (!d->context || context != CONTEXT_ID || d->begun) DONE(VA_STATUS_ERROR_INVALID_CONTEXT);
    if (!surface(d, target)) DONE(VA_STATUS_ERROR_INVALID_SURFACE);
    d->target = target; d->begun = true; d->have_picture = d->have_slice = d->render_failed = false;
    d->slice_count = d->unbound_slice = 0;
    free(d->bitstream); d->bitstream = NULL; d->bitstream_bytes = 0;
    DONE(VA_STATUS_SUCCESS);
}
static VAStatus cdv_RenderPicture(VADriverContextP ctx, VAContextID context, VABufferID *ids, int count) {
    LOCK;
    if (!d->context || context != CONTEXT_ID || !d->begun) DONE(VA_STATUS_ERROR_INVALID_CONTEXT);
    if (d->render_failed) DONE(VA_STATUS_ERROR_DECODING_ERROR);
    /* FFmpeg may call EndPicture to clean up after RenderPicture fails.
     * Never submit the partial picture staged before that failure. */
    d->render_failed = true;
    if (count < 1 || !ids) DONE(VA_STATUS_ERROR_INVALID_PARAMETER);
    for (int i = 0; i < count; i++) {
        Buffer *b = buffer(d, ids[i]); if (!b) DONE(VA_STATUS_ERROR_INVALID_BUFFER);
        size_t size = (size_t)b->element * b->count;
        switch (b->type) {
        case VAPictureParameterBufferType:
            if (b->count != 1 || size < sizeof(d->picture)) DONE(VA_STATUS_ERROR_INVALID_PARAMETER);
            memcpy(&d->picture, b->data, sizeof(d->picture)); d->have_picture = true; break;
        case VASliceParameterBufferType:
            if (b->element < sizeof(d->slice) || !b->count || b->count >
                ((d->caps.features & CEDAR_CAP_H264_SLICES) ? CEDAR_H264_MAX_SLICES : 1U) - d->slice_count)
                DONE(VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
            for (unsigned j = 0; j < b->count; j++) {
                StagedSlice *staged = &d->slices[d->slice_count++];
                memset(staged, 0, sizeof(*staged));
                memcpy(&staged->param, b->data + (size_t)j * b->element, sizeof(staged->param));
            }
            d->have_slice = true; break;
        case VAIQMatrixBufferType: {
            if (b->count != 1 || size < sizeof(VAIQMatrixBufferH264)) DONE(VA_STATUS_ERROR_INVALID_PARAMETER);
            const VAIQMatrixBufferH264 *iq = (const void *)b->data;
            for (unsigned j = 0; j < sizeof(iq->ScalingList4x4); j++) if (((const unsigned char *)iq->ScalingList4x4)[j] != 16) DONE(VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
            for (unsigned j = 0; j < sizeof(iq->ScalingList8x8); j++) if (((const unsigned char *)iq->ScalingList8x8)[j] != 16) DONE(VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
            break;
        }
        case VASliceDataBufferType: {
            size_t limit = (size_t)d->caps.max_slice_bytes * ((d->caps.features & CEDAR_CAP_H264_SLICES) ? CEDAR_H264_MAX_SLICES : 1U);
            if (d->unbound_slice == d->slice_count || !size || size > limit - d->bitstream_bytes) DONE(VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
            unsigned char *next = realloc(d->bitstream, d->bitstream_bytes + size);
            if (!next) DONE(VA_STATUS_ERROR_ALLOCATION_FAILED);
            d->bitstream = next;
            memcpy(next + d->bitstream_bytes, b->data, size);
            for (; d->unbound_slice < d->slice_count; d->unbound_slice++) {
                d->slices[d->unbound_slice].base = d->bitstream_bytes;
                d->slices[d->unbound_slice].bytes = size;
            }
            d->bitstream_bytes += size; break;
        }
        default: DONE(VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE);
        }
    }
    d->render_failed = false; DONE(VA_STATUS_SUCCESS);
}
static void word(unsigned char *p, unsigned offset, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) p[offset + i] = (unsigned char)(value >> (8 * i));
}
static int slot_of(Driver *d, VASurfaceID id) {
    for (int i = 0; i < (int)CEDAR_H264_SURFACES; i++) if (id != BAD && d->slots[i] == id) return i;
    return -1;
}
static VAStatus bind_destination(Driver *d, Surface *target, unsigned out) {
    if (storage_operation(d, NULL, CEDAR_SURFACE_CANCEL, UINT32_MAX)) return VA_STATUS_ERROR_OPERATION_FAILED;
    drop_frame(target); target->ready = false;
    /* An IDR resets the reference map but does not unbind kernel allocations. */
    bool replace = target->storage.handle && target->storage.slot != UINT32_MAX && target->storage.slot != out;
    if (target->storage.handle && !replace) {
        if (!storage_operation(d, target, CEDAR_SURFACE_BIND, out)) return VA_STATUS_SUCCESS;
        if (errno != EBUSY) return VA_STATUS_ERROR_OPERATION_FAILED;
        replace = true; /* An external descriptor still owns the old generation. */
    }
    if (replace) {
        if (d->pool_count < STORAGE_POOL_LIMIT) {
            d->pool[d->pool_count++] = target->storage;
            memset(&target->storage, 0, sizeof(target->storage));
        } else if (storage_operation(d, target, CEDAR_SURFACE_DESTROY, UINT32_MAX)) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        d->storage_replacements++;
    }
    for (unsigned i = 0; i < d->pool_count; i++) {
        /* Never alias storage still mapped at another decoder slot. Once
         * that slot is replaced, EndPicture marks the old generation unbound.
         * BIND atomically refuses storage with any outstanding readers.
         */
        if (d->pool[i].slot != UINT32_MAX && d->pool[i].slot != out) continue;
        Surface spare = { .storage = d->pool[i] };
        if (storage_operation(d, &spare, CEDAR_SURFACE_BIND, out)) {
            if (errno == EBUSY) continue;
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        target->storage = spare.storage;
        d->pool[i] = d->pool[--d->pool_count];
        d->storage_reuses++;
        return VA_STATUS_SUCCESS;
    }
    if (storage_operation(d, target, CEDAR_SURFACE_CREATE, UINT32_MAX)) {
        /* A full application surface set can consume the handle/page quota.
         * Evict a cached handle before retrying; live readers still pin its
         * allocation, so neither eviction nor retry can overwrite them.
         */
        if (!d->pool_count || (errno != ENOSPC && errno != EDQUOT))
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        Surface spare = { .storage = d->pool[d->pool_count - 1] };
        if (storage_operation(d, &spare, CEDAR_SURFACE_DESTROY, UINT32_MAX))
            return VA_STATUS_ERROR_OPERATION_FAILED;
        d->pool_count--;
        if (storage_operation(d, target, CEDAR_SURFACE_CREATE, UINT32_MAX))
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (storage_operation(d, target, CEDAR_SURFACE_BIND, out)) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    return VA_STATUS_SUCCESS;
}
static VAStatus prediction_weights(Driver *d, const VASliceParameterBufferH264 *s, unsigned ref,
                                   struct cedar_h264_weights *w, bool *nonidentity) {
    if (s->luma_log2_weight_denom > 7 || s->chroma_log2_weight_denom > 7 ||
        s->luma_weight_l0_flag > 1 || s->chroma_weight_l0_flag > 1) return VA_STATUS_ERROR_INVALID_PARAMETER;
    w->luma_denom = s->luma_log2_weight_denom; w->chroma_denom = s->chroma_log2_weight_denom;
    int yw = 1 << w->luma_denom, cw = 1 << w->chroma_denom;
    w->luma_weight = s->luma_weight_l0_flag ? s->luma_weight_l0[ref] : yw;
    w->luma_offset = s->luma_weight_l0_flag ? s->luma_offset_l0[ref] : 0;
    w->cb_weight = s->chroma_weight_l0_flag ? s->chroma_weight_l0[ref][0] : cw;
    w->cb_offset = s->chroma_weight_l0_flag ? s->chroma_offset_l0[ref][0] : 0;
    w->cr_weight = s->chroma_weight_l0_flag ? s->chroma_weight_l0[ref][1] : cw;
    w->cr_offset = s->chroma_weight_l0_flag ? s->chroma_offset_l0[ref][1] : 0;
    *nonidentity |= w->luma_weight != yw || w->luma_offset || w->cb_weight != cw ||
                    w->cb_offset || w->cr_weight != cw || w->cr_offset;
    if (!cedar_h264_weights_valid(w, 1)) {
        fprintf(stderr, "cedar-va: unsupported prediction weights: ref=%u denom=%u/%u Y=%d/%d Cb=%d/%d Cr=%d/%d\n",
                ref, w->luma_denom, w->chroma_denom, w->luma_weight, w->luma_offset,
                w->cb_weight, w->cb_offset, w->cr_weight, w->cr_offset);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (d->identity_weights) {
        w->luma_weight = yw; w->cb_weight = w->cr_weight = cw;
        w->luma_offset = w->cb_offset = w->cr_offset = 0;
    }
    return VA_STATUS_SUCCESS;
}
#pragma GCC diagnostic push
/* The legacy FMO field is deprecated, but still reject unsupported layouts. */
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
typedef struct {
    unsigned char *packet; size_t bytes;
    int out, poc, origin; unsigned type, l0_count;
    bool explicit_p, nonidentity, p_ref_packet, older_first, stable_packet, dpb_packet;
    unsigned l1_count, max_refs;
    bool explicit_dpb, reference, idr;
    bool discard;
    VASurfaceID retained[CEDAR_H264_MAX_REFS];
    unsigned retained_count;
} PreparedSlice;

static VAStatus prepare_slice(Driver *d, VASliceParameterBufferH264 *s, bool force_p_refs, PreparedSlice *prepared) {
    VAPictureParameterBufferH264 *p = &d->picture;
    Surface *target = surface(d, d->target);
    if (!d->have_picture || !d->have_slice || !d->bitstream || !target) {
        fprintf(stderr, "cedar-va: incomplete picture (%u/%u/%u/%u)\n", d->have_picture, d->have_slice, !!d->bitstream, !!target);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    unsigned config = d->caps.config_flags;
    bool dynamic = runtime_config(d);
    bool dpb_packet = dynamic && (d->caps.features & CEDAR_CAP_H264_DPB4);
    bool explicit_dpb = dpb_packet && (d->caps.features & CEDAR_CAP_H264_EXPLICIT_DPB);
    bool wide = explicit_dpb && (d->caps.features & CEDAR_CAP_H264_FULL_LISTS);
    unsigned max_refs = p->num_ref_frames ? p->num_ref_frames : 1;
    struct cedar_h264_config controls = {
        .size = sizeof(controls), .version = 1, .width = d->width, .height = d->height,
        .profile_idc = d->profile == VAProfileH264ConstrainedBaseline ? 66 : d->profile == VAProfileH264Main ? 77 : 100,
        .config_flags = (p->pic_fields.bits.entropy_coding_mode_flag ? CEDAR_CONFIG_CABAC : 0) |
            (p->pic_fields.bits.transform_8x8_mode_flag ? CEDAR_CONFIG_TRANSFORM8 : 0) |
            (p->pic_fields.bits.constrained_intra_pred_flag ? CEDAR_CONFIG_CONSTRAINED : 0) |
            (p->seq_fields.bits.direct_8x8_inference_flag ? CEDAR_CONFIG_DIRECT8 : 0) |
            (p->pic_fields.bits.weighted_pred_flag ? CEDAR_CONFIG_WEIGHT_P : 0) |
            (p->pic_fields.bits.weighted_bipred_idc << CEDAR_CONFIG_WEIGHT_B_SHIFT),
        .chroma_qp_offset = p->chroma_qp_index_offset, .second_chroma_qp_offset = p->second_chroma_qp_index_offset,
        .disable_deblock = s->disable_deblocking_filter_idc, .alpha = s->slice_alpha_c0_offset_div2,
        .beta = s->slice_beta_offset_div2,
    };
    if ((p->picture_width_in_mbs_minus1 + 1U) * 16 != d->width ||
        (p->picture_height_in_mbs_minus1 + 1U) * 16 != d->height ||
        target->width < d->width || target->height < d->height ||
        p->bit_depth_luma_minus8 || p->bit_depth_chroma_minus8 || p->num_slice_groups_minus1 ||
        !p->seq_fields.bits.frame_mbs_only_flag || p->seq_fields.bits.mb_adaptive_frame_field_flag ||
        p->seq_fields.bits.chroma_format_idc != 1 || p->pic_fields.bits.field_pic_flag || p->num_ref_frames > (wide ? 32 : dpb_packet ? CEDAR_H264_MAX_REFS : 2) ||
        (p->pic_fields.bits.weighted_pred_flag && !(dynamic && (d->caps.features & CEDAR_CAP_H264_WEIGHT_P))) ||
        (dynamic ? !cedar_h264_config_valid(&controls, d->caps.width, d->caps.height) :
            controls.config_flags != config || p->chroma_qp_index_offset != d->caps.chroma_qp_offset ||
            p->second_chroma_qp_index_offset != d->caps.second_chroma_qp_offset)) {
        fprintf(stderr, "cedar-va: unsupported picture configuration\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (s->slice_data_flag != VA_SLICE_DATA_FLAG_ALL ||
        (!dynamic && (s->disable_deblocking_filter_idc != d->caps.disable_deblock || s->slice_alpha_c0_offset_div2 != d->caps.alpha ||
        s->slice_beta_offset_div2 != d->caps.beta)) || s->slice_data_offset > d->bitstream_bytes ||
        !s->slice_data_size || s->slice_data_size > d->caps.max_slice_bytes ||
        s->slice_data_size > d->bitstream_bytes - s->slice_data_offset) {
        fprintf(stderr, "cedar-va: unsupported slice: first=%u flag=%u deblock=%u alpha=%d beta=%d offset=%u size=%u available=%zu\n",
                s->first_mb_in_slice, s->slice_data_flag, s->disable_deblocking_filter_idc, s->slice_alpha_c0_offset_div2,
                s->slice_beta_offset_div2, s->slice_data_offset, s->slice_data_size, d->bitstream_bytes);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    unsigned type = s->slice_type % 5;
    const unsigned char *nal = d->bitstream + s->slice_data_offset;
    unsigned nal_type = nal[0] & 31;
    bool reference_picture = !!(nal[0] & 0x60);
    int qp = 26 + p->pic_init_qp_minus26 + s->slice_qp_delta;
    bool idr = type == 2 && nal_type == 5;
    int origin = idr || (wide && type == 2 && !d->ref_limit) ? p->CurrPic.TopFieldOrderCnt : d->poc_origin;
    unsigned packet_type = type == 2 ? (idr ? 5U : 6U) : type == 0 ? 1U : 2U;
    int64_t relative_poc = (int64_t)p->CurrPic.TopFieldOrderCnt - origin;
    bool preroll = wide && d->recovery_preroll && type == 1 && relative_poc < 0 && relative_poc >= -65535;
    if (p->CurrPic.picture_id != d->target || p->CurrPic.BottomFieldOrderCnt != p->CurrPic.TopFieldOrderCnt ||
        (!preroll && relative_poc < 0) || relative_poc > (wide ? 0x7ffffffe : 65534) || (relative_poc & 1) ||
        qp < 0 || qp > 51 || type > 2 || (type == 2 ? (nal_type != 5 && !(wide && nal_type == 1)) : nal_type != 1) ||
        (!explicit_dpb && reference_picture == (type == 1))) {
        fprintf(stderr, "cedar-va: unsupported picture: target=%u/%u POC=%d/%d QP=%d type=%u NAL=%02x bit_offset=%u\n",
                p->CurrPic.picture_id, d->target, p->CurrPic.TopFieldOrderCnt, p->CurrPic.BottomFieldOrderCnt, qp, type, nal[0], s->slice_data_bit_offset);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (preroll) {
        /* Before an open-GOP recovery picture, leading B pictures can need
         * references absent after seeking. They are not valid output. Defer
         * their failure to surface synchronization/download, as with other
         * asynchronous decode errors, without destroying valid post-seek DPB.
         * A later picture may never use one as an active reference. */
        *prepared = (PreparedSlice){.discard=true,.poc=(int)relative_poc,.origin=origin,.type=type};
        return VA_STATUS_SUCCESS;
    }
    int poc = (int)relative_poc;
    bool stable_packet = !dpb_packet && type != 2 && dynamic && (d->caps.features & CEDAR_CAP_H264_STABLE_REFS);
    unsigned l1_count = type == 1 ? (unsigned)s->num_ref_idx_l1_active_minus1 + 1 : 0;
    unsigned l0_count = type == 0 || (type == 1 && (stable_packet || dpb_packet)) ? (unsigned)s->num_ref_idx_l0_active_minus1 + 1 : 1;
    if (l0_count > (wide ? 32 : dpb_packet ? CEDAR_H264_MAX_REFS : 2) || (l0_count > 1 && !(d->caps.features & CEDAR_CAP_H264_P_REF2))) {
        fprintf(stderr, "cedar-va: unsupported active list0 count: %u (stored references=%u)\n", l0_count, max_refs);
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }
    if (type == 1 && !stable_packet && !dpb_packet && (s->num_ref_idx_l0_active_minus1 || d->p_refs)) return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    if (l1_count > (wide ? 32 : dpb_packet ? CEDAR_H264_MAX_REFS : 2) || (type == 1 && !stable_packet && !dpb_packet && s->num_ref_idx_l1_active_minus1)) return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    bool explicit_p = type == 0 && p->pic_fields.bits.weighted_pred_flag;
    bool weight_packet = dynamic && p->pic_fields.bits.weighted_pred_flag;
    struct cedar_h264_weights weights[32];
    for (unsigned i = 0; i < 32; i++) weights[i] = (struct cedar_h264_weights){ .size = 64, .version = 1 };
    bool nonidentity = false;
    if (explicit_p) {
        for (unsigned i = 0; i < l0_count; i++) {
            VAStatus status = prediction_weights(d, s, i, &weights[i], &nonidentity);
            if (status != VA_STATUS_SUCCESS) return status;
        }
    }
    int forward = -1, backward = -1, out = 0;
    unsigned fpoc = 0, bpoc = 0;
    bool p_ref_packet = false, older_first = false;
    struct cedar_h264_p_refs refs = { .size = sizeof(refs), .version = 1, .count = l0_count, .slots = { BAD, BAD } };
    struct cedar_h264_refs stable = { .size = 64, .version = 1, .l0_count = l0_count, .l1_count = l1_count,
        .l0_slots = { BAD, BAD }, .l1_slots = { BAD, BAD } };
    struct cedar_h264_dpb dpb = { .size = sizeof(dpb), .version = 1, .max_refs = max_refs,
        .l0_count = type == 2 ? 0 : l0_count, .l1_count = l1_count,
        .l0_slots = { BAD, BAD, BAD, BAD }, .l1_slots = { BAD, BAD, BAD, BAD } };
    struct cedar_h264_dpb32 full = { .size=sizeof(full), .version=1, .max_refs=max_refs,
        .l0_count=type==2?0:l0_count, .l1_count=l1_count, .reference=reference_picture };
    for (unsigned i=0;i<32;i++) full.l0_slots[i]=full.l1_slots[i]=BAD;
    unsigned *list0_slots=wide?full.l0_slots:dpb.l0_slots, *list1_slots=wide?full.l1_slots:dpb.l1_slots;
    unsigned *list0_pocs=wide?full.l0_pocs:dpb.l0_pocs, *list1_pocs=wide?full.l1_pocs:dpb.l1_pocs;
    VASurfaceID retained_ids[CEDAR_H264_MAX_REFS];
    unsigned retained_count = 0;
    if (explicit_dpb) {
        dpb.version = 2;
        dpb.reserved[1] = reference_picture;
        if (!idr) for (unsigned i = 0; i < 16; i++) {
            VAPictureH264 *entry = &p->ReferenceFrames[i];
            if (entry->flags & VA_PICTURE_H264_INVALID) continue;
            Surface *ref = surface(d, entry->picture_id);
            int slot = slot_of(d, entry->picture_id);
            bool known = false;
            for (unsigned j = 0; j < d->ref_count; j++) known |= d->history[j] == entry->picture_id;
            if (entry->flags & VA_PICTURE_H264_LONG_TERM_REFERENCE) return VA_STATUS_ERROR_INVALID_SURFACE;
            if (!known || !ref || !ref->ready || ref->error || slot < 0) {
                /* Open-GOP random access can leave unavailable, inactive
                 * entries in the client's DPB. No hardware address is needed
                 * for those entries. Every active reference is checked below. */
                bool used = false;
                if (type != 2) for (unsigned j=0;j<l0_count;j++) used |= s->RefPicList0[j].picture_id == entry->picture_id;
                for (unsigned j=0;j<l1_count;j++) used |= s->RefPicList1[j].picture_id == entry->picture_id;
                if (used) return VA_STATUS_ERROR_INVALID_SURFACE;
                continue;
            }
            if (entry->picture_id == d->target ||
                (int64_t)entry->TopFieldOrderCnt != (int64_t)ref->poc + origin ||
                entry->BottomFieldOrderCnt != entry->TopFieldOrderCnt ||
                (dpb.reserved[0] & (1U << slot)) || retained_count >= max_refs)
                return VA_STATUS_ERROR_INVALID_SURFACE;
            dpb.reserved[0] |= 1U << slot;
            retained_ids[retained_count++] = entry->picture_id;
        }
    }
    full.retained_mask=dpb.reserved[0];
    if (dpb_packet) {
        unsigned active_count = explicit_dpb ? retained_count : d->ref_count;
        VASurfaceID *active = explicit_dpb ? retained_ids : d->history;
        if (!idr) for (unsigned j = 0; j < active_count; j++)
            if (active[j] == d->target) return VA_STATUS_ERROR_INVALID_SURFACE;
        if (type != 2 && d->ref_limit != max_refs) return VA_STATUS_ERROR_INVALID_PARAMETER;
        for (unsigned list = 0; list < 2; list++) {
            unsigned count = list ? l1_count : type==2?0:l0_count;
            VAPictureH264 *pictures = list ? s->RefPicList1 : s->RefPicList0;
            for (unsigned i = 0; i < count; i++) {
                VASurfaceID id = pictures[i].picture_id;
                Surface *reference = surface(d, id); int slot = slot_of(d, id);
                bool retained = false;
                for (unsigned j = 0; j < active_count; j++) retained |= active[j] == id;
                if (!reference || !reference->ready || reference->error || slot < 0 || !retained || id == d->target ||
                    (pictures[i].flags & (VA_PICTURE_H264_INVALID | VA_PICTURE_H264_LONG_TERM_REFERENCE)) ||
                    (int64_t)pictures[i].TopFieldOrderCnt != (int64_t)reference->poc + origin ||
                    pictures[i].BottomFieldOrderCnt != pictures[i].TopFieldOrderCnt)
                    return VA_STATUS_ERROR_INVALID_SURFACE;
                (list ? list1_slots : list0_slots)[i] = (unsigned)slot;
                (list ? list1_pocs : list0_pocs)[i] = (unsigned)reference->poc;
            }
        }
        if (wide ? !cedar_h264_dpb32_valid(&full, packet_type) : explicit_dpb ? !cedar_h264_explicit_dpb_valid(&dpb, packet_type) :
            !cedar_h264_dpb_valid(&dpb, packet_type)) {
            fprintf(stderr, "cedar-va: unsupported DPB lists: refs=%u l0=%u [%u,%u,%u,%u] l1=%u\n",
                    max_refs, dpb.l0_count, dpb.l0_slots[0], dpb.l0_slots[1], dpb.l0_slots[2], dpb.l0_slots[3], dpb.l1_count);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (!idr) {
            bool busy;
            do {
                busy = false;
                for (unsigned j = 0; j < active_count; j++) busy |= out == slot_of(d, active[j]);
                if (busy) out++;
            } while (busy && out < (int)CEDAR_H264_SURFACES);
            if (out >= (int)CEDAR_H264_SURFACES) return VA_STATUS_ERROR_INVALID_SURFACE;
            if (type != 2) { forward = (int)list0_slots[0]; fpoc = list0_pocs[0]; }
            if (type == 1) { backward = (int)list1_slots[0]; bpoc = list1_pocs[0]; }
        }
        p_ref_packet = type == 0;
        older_first = type == 0 && s->RefPicList0[0].picture_id != d->last;
    } else if (stable_packet) {
        for (unsigned list = 0; list < 2; list++) {
            unsigned count = list ? l1_count : l0_count;
            VAPictureH264 *pictures = list ? s->RefPicList1 : s->RefPicList0;
            for (unsigned i = 0; i < count; i++) {
                VASurfaceID id = pictures[i].picture_id;
                Surface *reference = surface(d, id); int slot = slot_of(d, id);
                if (!reference || !reference->ready || reference->error || slot < 0 || id == d->target ||
                    (id != d->last && id != d->previous) ||
                    (pictures[i].flags & (VA_PICTURE_H264_INVALID | VA_PICTURE_H264_LONG_TERM_REFERENCE)) ||
                    (int64_t)pictures[i].TopFieldOrderCnt != (int64_t)reference->poc + origin ||
                    pictures[i].BottomFieldOrderCnt != pictures[i].TopFieldOrderCnt)
                    return VA_STATUS_ERROR_INVALID_SURFACE;
                (list ? stable.l1_slots : stable.l0_slots)[i] = (unsigned)slot;
                (list ? stable.l1_pocs : stable.l0_pocs)[i] = (unsigned)reference->poc;
            }
        }
        if (!cedar_h264_refs_valid(&stable, type == 1)) return VA_STATUS_ERROR_INVALID_PARAMETER;
        forward = (int)stable.l0_slots[0]; fpoc = stable.l0_pocs[0];
        if (type == 1) { backward = (int)stable.l1_slots[0]; bpoc = stable.l1_pocs[0]; }
        while (out == slot_of(d, d->last) || out == slot_of(d, d->previous)) out++;
        if (out > 2) return VA_STATUS_ERROR_INVALID_SURFACE;
        p_ref_packet = type == 0;
        older_first = type == 0 && s->RefPicList0[0].picture_id != d->last;
    } else if (type != 2) {
        VASurfaceID fid = s->RefPicList0[0].picture_id;
        Surface *fs = surface(d, fid);
        forward = slot_of(d, fid);
        if (!fs || !fs->ready || fs->error || forward < 0 || d->target == fid ||
            (s->RefPicList0[0].flags & VA_PICTURE_H264_LONG_TERM_REFERENCE)) return VA_STATUS_ERROR_INVALID_SURFACE;
        if (type == 0) {
            older_first = fid != d->last;
            if (fid != d->last && fid != d->previous) return VA_STATUS_ERROR_INVALID_SURFACE;
            p_ref_packet = force_p_refs || d->p_refs || l0_count == 2 || older_first;
            if (p_ref_packet && !(d->caps.features & CEDAR_CAP_H264_P_REF2)) return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
            for (unsigned i = 0; i < l0_count; i++) {
                VASurfaceID ref_id = s->RefPicList0[i].picture_id;
                Surface *reference = surface(d, ref_id); int slot = slot_of(d, ref_id);
                if (!reference || !reference->ready || reference->error || slot < 0 || ref_id == d->target ||
                    (ref_id != d->last && ref_id != d->previous) ||
                    (s->RefPicList0[i].flags & VA_PICTURE_H264_LONG_TERM_REFERENCE)) return VA_STATUS_ERROR_INVALID_SURFACE;
                refs.slots[i] = (unsigned)slot; refs.pocs[i] = (unsigned)reference->poc;
            }
            if (!cedar_h264_p_refs_valid(&refs)) return VA_STATUS_ERROR_INVALID_PARAMETER;
        } else if (fid != d->previous) return VA_STATUS_ERROR_INVALID_SURFACE;
        fpoc = (unsigned)fs->poc;
        if (type == 1) {
            VASurfaceID bid = s->RefPicList1[0].picture_id;
            Surface *bs = surface(d, bid); backward = slot_of(d, bid);
            if (!bs || backward < 0 || bid != d->last || d->target == bid) return VA_STATUS_ERROR_INVALID_SURFACE;
            bpoc = (unsigned)bs->poc;
        }
        while (out == forward || out == backward || (p_ref_packet &&
            (out == slot_of(d, d->last) || out == slot_of(d, d->previous)))) out++;
        if (out > 2) return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    unsigned length_offset = wide ? CEDAR_H264_FULL_LENGTH_OFFSET : dpb_packet ? 544 : stable_packet || p_ref_packet ? 352 : weight_packet ? 224 : dynamic ? 160 : 96;
    size_t packet_bytes = length_offset + 4U + s->slice_data_size;
    unsigned char *packet = calloc(1, packet_bytes);
    if (!packet) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    memcpy(packet, "CDV2", 4);
    const uint32_t header[] = { wide ? 14U : explicit_dpb ? 13U : dpb_packet ? 12U : stable_packet ? 11U : p_ref_packet ? 9U : weight_packet ? 8U : dynamic ? 7U : 6U, d->width, d->height, 1, 1, d->disable_submission ? 1U : 0U, (uint32_t)packet_bytes - 32 };
    for (unsigned i = 0; i < 7; i++) word(packet, 4 + 4 * i, header[i]);
    const uint32_t control[] = { s->slice_data_bit_offset, (unsigned)qp, 0, packet_type,
        (unsigned)out, forward < 0 ? BAD : (unsigned)forward, backward < 0 ? BAD : (unsigned)backward,
        s->cabac_init_idc, (unsigned)poc, fpoc, bpoc, s->direct_spatial_mv_pred_flag, 0, 0, 0, 0 };
    for (unsigned i = 0; i < 16; i++) word(packet, 32 + 4 * i, control[i]);
    if (dynamic) {
        uint32_t words[16];
        _Static_assert(sizeof(controls) == sizeof(words), "runtime controls ABI size");
        memcpy(words, &controls, sizeof(words));
        for (unsigned i = 0; i < 16; i++) word(packet, 96 + 4 * i, words[i]);
    }
    if (!dpb_packet && (weight_packet || p_ref_packet || stable_packet)) {
        uint32_t words[16];
        _Static_assert(sizeof(weights[0]) == sizeof(words), "prediction weights ABI size");
        memcpy(words, &weights[0], sizeof(words));
        for (unsigned i = 0; i < 16; i++) word(packet, 160 + 4 * i, words[i]);
    }
    if (!dpb_packet && (p_ref_packet || stable_packet)) {
        uint32_t words[16];
        _Static_assert(sizeof(refs) == sizeof(words), "P reference ABI size");
        memcpy(words, stable_packet ? (const void *)&stable : (const void *)&refs, sizeof(words));
        for (unsigned i = 0; i < 16; i++) word(packet, 224 + 4 * i, words[i]);
        memcpy(words, &weights[1], sizeof(words));
        for (unsigned i = 0; i < 16; i++) word(packet, 288 + 4 * i, words[i]);
    }
    if (wide) {
        uint32_t words[sizeof(full)/4];
        memcpy(words, &full, sizeof(full));
        for(unsigned i=0;i<sizeof(full)/4;i++) word(packet,160+4*i,words[i]);
        for(unsigned j=0;j<32;j++) {
            memcpy(words,&weights[j],sizeof(weights[j]));
            for(unsigned i=0;i<16;i++) word(packet,704+64*j+4*i,words[i]);
        }
    } else if (dpb_packet) {
        uint32_t words[32];
        _Static_assert(sizeof(dpb) == sizeof(words), "DPB controls ABI size");
        memcpy(words, &dpb, sizeof(words));
        for (unsigned i = 0; i < 32; i++) word(packet, 160 + 4*i, words[i]);
        for (unsigned w = 0; w < CEDAR_H264_MAX_REFS; w++) {
            memcpy(words, &weights[w], sizeof(weights[w]));
            for (unsigned i = 0; i < 16; i++) word(packet, 288 + 64*w + 4*i, words[i]);
        }
    }
    word(packet, length_offset, s->slice_data_size); memcpy(packet + length_offset + 4, nal, s->slice_data_size);
    *prepared = (PreparedSlice){ .packet = packet, .bytes = packet_bytes, .out = out, .poc = poc, .origin = origin,
        .explicit_dpb = explicit_dpb, .reference = reference_picture, .idr=idr, .retained_count = retained_count,
        .type = type, .l0_count = l0_count, .explicit_p = explicit_p, .nonidentity = nonidentity,
        .p_ref_packet = p_ref_packet, .older_first = older_first, .stable_packet = stable_packet, .l1_count = l1_count, .dpb_packet = dpb_packet, .max_refs = max_refs };
    memcpy(prepared->retained, retained_ids, retained_count * sizeof(retained_ids[0]));
    return VA_STATUS_SUCCESS;
}

static VAStatus prepare_picture(Driver *d, PreparedSlice *result) {
    PreparedSlice prepared[CEDAR_H264_MAX_SLICES] = {{0}};
    VAStatus status = VA_STATUS_SUCCESS;
    bool force_p_refs = d->p_refs;
    unsigned type;
    if (!d->slice_count || d->unbound_slice != d->slice_count || d->slices[0].param.first_mb_in_slice)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    type = d->slices[0].param.slice_type % 5;
    for (unsigned i = 0; i < d->slice_count; i++) {
        const VASliceParameterBufferH264 *s = &d->slices[i].param;
        if (s->slice_type % 5 != type || s->first_mb_in_slice >= d->width*d->height/256 ||
            (i && s->first_mb_in_slice <= d->slices[i-1].param.first_mb_in_slice))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        if (type == 0 && (s->num_ref_idx_l0_active_minus1 || s->RefPicList0[0].picture_id != d->last))
            force_p_refs = true;
    }
    for (unsigned i = 0; i < d->slice_count; i++) {
        StagedSlice *staged = &d->slices[i];
        VASliceParameterBufferH264 s = staged->param;
        if (s.slice_data_offset > staged->bytes || s.slice_data_size > staged->bytes-s.slice_data_offset) {
            status = VA_STATUS_ERROR_INVALID_PARAMETER; goto cleanup;
        }
        s.slice_data_offset += (unsigned)staged->base;
        status = prepare_slice(d, &s, force_p_refs, &prepared[i]);
        if (status != VA_STATUS_SUCCESS) goto cleanup;
        if (i && (prepared[i].discard != prepared[0].discard || prepared[i].out != prepared[0].out || prepared[i].poc != prepared[0].poc || prepared[i].origin != prepared[0].origin)) {
            status = VA_STATUS_ERROR_INVALID_PARAMETER; goto cleanup;
        }
    }
    *result = prepared[0];
    if (result->discard) goto cleanup;
    if (d->slice_count == 1) { prepared[0].packet = NULL; goto cleanup; }
    size_t total = 32;
    for (unsigned i = 0; i < d->slice_count; i++) total += 8 + prepared[i].bytes;
    if (total > ((d->caps.features & CEDAR_CAP_H264_FULL_LISTS) ? CEDAR_H264_PACKET_LIMIT : CEDAR_H264_MULTI_LIMIT)) { status = VA_STATUS_ERROR_INVALID_PARAMETER; goto cleanup; }
    unsigned char *packet = calloc(1,total);
    if (!packet) { status = VA_STATUS_ERROR_ALLOCATION_FAILED; goto cleanup; }
    memcpy(packet,"CDV2",4);
    const uint32_t header[] = {10,d->width,d->height,d->slice_count,1,d->disable_submission ? 1U : 0U,(uint32_t)total-32};
    for (unsigned i=0;i<7;i++) word(packet,4+4*i,header[i]);
    size_t position=32;
    for (unsigned i=0;i<d->slice_count;i++) {
        word(packet,(unsigned)position,d->slices[i].param.first_mb_in_slice);
        word(packet,(unsigned)position+4,(uint32_t)prepared[i].bytes);
        word(prepared[i].packet,24,0); /* Only the outer picture has test flags. */
        memcpy(packet+position+8,prepared[i].packet,prepared[i].bytes);
        position += 8+prepared[i].bytes;
        result->nonidentity |= prepared[i].nonidentity;
        result->older_first |= prepared[i].older_first;
        if (prepared[i].l0_count > result->l0_count) result->l0_count=prepared[i].l0_count;
        if (prepared[i].l1_count > result->l1_count) result->l1_count=prepared[i].l1_count;
    }
    result->packet=packet; result->bytes=total;
cleanup:
    for (unsigned i=0;i<d->slice_count;i++) free(prepared[i].packet);
    return status;
}

static void commit_references(Driver *d, const PreparedSlice *p) {
    int type = (int)p->type, out = p->out, origin = p->origin;
    if (p->idr || (type == 2 && !d->ref_limit)) {
        bool recovery = !p->idr;
        reset_references(d); d->poc_origin = origin; d->recovery_preroll = recovery;
    }
    for (unsigned i = 0; i < CEDAR_H264_SURFACES; i++) if (d->slots[i] == d->target) d->slots[i] = BAD;
    d->slots[out] = d->target;
    if (p->dpb_packet) {
        d->ref_limit = p->max_refs;
        if (p->explicit_dpb) {
            d->ref_count = p->retained_count;
            memcpy(d->history, p->retained, d->ref_count * sizeof(d->history[0]));
        }
        if (p->explicit_dpb ? p->reference : type != 1) {
            if (!p->explicit_dpb && d->ref_count == d->ref_limit) {
                d->ref_count--;
                memmove(d->history, d->history + 1, d->ref_count * sizeof(d->history[0]));
            }
            d->history[d->ref_count++] = d->target;
        }
        d->dpb_frames++;
        if (p->l0_count > d->max_l0) d->max_l0 = p->l0_count;
        if (p->l1_count > d->max_l1) d->max_l1 = p->l1_count;
    }
    if (type != 1) { d->previous = type == 2 ? BAD : d->last; d->last = d->target; }
}

static bool frame_layout_valid(const Driver *d, const struct cedar_frame_export *frame, unsigned slot) {
    return cedar_h264_picture_geometry_valid(d->width, d->height) &&
        frame->width == d->width && frame->height == d->height &&
        frame->stride == CEDAR_H264_STORAGE_STRIDE && frame->uv_offset == CEDAR_H264_STORAGE_UV_OFFSET &&
        frame->image_bytes == bytes_for(d->width, d->height) && frame->mapping_bytes == CEDAR_H264_STORAGE_PAGES*4096 &&
        frame->fourcc == CEDAR_FRAME_NV12 && frame->slot == slot && frame->id && frame->fd >= 0;
}

static VAStatus decode_picture(Driver *d) {
    PreparedSlice prepared;
    VAStatus status = prepare_picture(d,&prepared);
    if (status != VA_STATUS_SUCCESS) return status;
    Surface *target=surface(d,d->target);
    if (prepared.discard) {
        drop_frame(target); target->ready=false; target->preroll=true;
        d->discarded_preroll++;
        return VA_STATUS_SUCCESS;
    }
    unsigned char *packet=prepared.packet; size_t packet_bytes=prepared.bytes;
    int type=(int)prepared.type, poc=prepared.poc;
    if (d->fd < 0) d->fd = cedar_decoder_open(&d->broker);
    if (d->fd < 0) { free(packet); return VA_STATUS_ERROR_OPERATION_FAILED; }
    if (d->dmabuf) {
        status = bind_destination(d, target, (unsigned)prepared.out);
        if (status != VA_STATUS_SUCCESS) { free(packet); return status; }
    }
    const char *dump_path = getenv("CEDAR_VA_TEST_PACKET_PATH");
    if (dump_path && !d->packet_dumped) {
        int dump = open(dump_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (dump < 0) { free(packet); return VA_STATUS_ERROR_OPERATION_FAILED; }
        ssize_t copied = write(dump, packet, packet_bytes);
        int closed = close(dump);
        if (copied != (ssize_t)packet_bytes || closed) { free(packet); return VA_STATUS_ERROR_OPERATION_FAILED; }
        d->packet_dumped = true;
    }
    ssize_t result;
    do result = cedar_decoder_submit(d->fd, d->broker, packet, packet_bytes); while (result < 0 && errno == EINTR);
    free(packet);
    if (result != (ssize_t)packet_bytes) {
        fprintf(stderr, "cedar-va: picture submission failed (%s), slices=%u type=%d POC=%d\n", strerror(errno), d->slice_count, type, poc);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    if (d->dmabuf) {
        struct cedar_frame_export frame = { .version = 1, .fd = -1 };
        if (cedar_decoder_export(d->fd, d->broker, &frame)) return VA_STATUS_ERROR_DECODING_ERROR;
        if (!frame_layout_valid(d, &frame, (unsigned)prepared.out)) {
            if (frame.fd >= 0) close(frame.fd);
            return VA_STATUS_ERROR_DECODING_ERROR;
        }
        target->frame = frame;
        for (unsigned i = 0; i < NSURF; i++)
            if (d->surfaces[i].storage.handle && d->surfaces[i].storage.slot == (unsigned)prepared.out)
                d->surfaces[i].storage.slot = UINT32_MAX;
        for (unsigned i = 0; i < d->pool_count; i++)
            if (d->pool[i].slot == (unsigned)prepared.out) d->pool[i].slot = UINT32_MAX;
        target->storage.slot = (unsigned)prepared.out;
    } else {
    size_t expected = bytes_for(d->width, d->height), received = 0;
    bool packed = target->width == d->width && target->height == d->height;
    unsigned char *pixels = packed ? target->data : malloc(expected);
    if (!pixels) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    while (received < expected) {
        result = read(d->fd, pixels + received, expected - received);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) break;
        received += (size_t)result;
    }
    unsigned char extra;
    do result = read(d->fd, &extra, 1); while (result < 0 && errno == EINTR);
    if (received != expected || result != 0) { if (!packed) free(pixels); return VA_STATUS_ERROR_DECODING_ERROR; }
    if (!packed) { copy_nv12(target->data, target->width, target->height, pixels, d->width, d->width*d->height, d->width, d->height); free(pixels); }
    }
    commit_references(d, &prepared);
    if (prepared.explicit_p) { d->explicit_p_frames++; d->nonidentity_p_frames += prepared.nonidentity; }
    if (prepared.p_ref_packet) { d->p_refs = true; d->p_ref_frames++; d->two_ref_frames += prepared.l0_count == 2; d->older_first_frames += prepared.older_first; }
    if ((prepared.stable_packet || prepared.dpb_packet) && type == 1) {
        d->b_ref_frames++; d->b_two_ref_frames += prepared.l0_count >= 2 || prepared.l1_count >= 2;
        if (prepared.l0_count > d->b_max_l0) d->b_max_l0 = prepared.l0_count;
        if (prepared.l1_count > d->b_max_l1) d->b_max_l1 = prepared.l1_count;
    }
    target->poc = poc; target->ready = true; target->preroll=false; return VA_STATUS_SUCCESS;
}
#pragma GCC diagnostic pop
static VAStatus cdv_EndPicture(VADriverContextP ctx, VAContextID context) {
    LOCK; if (!d->context || context != CONTEXT_ID || !d->begun) DONE(VA_STATUS_ERROR_INVALID_CONTEXT);
    VAStatus result = d->render_failed ? VA_STATUS_ERROR_DECODING_ERROR : decode_picture(d);
    Surface *s = surface(d, d->target); if (s) s->error = result != VA_STATUS_SUCCESS;
    d->begun = false;
    if (result != VA_STATUS_SUCCESS) {
        if (d->fd >= 0) close(d->fd);
        d->fd = -1; reset_references(d);
        for (unsigned i = 0; i < NSURF; i++) memset(&d->surfaces[i].storage, 0, sizeof(d->surfaces[i].storage));
    }
    free(d->bitstream); d->bitstream = NULL; d->bitstream_bytes = 0;
    DONE(result);
}

static VAStatus cdv_AssociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture, VASurfaceID *target_surfaces, int num_surfaces, short src_x, short src_y, unsigned short src_width, unsigned short src_height, short dest_x, short dest_y, unsigned short dest_width, unsigned short dest_height, unsigned int flags) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus cdv_CreateSubpicture(VADriverContextP ctx, VAImageID image, VASubpictureID *subpicture) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus cdv_DeassociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture, VASurfaceID *target_surfaces, int num_surfaces) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

/* Alias the cached CPU readback, never a GPU/DMA-BUF mapping. Derived images
 * retain that allocation even if the client destroys the originating surface. */
static VAStatus cdv_DeriveImage(VADriverContextP ctx, VASurfaceID id, VAImage *out) {
    LOCK; Surface *s = surface(d, id);
    if (!s) DONE(VA_STATUS_ERROR_INVALID_SURFACE);
    if (s->error || s->preroll) DONE(VA_STATUS_ERROR_DECODING_ERROR);
    if (d->dmabuf) DONE(VA_STATUS_ERROR_OPERATION_FAILED); /* Read-only export; use GetImage for explicit CPU copies. */
    for (unsigned i = 0; i < NIMAGE; i++) if (!d->images[i].id) {
        for (unsigned j = 0; j < NBUF; j++) if (!d->buffers[j].id) {
            Image *im = &d->images[i]; Buffer *b = &d->buffers[j];
            unsigned image_id = identifier(d), buffer_id = identifier(d);
            if (!image_id || !buffer_id) DONE(VA_STATUS_ERROR_ALLOCATION_FAILED);
            b->id = buffer_id; b->element = (unsigned)bytes_for(s->width, s->height); b->count = 1;
            b->capacity = b->element; b->type = VAImageBufferType; b->data = s->data; b->owner = s; s->image_refs++;
            im->id = image_id;
            im->image = (VAImage){ .image_id = im->id, .format = nv12, .buf = b->id,
                .width = (unsigned short)s->width, .height = (unsigned short)s->height,
                .data_size = b->element, .num_planes = 2, .pitches = { s->width, s->width, 0 },
                .offsets = { 0, s->width * s->height, 0 } };
            *out = im->image; DONE(VA_STATUS_SUCCESS);
        }
        break;
    }
    DONE(VA_STATUS_ERROR_ALLOCATION_FAILED);
}

static VAStatus cdv_ExportSurfaceHandle(VADriverContextP ctx, VASurfaceID id,
                                      uint32_t memory_type, uint32_t flags, void *descriptor) {
    LOCK;
    if (!d->dmabuf) DONE(VA_STATUS_ERROR_UNIMPLEMENTED);
    if (memory_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) DONE(VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE);
    if (!descriptor || !(flags & VA_EXPORT_SURFACE_READ_ONLY) || (flags & VA_EXPORT_SURFACE_WRITE_ONLY) ||
        (flags & ~(VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS | VA_EXPORT_SURFACE_COMPOSED_LAYERS)) ||
        ((flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) && (flags & VA_EXPORT_SURFACE_COMPOSED_LAYERS))) DONE(VA_STATUS_ERROR_INVALID_PARAMETER);
    Surface *s = surface(d, id);
    if (!s) DONE(VA_STATUS_ERROR_INVALID_SURFACE);
    if (s->error || s->preroll) DONE(VA_STATUS_ERROR_DECODING_ERROR);
    if (!s->ready && !s->frame.id) {
        if (!(d->caps.features & CEDAR_CAP_EXPORT_BLANK)) DONE(VA_STATUS_ERROR_OPERATION_FAILED);
        struct cedar_frame_export blank = { .version = 1, .fd = -1, .width = s->width, .height = s->height };
        bool broker = d->broker;
        int probe = d->fd >= 0 ? d->fd : cedar_decoder_open(&broker);
        if (probe < 0) DONE(VA_STATUS_ERROR_OPERATION_FAILED);
        int result = cedar_decoder_export_blank(probe, broker, &blank);
        if (probe != d->fd) close(probe);
        if (result) DONE(VA_STATUS_ERROR_OPERATION_FAILED);
        s->frame = blank;
    }
    if (!s->frame.id) DONE(VA_STATUS_ERROR_OPERATION_FAILED);
    int fd = fcntl(s->frame.fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) DONE(VA_STATUS_ERROR_ALLOCATION_FAILED);
    VADRMPRIMESurfaceDescriptor r = { .fourcc = VA_FOURCC_NV12, .width = s->width, .height = s->height, .num_objects = 1 };
    for (unsigned i = 0; i < 4; i++) r.objects[i].fd = -1;
    r.objects[0].fd = fd; r.objects[0].size = s->frame.mapping_bytes; r.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    bool separate = !!(flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS);
    r.num_layers = separate ? 2 : 1;
    r.layers[0].drm_format = separate ? DRM_FORMAT_R8 : DRM_FORMAT_NV12;
    r.layers[0].num_planes = separate ? 1 : 2; r.layers[0].pitch[0] = s->frame.stride;
    if (separate) {
        r.layers[1].drm_format = DRM_FORMAT_GR88; r.layers[1].num_planes = 1;
        r.layers[1].pitch[0] = s->frame.stride; r.layers[1].offset[0] = s->frame.uv_offset;
    } else { r.layers[0].pitch[1] = s->frame.stride; r.layers[0].offset[1] = s->frame.uv_offset; }
    *(VADRMPRIMESurfaceDescriptor *)descriptor = r;
    d->frame_exports++;
    DONE(VA_STATUS_SUCCESS);
}

static VAStatus cdv_DestroySubpicture(VADriverContextP ctx, VASubpictureID subpicture) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus cdv_GetDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *attr_list, int num_attributes) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus cdv_MFSubmit(VADriverContextP ctx, VAMFContextID mf_context, VAContextID *contexts, int num_contexts) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus cdv_PutImage(VADriverContextP ctx, VASurfaceID surface, VAImageID image, int src_x, int src_y, unsigned int src_width, unsigned int src_height, int dest_x, int dest_y, unsigned int dest_width, unsigned int dest_height) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus cdv_PutSurface(VADriverContextP ctx, VASurfaceID surface, void* draw, short srcx, short srcy, unsigned short srcw, unsigned short srch, short destx, short desty, unsigned short destw, unsigned short desth, VARectangle *cliprects, unsigned int number_cliprects, unsigned int flags) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus cdv_SetDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *attr_list, int num_attributes) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus cdv_SetImagePalette(VADriverContextP ctx, VAImageID image, unsigned char *palette) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus cdv_SetSubpictureChromakey(VADriverContextP ctx, VASubpictureID subpicture, unsigned int chromakey_min, unsigned int chromakey_max, unsigned int chromakey_mask) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus cdv_SetSubpictureGlobalAlpha(VADriverContextP ctx, VASubpictureID subpicture, float global_alpha) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus cdv_SetSubpictureImage(VADriverContextP ctx, VASubpictureID subpicture, VAImageID image) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

/* libva looks up an entry point matching its API version. Build against the
 * host headers rather than fixing this to the original Arch libva 1.24. */
#define CDV_VA_INIT_NAME_(major, minor) __vaDriverInit_##major##_##minor
#define CDV_VA_INIT_NAME(major, minor) CDV_VA_INIT_NAME_(major, minor)
__attribute__((visibility("default")))
VAStatus CDV_VA_INIT_NAME(VA_MAJOR_VERSION, VA_MINOR_VERSION)(VADriverContextP ctx)
{
    _Static_assert(sizeof(struct cedar_msvdx_caps) == 64, "private caps ABI size");
    Driver *d = calloc(1, sizeof(*d));
    if (!d) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    d->fd = -1;
    const char *disable = getenv("CEDAR_VA_TEST_DISABLE_SUBMISSION");
    d->disable_submission = disable && !strcmp(disable, "1");
    const char *identity = getenv("CEDAR_VA_TEST_IDENTITY_WEIGHTS");
    d->identity_weights = identity && !strcmp(identity, "1");
    const char *dmabuf = getenv("CEDAR_VA_DMABUF");
    d->dmabuf = dmabuf && !strcmp(dmabuf, "1");
    int fd = cedar_decoder_open(&d->broker);
    if (fd < 0 || cedar_decoder_caps(fd, d->broker, &d->caps)) {
        fprintf(stderr, "cedar-va: capability query failed: %s\n", strerror(errno));
        if (fd >= 0) close(fd);
        free(d); return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    close(fd);
    if (d->broker && !d->dmabuf) {
        fprintf(stderr, "cedar-va: broker requires explicit CEDAR_VA_DMABUF=1; no fallback\n");
        free(d); return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    unsigned required = CEDAR_CAP_FRAME_DMABUF | CEDAR_CAP_INDEPENDENT_SURFACES | CEDAR_CAP_SURFACE_BINDING | CEDAR_CAP_LAZY_READBACK;
    if (d->dmabuf && ((d->caps.features & required) != required || d->caps.width != CEDAR_H264_MAX_WIDTH || d->caps.height != CEDAR_H264_MAX_HEIGHT || d->caps.stride != CEDAR_H264_STORAGE_STRIDE)) {
        fprintf(stderr, "cedar-va: DMA-BUF mode requires matching backing layout and lazy readback\n");
        free(d); return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (d->caps.size != sizeof(d->caps) || d->caps.version != 1 || d->caps.codec != 1 ||
        !(d->caps.features & CEDAR_CAP_SINGLE_PICTURE) || d->caps.width < 16 || d->caps.width > CEDAR_H264_MAX_WIDTH ||
        d->caps.height < 16 || d->caps.height > CEDAR_H264_MAX_HEIGHT || !d->caps.max_slice_bytes || d->caps.max_slice_bytes > CEDAR_H264_MAX_SLICE_BYTES ||
        (d->caps.max_slice_bytes > 65535 && !(d->caps.features & CEDAR_CAP_H264_LARGE_SLICES))) {
        fprintf(stderr, "cedar-va: requires the matching persistent H.264 prototype\n");
        free(d); return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }
    d->profile = d->caps.profile_idc == 100 ? VAProfileH264High : d->caps.profile_idc == 77 ? VAProfileH264Main : VAProfileNone;
    if (d->profile == VAProfileNone) { free(d); return VA_STATUS_ERROR_UNSUPPORTED_PROFILE; }
    if (pthread_mutex_init(&d->mutex, NULL)) { free(d); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    reset_references(d);
    ctx->pDriverData = d;
    ctx->version_major = VA_MAJOR_VERSION; ctx->version_minor = VA_MINOR_VERSION;
    ctx->max_profiles = runtime_config(d) ? 3 : 1;
    ctx->max_entrypoints = ctx->max_image_formats = ctx->max_subpic_formats = 1;
    ctx->max_attributes = 16; ctx->max_display_attributes = 0;
    ctx->str_vendor = d->dmabuf ? "Experimental Cedarview MSVDX: restricted root decode, read-only DMA-BUF export" :
                                "Experimental Cedarview MSVDX: restricted root decode, CPU image copies";
    ctx->vtable->vaAssociateSubpicture = cdv_AssociateSubpicture;
    ctx->vtable->vaBeginPicture = cdv_BeginPicture;
    ctx->vtable->vaBufferInfo = cdv_BufferInfo;
    ctx->vtable->vaBufferSetNumElements = cdv_BufferSetNumElements;
    ctx->vtable->vaCreateBuffer = cdv_CreateBuffer;
    ctx->vtable->vaCreateConfig = cdv_CreateConfig;
    ctx->vtable->vaCreateContext = cdv_CreateContext;
    ctx->vtable->vaCreateImage = cdv_CreateImage;
    ctx->vtable->vaCreateSubpicture = cdv_CreateSubpicture;
    ctx->vtable->vaCreateSurfaces = cdv_CreateSurfaces;
    ctx->vtable->vaCreateSurfaces2 = cdv_CreateSurfaces2;
    ctx->vtable->vaDeassociateSubpicture = cdv_DeassociateSubpicture;
    ctx->vtable->vaDeriveImage = cdv_DeriveImage;
    ctx->vtable->vaDestroyBuffer = cdv_DestroyBuffer;
    ctx->vtable->vaDestroyConfig = cdv_DestroyConfig;
    ctx->vtable->vaDestroyContext = cdv_DestroyContext;
    ctx->vtable->vaDestroyImage = cdv_DestroyImage;
    ctx->vtable->vaDestroySubpicture = cdv_DestroySubpicture;
    ctx->vtable->vaDestroySurfaces = cdv_DestroySurfaces;
    ctx->vtable->vaEndPicture = cdv_EndPicture;
    ctx->vtable->vaExportSurfaceHandle = cdv_ExportSurfaceHandle;
    ctx->vtable->vaGetConfigAttributes = cdv_GetConfigAttributes;
    ctx->vtable->vaGetDisplayAttributes = cdv_GetDisplayAttributes;
    ctx->vtable->vaGetImage = cdv_GetImage;
    ctx->vtable->vaMFSubmit = cdv_MFSubmit;
    ctx->vtable->vaMapBuffer = cdv_MapBuffer;
    ctx->vtable->vaPutImage = cdv_PutImage;
    ctx->vtable->vaPutSurface = cdv_PutSurface;
    ctx->vtable->vaQueryConfigAttributes = cdv_QueryConfigAttributes;
    ctx->vtable->vaQueryConfigEntrypoints = cdv_QueryConfigEntrypoints;
    ctx->vtable->vaQueryConfigProfiles = cdv_QueryConfigProfiles;
    ctx->vtable->vaQueryDisplayAttributes = cdv_QueryDisplayAttributes;
    ctx->vtable->vaQueryImageFormats = cdv_QueryImageFormats;
    ctx->vtable->vaQuerySubpictureFormats = cdv_QuerySubpictureFormats;
    ctx->vtable->vaQuerySurfaceAttributes = cdv_QuerySurfaceAttributes;
    ctx->vtable->vaQuerySurfaceStatus = cdv_QuerySurfaceStatus;
    ctx->vtable->vaRenderPicture = cdv_RenderPicture;
    ctx->vtable->vaSetDisplayAttributes = cdv_SetDisplayAttributes;
    ctx->vtable->vaSetImagePalette = cdv_SetImagePalette;
    ctx->vtable->vaSetSubpictureChromakey = cdv_SetSubpictureChromakey;
    ctx->vtable->vaSetSubpictureGlobalAlpha = cdv_SetSubpictureGlobalAlpha;
    ctx->vtable->vaSetSubpictureImage = cdv_SetSubpictureImage;
    ctx->vtable->vaSyncSurface = cdv_SyncSurface;
    ctx->vtable->vaTerminate = cdv_Terminate;
    ctx->vtable->vaUnmapBuffer = cdv_UnmapBuffer;
    return VA_STATUS_SUCCESS;
}
