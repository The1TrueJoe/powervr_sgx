/* SPDX-License-Identifier: MIT */
/* Hardware check: shared blank probes and the global DMA32 allocation cap.
 * Run as root with no other decoder client. Does not submit firmware work.
 */
#define _GNU_SOURCE
#include "cedar-msvdx.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

static unsigned objects(void)
{
    unsigned value;
    FILE *file = fopen("/sys/module/gma500_gfx/parameters/msvdx_surface_objects", "r");
    assert(file && fscanf(file, "%u", &value) == 1);
    fclose(file);
    return value;
}

int main(void)
{
    int device = open("/dev/cedar-msvdx", O_RDWR | O_CLOEXEC);
    assert(device >= 0);
    struct cedar_frame_export probes[8];
    for (unsigned i = 0; i < 8; i++) {
        probes[i] = (struct cedar_frame_export){.version=1,.fd=-1,.width=16+2*i,.height=16};
        assert(!ioctl(device, CEDAR_MSVDX_EXPORT_BLANK, &probes[i]));
    }
    assert(objects() == 1); /* Eight concurrent exports share one black allocation. */
    for (unsigned i = 0; i < 8; i++) close(probes[i].fd);
    unsigned made = 0;
    for (; made < CEDAR_SURFACE_HANDLES; made++) {
        struct cedar_surface_request request = {.version=1,.operation=CEDAR_SURFACE_CREATE,
                                                 .slot=UINT32_MAX};
        if (ioctl(device, CEDAR_MSVDX_SURFACE, &request)) {
            assert(errno == ENOSPC);
            break;
        }
    }
    assert(made == 63 && objects() == 64);
    close(device);
    assert(objects() == 1); /* Only the bounded blank cache remains. */
    printf("Shared blank exports and global 64-object cap/retirement: OK\n");
    return 0;
}
