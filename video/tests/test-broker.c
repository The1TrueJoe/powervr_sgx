/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "cedar-broker.h"
#include "../src/decoder-transport.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static void put(unsigned char *p, unsigned offset, uint32_t value) { memcpy(p+offset,&value,4); }
static void picture(unsigned char *p, size_t bytes, unsigned type)
{
    memset(p,0,bytes); memcpy(p,"CDV2",4);
    put(p,4,12); put(p,8,1280); put(p,12,720); put(p,16,1); put(p,20,1);
    put(p,28,(unsigned)bytes-32); put(p,44,type); put(p,52,UINT32_MAX); put(p,56,UINT32_MAX);
    struct cedar_h264_config c={.size=64,.version=1,.width=1280,.height=720,.profile_idc=100};
    memcpy(p+96,&c,sizeof(c));
    struct cedar_h264_dpb d={.size=128,.version=1,.max_refs=4};
    for (unsigned i=0;i<4;i++) d.l0_slots[i]=d.l1_slots[i]=UINT32_MAX;
    if (type!=5) { d.l0_count=1; d.l0_slots[0]=1; }
    if (type==2) { d.l1_count=1; d.l1_slots[0]=2; }
    memcpy(p+160,&d,sizeof(d));
    for (unsigned i=0;i<4;i++) { put(p,288+i*64,64); put(p,292+i*64,1); }
    put(p,544,(unsigned)bytes-548); p[548]=type==5 ? 0x65 : type==1 ? 0x41 : 0x01;
}

static unsigned fd_count(void)
{
    unsigned count=0; DIR *d=opendir("/proc/self/fd"); assert(d);
    while (readdir(d)) ++count;
    closedir(d); return count;
}

static void wide_intra(unsigned char *p,size_t bytes)
{
    picture(p,bytes,5);put(p,4,14);memset(p+160,0,2752-160);
    struct cedar_h264_dpb32 d={.size=sizeof(d),.version=1,.max_refs=1,.reference=1};
    for(unsigned i=0;i<32;i++)d.l0_slots[i]=d.l1_slots[i]=UINT32_MAX;
    memcpy(p+160,&d,sizeof(d));
    for(unsigned i=0;i<32;i++){put(p,704+i*64,64);put(p,708+i*64,1);}
    put(p,2752,(unsigned)bytes-2756);p[2756]=0x65;
}

static void multislice_packet_tests(void)
{
    enum { CHILD=2759, BYTES=32+2*(8+CHILD), SECOND=32+8+CHILD };
    unsigned char packet[BYTES];memset(packet,0,sizeof(packet));
    memcpy(packet,"CDV2",4);put(packet,4,10);put(packet,8,1280);put(packet,12,720);
    put(packet,16,2);put(packet,20,1);put(packet,28,BYTES-32);
    put(packet,36,CHILD);wide_intra(packet+40,CHILD);
    put(packet,SECOND,80);put(packet,SECOND+4,CHILD);wide_intra(packet+SECOND+8,CHILD);
    unsigned char *child=packet+SECOND+8;
    assert(cedar_broker_packet_valid(packet,BYTES));
    put(child,36,31);put(child,128,1); /* Slice-local QP/deblock changes. */
    assert(cedar_broker_packet_valid(packet,BYTES));
    for(unsigned i=0;i<BYTES;i++) assert(!cedar_broker_packet_valid(packet,i));
    put(packet,SECOND,0);assert(!cedar_broker_packet_valid(packet,BYTES));put(packet,SECOND,80);
    put(packet,SECOND,3600);assert(!cedar_broker_packet_valid(packet,BYTES));put(packet,SECOND,80);
    put(child,4,10);assert(!cedar_broker_packet_valid(packet,BYTES));put(child,4,14);
    put(child,48,1);assert(!cedar_broker_packet_valid(packet,BYTES));put(child,48,0);
    put(child,64,2);assert(!cedar_broker_packet_valid(packet,BYTES));put(child,64,0);
    put(child,168,2);assert(!cedar_broker_packet_valid(packet,BYTES));put(child,168,1);
    put(child,120,1);assert(!cedar_broker_packet_valid(packet,BYTES));put(child,120,0);
    put(packet,16,3);assert(!cedar_broker_packet_valid(packet,BYTES));put(packet,16,2);
    put(packet,24,1);assert(!cedar_broker_packet_valid(packet,BYTES));put(packet,24,0);
    assert(cedar_broker_packet_valid(packet,BYTES));
    puts("Multislice framing, per-picture consistency, truncation and nested-packet rejection passed (offline).");
}

