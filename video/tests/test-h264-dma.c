/* SPDX-License-Identifier: MIT */
#include "cedar-msvdx-dma.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const unsigned sizes[]={1,65534,65535,65536,131070,131071,CEDAR_H264_MAX_SLICE_BYTES};
    const unsigned command_base=0x01000000, data_base=0x04000000;
    for(unsigned n=0;n<sizeof(sizes)/sizeof(sizes[0]);n++) {
        __u32 command[1024], descriptor[8];memset(command,0xa5,sizeof(command));
        unsigned index=0;
        while(cedar_h264_dma_descriptor(descriptor,command_base,data_base,sizes[n],index)) {
            unsigned offset=cedar_h264_dma_offset(index++);
            assert(offset+sizeof(descriptor)<=sizeof(command));
            memcpy((char *)command+offset,descriptor,sizeof(descriptor));
        }
        /* Interpret the produced hardware chain independently, checking exact
         * coverage, finite termination, byte counts and link-address units. */
        unsigned offset=CEDAR_H264_DMA_FIRST_OFFSET, total=0, records=0;
        for(;;) {
            const __u32 *d=command+offset/4;unsigned bytes=d[1]&65535;
            assert(bytes && d[0]==0x20000000 && d[2]==0x0820 && d[3]==(4U<<26));
            assert(d[6]==data_base+total && !d[4] && !d[5]);
            total+=bytes;records++;
            assert(total<=sizes[n] && records<=CEDAR_H264_DMA_DESCRIPTORS);
            if(d[1]&0x80000000U) {assert(!d[7]);break;}
            assert(d[7]);offset=(d[7]<<4)-command_base;
            assert(offset>=0xe00 && offset+32<=sizeof(command) && !(offset&15));
        }
        assert(total==sizes[n] && records==index);
        assert(command[0xc80/4]==0xa5a5a5a5 && command[0xcfc/4]==0xa5a5a5a5);
    }
    __u32 d[8];
    assert(!cedar_h264_dma_descriptor(d,command_base,data_base,0,0));
    assert(!cedar_h264_dma_descriptor(d,command_base,data_base,CEDAR_H264_MAX_SLICE_BYTES+1,0));
    assert(!cedar_h264_dma_descriptor(d,command_base,data_base,CEDAR_H264_MAX_SLICE_BYTES,CEDAR_H264_DMA_DESCRIPTORS));
    assert(!cedar_h264_dma_descriptor(d,command_base+1,data_base,1,0));
    assert(!cedar_h264_dma_descriptor(d,command_base,0xfffffff0U,32,0));
    puts("DE2 DMA chains cover 64 KiB boundaries through 1 MiB within owned command-page bounds.");
    return 0;
}
