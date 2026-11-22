/* SPDX-License-Identifier: MIT */
/* Decode with VA-API only and compare exported NV12 GPU samples with their
 * CPU mappings using test-dmabuf-planes. This validates import/sampling, not
 * decode correctness: run the software-reference frame checker as well.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <va/va_drmcommon.h>

static enum AVPixelFormat hardware_format(AVCodecContext *ctx, const enum AVPixelFormat *formats)
{
    (void)ctx;
    for (; *formats != AV_PIX_FMT_NONE; formats++)
        if (*formats == AV_PIX_FMT_VAAPI) return *formats;
    return AV_PIX_FMT_NONE;
}

struct retained {
    int fd;
    size_t bytes;
    unsigned char *snapshot;
};

static void retain_image(struct retained *saved, int fd, size_t bytes)
{
    void *mapping=mmap(NULL,bytes,PROT_READ,MAP_SHARED,fd,0);
    assert(mapping != MAP_FAILED);
    struct dma_buf_sync sync={.flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ};
    assert(!ioctl(fd,DMA_BUF_IOCTL_SYNC,&sync));
    saved->snapshot=malloc(bytes);assert(saved->snapshot);
    memcpy(saved->snapshot,mapping,bytes);saved->bytes=bytes;
    saved->fd=fcntl(fd,F_DUPFD_CLOEXEC,0);assert(saved->fd>=0);
    sync.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_READ;
    assert(!ioctl(fd,DMA_BUF_IOCTL_SYNC,&sync));
    assert(!munmap(mapping,bytes));
}

static void check_retained(struct retained *saved)
{
    void *mapping=mmap(NULL,saved->bytes,PROT_READ,MAP_SHARED,saved->fd,0);
    assert(mapping != MAP_FAILED);
    struct dma_buf_sync sync={.flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ};
    assert(!ioctl(saved->fd,DMA_BUF_IOCTL_SYNC,&sync));
    assert(!memcmp(mapping,saved->snapshot,saved->bytes));
    sync.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_READ;
    assert(!ioctl(saved->fd,DMA_BUF_IOCTL_SYNC,&sync));
    assert(!munmap(mapping,saved->bytes));
    close(saved->fd);free(saved->snapshot);
}

static void sample_frame(const AVFrame *frame, const char *probe, struct retained *saved)
{
    assert(frame->format == AV_PIX_FMT_VAAPI && frame->hw_frames_ctx);
    AVHWFramesContext *frames=(AVHWFramesContext *)frame->hw_frames_ctx->data;
    AVVAAPIDeviceContext *device=frames->device_ctx->hwctx;
    VASurfaceID surface=(VASurfaceID)(uintptr_t)frame->data[3];
    VADRMPRIMESurfaceDescriptor d;
    assert(vaSyncSurface(device->display,surface) == VA_STATUS_SUCCESS);
    assert(vaExportSurfaceHandle(device->display,surface,VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY|VA_EXPORT_SURFACE_SEPARATE_LAYERS,&d) == VA_STATUS_SUCCESS);
    assert(d.fourcc == VA_FOURCC_NV12 && d.num_objects == 1 && d.num_layers == 2);
    assert(d.layers[0].offset[0] == 0 && d.layers[0].pitch[0] == d.layers[1].pitch[0]);
    int fd=d.objects[0].fd;
    assert(fd >= 0 && (fcntl(fd,F_GETFL)&O_ACCMODE) == O_RDONLY);
    char args[5][32];
    unsigned values[]={fd,d.width,d.height,d.layers[0].pitch[0],d.layers[1].offset[0]};
    for (unsigned i=0;i<5;i++) snprintf(args[i],sizeof(args[i]),"%u",values[i]);
    pid_t child=fork();assert(child>=0);
    if (!child) {
        if (fcntl(fd,F_SETFD,0)) _exit(126);
        execl(probe,probe,args[0],args[1],args[2],args[3],args[4],(char *)NULL);
        _exit(127);
    }
    int status;
    assert(waitpid(child,&status,0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (saved) retain_image(saved,fd,d.objects[0].size);
    close(fd);
}

int main(int argc, char **argv)
{
    if (argc != 4 && (argc != 5 || strcmp(argv[4],"--retain"))) {
        fprintf(stderr,"usage: %s video test-dmabuf-planes frames(1..128) [--retain]\n",argv[0]);return 2;
    }
    char *end;
    long wanted=strtol(argv[3],&end,10);
    assert(*argv[3] && !*end && wanted>0 && wanted<=128);
    AVFormatContext *input=NULL;
    assert(avformat_open_input(&input,argv[1],NULL,NULL) >= 0);
    assert(avformat_find_stream_info(input,NULL) >= 0);
    const AVCodec *decoder=NULL;
    int stream=av_find_best_stream(input,AVMEDIA_TYPE_VIDEO,-1,-1,&decoder,0);
    assert(stream>=0 && decoder && decoder->id == AV_CODEC_ID_H264);
    AVCodecContext *ctx=avcodec_alloc_context3(decoder);assert(ctx);
    assert(avcodec_parameters_to_context(ctx,input->streams[stream]->codecpar) >= 0);
    assert(av_hwdevice_ctx_create(&ctx->hw_device_ctx,AV_HWDEVICE_TYPE_VAAPI,"/dev/dri/card0",NULL,0) >= 0);
    ctx->get_format=hardware_format;
    ctx->thread_count=1;
    assert(avcodec_open2(ctx,decoder,NULL) >= 0);
    AVPacket *packet=av_packet_alloc();AVFrame *frame=av_frame_alloc();assert(packet && frame);
    unsigned checked=0, retained_count=0;
    struct retained retained[8];
    int eof=0;
    while (checked<(unsigned)wanted) {
        int ret=avcodec_receive_frame(ctx,frame);
        if (ret>=0) {
            struct retained *saved=argc == 5 && retained_count<8 ? &retained[retained_count++] : NULL;
            sample_frame(frame,argv[2],saved);
            av_frame_unref(frame);checked++;
            continue;
        }
        assert(ret == AVERROR(EAGAIN) && !eof);
        do {
            ret=av_read_frame(input,packet);
            if (ret<0) break;
            if (packet->stream_index == stream) break;
            av_packet_unref(packet);
        } while (1);
        if (ret<0) {
            assert(ret == AVERROR_EOF);eof=1;
            assert(avcodec_send_packet(ctx,NULL) >= 0);
        } else {
            assert(avcodec_send_packet(ctx,packet) >= 0);
            av_packet_unref(packet);
        }
    }
    av_frame_free(&frame);av_packet_free(&packet);avcodec_free_context(&ctx);avformat_close_input(&input);
    for (unsigned i=0;i<retained_count;i++) check_retained(&retained[i]);
    if (retained_count) printf("%u retained exports unchanged after later decoding and decoder closure\n",retained_count);
    printf("%u VA-decoded frames exported and sampled exactly by SGX\n",checked);
    return 0;
}