static void blank_export_tests(void)
{
    struct cedar_frame_export good={.version=1,.fd=-1,.width=1920,.height=1080};
    assert(cedar_frame_blank_valid(&good));
    struct cedar_frame_export bad=good;
    bad.width=1922;assert(!cedar_frame_blank_valid(&bad));
    bad=good;bad.height=1089;assert(!cedar_frame_blank_valid(&bad));
    bad=good;bad.width=15;assert(!cedar_frame_blank_valid(&bad));
    bad=good;bad.width=17;assert(!cedar_frame_blank_valid(&bad));
    bad=good;bad.reserved2=1;assert(!cedar_frame_blank_valid(&bad));
    bad=good;bad.id=1;assert(!cedar_frame_blank_valid(&bad));
    bad=good;bad.fd=0;assert(!cedar_frame_blank_valid(&bad));
    unsigned initial=fd_count();
    for (unsigned writable=0;writable<2;writable++) {
        int pair[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair));
        pid_t child=fork();assert(child>=0);
        if (!child) {
            close(pair[0]);struct cedar_broker_header h;struct cedar_frame_export r;int right=-1;
            assert(cedar_broker_recv(pair[1],&h,&r,sizeof(r),&right)==sizeof(r));
            assert(h.operation==CEDAR_BROKER_EXPORT_BLANK && right<0 && cedar_frame_blank_valid(&r));
            int fd=open("/dev/null",(writable ? O_RDWR : O_RDONLY)|O_CLOEXEC);assert(fd>=0);
            r.id=1;
            assert(!cedar_broker_send(pair[1],h.operation,0,&r,sizeof(r),fd));
            close(fd);close(pair[1]);_exit(0);
        }
        close(pair[1]);struct cedar_frame_export r=good;
        if (writable) assert(cedar_decoder_export_blank(pair[0],true,&r)==-1 && errno==EPROTO);
        else { assert(!cedar_decoder_export_blank(pair[0],true,&r));close(r.fd); }
        close(pair[0]);int status;assert(waitpid(child,&status,0)==child && status==0);
    }
    assert(fd_count()==initial);
    puts("Blank export geometry, request bounds, read-only FD transfer and retirement passed (offline).");
}

static void sealed_packet_tests(void)
{
    unsigned initial=fd_count();
    int sealed=memfd_create("cedar-test",MFD_CLOEXEC|MFD_ALLOW_SEALING);assert(sealed>=0);
    unsigned char packet[2759];wide_intra(packet,sizeof(packet));
    assert(write(sealed,packet,sizeof(packet))==sizeof(packet));
    const void *mapped=NULL;size_t bytes=0;
    assert(cedar_broker_map_sealed_packet(sealed,&mapped,&bytes)==-1 && errno==EPERM);
    assert(!fcntl(sealed,F_ADD_SEALS,F_SEAL_WRITE));
    assert(cedar_broker_map_sealed_packet(sealed,&mapped,&bytes)==-1);
    assert(!fcntl(sealed,F_ADD_SEALS,F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL));
    assert(!cedar_broker_map_sealed_packet(sealed,&mapped,&bytes));
    assert(bytes==sizeof(packet) && cedar_broker_packet_valid(mapped,bytes));
    assert(pwrite(sealed,"X",1,0)==-1 && errno==EPERM);
    assert(ftruncate(sealed,1)==-1 && errno==EPERM);
    munmap((void *)mapped,bytes);close(sealed);
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair));
    size_t large_bytes=CEDAR_BROKER_MAX_PACKET;
    unsigned char *large=malloc(large_bytes);assert(large);wide_intra(large,large_bytes);
    assert(cedar_broker_packet_valid(large,large_bytes));
    pid_t child=fork();assert(child>=0);
    if(!child) {
        close(pair[0]);struct cedar_broker_header h;int right=-1;
        assert(cedar_broker_recv(pair[1],&h,NULL,0,&right)==0 && h.operation==CEDAR_BROKER_SUBMIT_FD && right>=0);
        assert(!cedar_broker_map_sealed_packet(right,&mapped,&bytes));
        assert(bytes==large_bytes && !memcmp(mapped,large,bytes) && cedar_broker_packet_valid(mapped,bytes));
        munmap((void *)mapped,bytes);close(right);
        assert(!cedar_broker_send(pair[1],h.operation,0,NULL,0,-1));close(pair[1]);_exit(0);
    }
    close(pair[1]);
    assert(cedar_decoder_submit(pair[0],true,large,large_bytes)==(ssize_t)large_bytes);
    close(pair[0]);free(large);int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(fd_count()==initial);
    puts("Sealed large-packet transfer, immutability and fd retirement passed (offline).");
}

