/* SPDX-License-Identifier: MIT */
/* Hardware check: undecoded VA surface exports, writable access and lifetime.
 * Run with the Cedar VA backend. Optional second argument is an SGX render node.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

int main(int argc, char **argv)
{
    int display = open(argc > 1 ? argv[1] : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
    assert(display >= 0);
    VADisplay va = vaGetDisplayDRM(display);
    int major, minor;
    assert(va && vaInitialize(va, &major, &minor) == VA_STATUS_SUCCESS);
    assert(strstr(vaQueryVendorString(va), "Cedar"));
    int render = argc > 2 ? open(argv[2], O_RDWR | O_CLOEXEC) : -1;
    assert(argc < 3 || render >= 0);
    const unsigned geometry[][2] = {{16,16}, {640,368}, {1280,720}, {1920,1080}, {1920,1088}};
    for (unsigned i=0; i<sizeof(geometry)/sizeof(geometry[0]); i++) {
        unsigned w=geometry[i][0], h=geometry[i][1];
        VASurfaceID surface;
        assert(vaCreateSurfaces(va, VA_RT_FORMAT_YUV420, w, h, &surface, 1, NULL, 0) == VA_STATUS_SUCCESS);
        VADRMPRIMESurfaceDescriptor d;
        assert(vaExportSurfaceHandle(va, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            (i % 4) | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &d) == VA_STATUS_SUCCESS);
        assert(d.fourcc == VA_FOURCC_NV12 && d.width == w && d.height == h && d.num_objects == 1 && d.num_layers == 2);
        assert(d.layers[0].drm_format == DRM_FORMAT_R8 && d.layers[1].drm_format == DRM_FORMAT_GR88);
        assert(d.layers[0].num_planes == 1 && d.layers[1].num_planes == 1);
        assert(d.layers[0].object_index[0] == 0 && d.layers[1].object_index[0] == 0);
        assert(d.objects[0].drm_format_modifier == DRM_FORMAT_MOD_LINEAR);
        int fd=d.objects[0].fd;
        assert((fcntl(fd,F_GETFL)&O_ACCMODE) == O_RDWR);
        unsigned char *pixels=mmap(NULL,d.objects[0].size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        assert(pixels != MAP_FAILED);
        struct dma_buf_sync sync={.flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW};
        assert(!ioctl(fd,DMA_BUF_IOCTL_SYNC,&sync));
        /* External descriptors retain the image after its VA surface dies. */
        assert(vaDestroySurfaces(va,&surface,1) == VA_STATUS_SUCCESS);
        for (unsigned y=0;y<h;y++) for(unsigned x=0;x<w;x++)
            assert(pixels[d.layers[0].offset[0]+y*d.layers[0].pitch[0]+x] == 16);
        for (unsigned y=0;y<h/2;y++) for(unsigned x=0;x<w;x++)
            assert(pixels[d.layers[1].offset[0]+y*d.layers[1].pitch[0]+x] == 128);
        /* A second probe must not alias the writable first surface. */
        VASurfaceID other;
        VADRMPRIMESurfaceDescriptor second;
        assert(vaCreateSurfaces(va,VA_RT_FORMAT_YUV420,w,h,&other,1,NULL,0) == VA_STATUS_SUCCESS);
        assert(vaExportSurfaceHandle(va,other,VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_SEPARATE_LAYERS,&second) == VA_STATUS_SUCCESS);
        pixels[0]=77;
        sync.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW;
        assert(!ioctl(fd,DMA_BUF_IOCTL_SYNC,&sync));
        unsigned char *neighbor=mmap(NULL,second.objects[0].size,PROT_READ,MAP_SHARED,second.objects[0].fd,0);
        assert(neighbor != MAP_FAILED);
        sync.flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ;
        assert(!ioctl(second.objects[0].fd,DMA_BUF_IOCTL_SYNC,&sync));
        assert(neighbor[0] == 16 && pixels[0] == 77);
        sync.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_READ;
        assert(!ioctl(second.objects[0].fd,DMA_BUF_IOCTL_SYNC,&sync));
        assert(!munmap(neighbor,second.objects[0].size));
        close(second.objects[0].fd);
        assert(vaDestroySurfaces(va,&other,1) == VA_STATUS_SUCCESS);
        if (render >= 0) {
            uint32_t handle, again;
            assert(!drmPrimeFDToHandle(render,fd,&handle));
            assert(!drmPrimeFDToHandle(render,fd,&again) && again == handle);
            struct drm_gem_close close_handle={.handle=handle};
            assert(!drmIoctl(render,DRM_IOCTL_GEM_CLOSE,&close_handle));
        }
        assert(!munmap(pixels,d.objects[0].size));
        close(fd);
        printf("%ux%u initialized export, writable isolated mapping, %s and retirement: OK\n",w,h,render >= 0 ? "SGX import" : "VA lifetime");
    }
    assert(vaTerminate(va) == VA_STATUS_SUCCESS);
    if (render >= 0) close(render);
    close(display);
    return 0;
}