static void wire_tests(void)
{
    int pair[2]; assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair));
    char path[]="/tmp/cedar-broker-test-XXXXXX";
    int temp=mkstemp(path); assert(temp>=0); assert(write(temp,"NV12",4)==4); close(temp);
    int source=open(path,O_RDONLY|O_CLOEXEC); assert(source>=0); unlink(path);
    unsigned initial=fd_count();
    for (unsigned i=0;i<100;i++) {
        assert(!cedar_broker_send(pair[0],CEDAR_BROKER_EXPORT,0,"abcd",4,source));
        struct cedar_broker_header h; char buffer[8]; int right=-1;
        assert(cedar_broker_recv(pair[1],&h,buffer,sizeof(buffer),&right)==4);
        assert(right>=0 && (fcntl(right,F_GETFD)&FD_CLOEXEC));
        assert((fcntl(right,F_GETFL)&O_ACCMODE)==O_RDONLY);
        assert(!memcmp(buffer,"abcd",4)); close(right);
        assert(!cedar_broker_send(pair[0],CEDAR_BROKER_EXPORT,0,"abcdef",6,source));
        assert(cedar_broker_recv(pair[1],&h,buffer,2,&right)==-1 && errno==EPROTO && right==-1);
        struct cedar_broker_header bad={CEDAR_BROKER_MAGIC,1,1,0,0,1};
        struct iovec raw_iov={&bad,sizeof(bad)};
        struct msghdr raw_msg={.msg_iov=&raw_iov,.msg_iovlen=1};
        if(sendmsg(pair[0],&raw_msg,0)!=(ssize_t)sizeof(bad)) {perror("malformed header send");abort();}
        assert(cedar_broker_recv(pair[1],&h,buffer,sizeof(buffer),&right)==-1 && errno==EPROTO);
    }
    /* Malicious input rights must all close, including a duplicated source. */
    struct cedar_broker_header good={CEDAR_BROKER_MAGIC,1,1,0,0,0};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(2*sizeof(int))]; } control;
    memset(&control,0,sizeof(control));
    struct iovec iov={&good,sizeof(good)};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg); c->cmsg_level=SOL_SOCKET; c->cmsg_type=SCM_RIGHTS;
    c->cmsg_len=CMSG_LEN(2*sizeof(int)); int rights[]={source,source}; memcpy(CMSG_DATA(c),rights,sizeof(rights));
    assert(sendmsg(pair[0],&msg,0)==sizeof(good));
    char buffer[8]; int right; struct cedar_broker_header h;
    assert(cedar_broker_recv(pair[1],&h,buffer,sizeof(buffer),&right)==-1 && errno==EPROTO && right==-1);
    assert(fd_count()==initial);
    close(source);close(pair[0]);close(pair[1]);
}

int main(void)
{
    unsigned char packet[CEDAR_BROKER_MAX_PACKET];
    for (unsigned i=0;i<3;i++) {
        const unsigned type[]={5,1,2}; picture(packet,600,type[i]); assert(cedar_broker_packet_valid(packet,600));
    }
    picture(packet,548+65535,5); assert(cedar_broker_packet_valid(packet,548+65535));
    assert(!cedar_broker_packet_valid(packet,548+65534));
    picture(packet,sizeof(packet),5); assert(!cedar_broker_packet_valid(packet,sizeof(packet)));
    assert(!cedar_broker_packet_valid(NULL,0));
    static const unsigned offsets[]={4,8,12,16,20,24,28,36,40,44,48,52,56,60,64,68,72,76,80,84,88,92,
        96,100,104,108,112,116,140,144,148,152,156,160,164,168,172,176,244,284,
        288,292,328,348,352,412,416,476,480,540,544};
    for (unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
        picture(packet,600,5); put(packet,offsets[i],offsets[i]==52 || offsets[i]==56 ? 5U : UINT32_MAX); assert(!cedar_broker_packet_valid(packet,600));
    }
    picture(packet,600,5); packet[548]=0xe5; assert(!cedar_broker_packet_valid(packet,600));
    picture(packet,600,2); put(packet,4,13); put(packet,164,2);
    put(packet,244,6); put(packet,248,1); packet[548]=0x21;
    assert(cedar_broker_packet_valid(packet,600));
    put(packet,244,2); assert(!cedar_broker_packet_valid(packet,600));
    put(packet,244,7); assert(!cedar_broker_packet_valid(packet,600));
    put(packet,244,6); put(packet,248,2); assert(!cedar_broker_packet_valid(packet,600));
    picture(packet,2759,1);put(packet,4,14);put(packet,2752,3);packet[2756]=0x41;
    memset(packet+160,0,2752-160);
    struct cedar_h264_dpb32 full={.size=sizeof(full),.version=1,.max_refs=1,.l0_count=32,.retained_mask=2,.reference=1};
    for(unsigned i=0;i<32;i++){full.l0_slots[i]=1;full.l1_slots[i]=UINT32_MAX;}
    memcpy(packet+160,&full,sizeof(full));
    for(unsigned i=0;i<32;i++){put(packet,704+i*64,64);put(packet,708+i*64,1);}
    assert(cedar_broker_packet_valid(packet,2759));
    put(packet,172,33);assert(!cedar_broker_packet_valid(packet,2759));put(packet,172,32);
    put(packet,180,3);assert(!cedar_broker_packet_valid(packet,2759));put(packet,180,2);
    put(packet,188,1);assert(!cedar_broker_packet_valid(packet,2759));put(packet,188,0);
    put(packet,4,13);assert(!cedar_broker_packet_valid(packet,2759));
    put(packet,4,14);put(packet,8,640);put(packet,12,368);put(packet,104,640);put(packet,108,368);
    assert(cedar_broker_packet_valid(packet,2759));
    put(packet,12,360);put(packet,108,360);assert(!cedar_broker_packet_valid(packet,2759));
    put(packet,8,1920);put(packet,12,1088);put(packet,104,1920);put(packet,108,1088);
    assert(cedar_broker_packet_valid(packet,2759));
    put(packet,8,1936);put(packet,104,1936);assert(!cedar_broker_packet_valid(packet,2759));
    struct cedar_surface_request r={.version=1,.operation=CEDAR_SURFACE_CREATE,.slot=UINT32_MAX};
    assert(cedar_broker_surface_valid(&r));r.handle=1;assert(!cedar_broker_surface_valid(&r));
    r.operation=CEDAR_SURFACE_BIND;r.id=4;r.slot=4;assert(cedar_broker_surface_valid(&r));
    r.slot=5;assert(!cedar_broker_surface_valid(&r));r.slot=0;r.image_bytes=1;assert(!cedar_broker_surface_valid(&r));
    wire_tests();
    blank_export_tests();
    sealed_packet_tests();
    multislice_packet_tests();
    assert(!setenv("CEDAR_DECODER_SOCKET","/nonexistent/cedar-test",1));
    bool broker=false;assert(cedar_decoder_open(&broker)==-1 && broker && (errno==ENOENT || errno==EACCES || errno==EPERM));
    unsetenv("CEDAR_DECODER_SOCKET");
    puts("Broker packet bounds, surface controls, fd transfer/leak rejection and explicit no-fallback passed.");
}
