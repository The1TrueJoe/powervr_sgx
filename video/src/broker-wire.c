/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "cedar-broker.h"
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

int cedar_broker_send(int socket_fd, unsigned operation, int status,
                      const void *data, size_t bytes, int right)
{
    if (bytes > CEDAR_BROKER_MAX_PACKET) { errno = EMSGSIZE; return -1; }
    struct cedar_broker_header h = { CEDAR_BROKER_MAGIC, CEDAR_BROKER_VERSION,
                                    operation, (uint32_t)bytes, status, 0 };
    struct iovec iov[] = {{&h, sizeof(h)}, {(void *)data, bytes}};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof(control));
    struct msghdr msg = { .msg_iov = iov, .msg_iovlen = bytes ? 2 : 1 };
    if (right >= 0) {
        msg.msg_control = control.bytes; msg.msg_controllen = sizeof(control.bytes);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(c), &right, sizeof(right));
    }
    ssize_t n;
    do n = sendmsg(socket_fd, &msg, MSG_NOSIGNAL); while (n < 0 && errno == EINTR);
    if (n < 0) return -1;
    if ((size_t)n != sizeof(h)+bytes) { errno = EIO; return -1; }
    return 0;
}

ssize_t cedar_broker_recv(int socket_fd, struct cedar_broker_header *h,
                          void *data, size_t capacity, int *right)
{
    /* Space for many unwanted rights, so they can be closed rather than leaked.
     * MSG_CTRUNC is rejected; Linux closes rights truncated out of the buffer. */
    union { struct cmsghdr align; char bytes[CMSG_SPACE(16*sizeof(int))]; } control;
    struct iovec iov[] = {{h, sizeof(*h)}, {data, capacity}};
    struct msghdr msg = { .msg_iov = iov, .msg_iovlen = 2,
                         .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
    *right = -1;
    ssize_t n;
    do n = recvmsg(socket_fd, &msg, MSG_CMSG_CLOEXEC); while (n < 0 && errno == EINTR);
    if (n <= 0) { if (!n) errno = ECONNRESET; return -1; }
    bool bad = !!(msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
    unsigned rights = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg,c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
            c->cmsg_len < CMSG_LEN(0)) { bad = true; continue; }
        size_t bytes = c->cmsg_len-CMSG_LEN(0);
        if (bytes % sizeof(int)) bad = true;
        for (size_t i=0; i < bytes/sizeof(int); i++) {
            int fd; memcpy(&fd, (char *)CMSG_DATA(c)+i*sizeof(int), sizeof(fd));
            if (!rights++) *right = fd; else { close(fd); bad = true; }
        }
    }
    if ((size_t)n < sizeof(*h) || h->magic != CEDAR_BROKER_MAGIC ||
        h->version != CEDAR_BROKER_VERSION || h->reserved ||
        h->bytes > capacity || (size_t)n != sizeof(*h)+h->bytes ||
        h->status < 0 || h->status > 4095) bad = true;
    if (bad) {
        if (*right >= 0) close(*right);
        *right = -1; errno = EPROTO; return -1;
    }
    return h->bytes;
}

static uint32_t word(const unsigned char *p, size_t offset)
{
    uint32_t value; memcpy(&value, p+offset, sizeof(value)); return value;
}

int cedar_broker_surface_valid(const struct cedar_surface_request *r)
{
    return r->version == 1 && r->operation >= CEDAR_SURFACE_CREATE &&
        r->operation <= CEDAR_SURFACE_INFO && !r->image_bytes && !r->auxiliary_bytes &&
        !r->reserved[0] && !r->reserved[1] &&
        (r->operation == CEDAR_SURFACE_BIND ? r->slot < CEDAR_H264_SURFACES : r->slot == UINT32_MAX) &&
        ((r->operation == CEDAR_SURFACE_CREATE || r->operation == CEDAR_SURFACE_CANCEL) ?
          (!r->handle && !r->id) : (r->handle && r->id));
}

static int single_packet_valid(const void *packet, size_t bytes)
{
    const unsigned char *p = packet;
    if (bytes < 549 || bytes > CEDAR_BROKER_MAX_PACKET) return 0;
    unsigned version = word(p,4), length = version == 14 ? CEDAR_H264_FULL_LENGTH_OFFSET : 544;
    unsigned weight_start = version == 14 ? 704 : 288, weight_count = version == 14 ? 32 : 4;
    if (bytes <= length+4 || bytes-length-4 > (version == 14 ? CEDAR_H264_MAX_SLICE_BYTES : 65535) || memcmp(p,"CDV2",4) ||
        (version != 12 && version != 13 && version != 14) || (version == 14 ? !cedar_h264_picture_geometry_valid(word(p,8), word(p,12)) : (word(p,8) != 1280 || word(p,12) != 720)) ||
        word(p,16) != 1 || word(p,20) != 1 || word(p,24) ||
        word(p,28) != bytes-32 || word(p,length) != bytes-length-4) return 0;
    unsigned type = word(p,44), slot = word(p,48);
    if (word(p,32) >= (bytes-length-4)*8 || word(p,36) > 51 || word(p,40) ||
        (type != 1 && type != 2 && type != 5 && !(version == 14 && type == 6)) || slot >= CEDAR_H264_SURFACES ||
        (word(p,52) != UINT32_MAX && word(p,52) >= CEDAR_H264_SURFACES) ||
        (word(p,56) != UINT32_MAX && word(p,56) >= CEDAR_H264_SURFACES) ||
        word(p,60) > 2 || word(p,64) > (version == 14 ? 0x7ffffffeU : 65534) || (word(p,64)&1) ||
        word(p,68) > (version == 14 ? 0x7ffffffeU : 65534) || (word(p,68)&1) ||
        word(p,72) > (version == 14 ? 0x7ffffffeU : 65534) || (word(p,72)&1) ||
        word(p,76) > 1 || word(p,80) || word(p,84) || word(p,88) || word(p,92)) return 0;
    struct cedar_h264_config config;
    struct cedar_h264_dpb dpb;
    struct cedar_h264_dpb32 full;
    memcpy(&config,p+96,sizeof(config)); memcpy(&dpb,p+160,sizeof(dpb));
    if (version == 14) memcpy(&full,p+160,sizeof(full));
    if (!cedar_h264_config_valid(&config,CEDAR_H264_MAX_WIDTH,CEDAR_H264_MAX_HEIGHT) || config.width != word(p,8) || config.height != word(p,12) ||
        (version == 14 ? !cedar_h264_dpb32_valid(&full,type) : version == 13 ? !cedar_h264_explicit_dpb_valid(&dpb,type) :
         !cedar_h264_dpb_valid(&dpb,type))) return 0;
    unsigned retained = version == 14 ? full.retained_mask : version == 13 ? dpb.reserved[0] : 0;
    unsigned l0_count = version == 14 ? full.l0_count : dpb.l0_count;
    unsigned l1_count = version == 14 ? full.l1_count : dpb.l1_count;
    const uint32_t *l0 = version == 14 ? full.l0_slots : dpb.l0_slots;
    const uint32_t *l1 = version == 14 ? full.l1_slots : dpb.l1_slots;
    if (retained & (1U << slot)) return 0;
    for (unsigned i=0; i < l0_count; i++) if (l0[i] == slot) return 0;
    for (unsigned i=0; i < l1_count; i++) if (l1[i] == slot) return 0;
    for (unsigned i=0; i < weight_count; i++) {
        struct cedar_h264_weights weights;
        memcpy(&weights,p+weight_start+i*64,sizeof(weights));
        if (!cedar_h264_weights_valid(&weights, type == 1 &&
            (config.config_flags & CEDAR_CONFIG_WEIGHT_P) && i < l0_count)) return 0;
    }
    unsigned nal = p[length+4];
    if ((nal&0x80) || (nal&31) != (type == 5 ? 5U : 1U) ||
        (version == 14 && !!(nal&0x60) != full.reference)) return 0;
    return 1; /* Kernel retains full NAL/state/reference validation. */
}

int cedar_broker_packet_valid(const void *packet, size_t bytes)
{
    const unsigned char *p = packet;
    if (bytes < 32 || bytes > CEDAR_BROKER_MAX_PACKET) return 0;
    if (word(p,4) != 10) return single_packet_valid(packet,bytes);
    unsigned slices=word(p,16), width=word(p,8), height=word(p,12);
    if (memcmp(p,"CDV2",4) || !cedar_h264_picture_geometry_valid(width,height) ||
        slices < 2 || slices > CEDAR_H264_MAX_SLICES || word(p,20) != 1 ||
        word(p,24) || word(p,28) != bytes-32) return 0;
    size_t position=32;
    unsigned previous=0;
    const unsigned char *first=NULL;
    for (unsigned i=0;i<slices;i++) {
        if (bytes-position < 8) return 0;
        unsigned mb=word(p,position), child_bytes=word(p,position+4);
        position+=8;
        if (child_bytes > bytes-position || child_bytes < 32 ||
            (!i ? mb != 0 : mb <= previous) || mb >= width*height/256) return 0;
        const unsigned char *child=p+position;
        if (word(child,4) != 14 || word(child,8) != width || word(child,12) != height ||
            !single_packet_valid(child,child_bytes)) return 0;
        if (!i) first=child;
        else {
            struct cedar_h264_config a,b;
            memcpy(&a,first+96,sizeof(a));memcpy(&b,child+96,sizeof(b));
            /* Slice QP, deblocking, lists and weights may differ. Picture,
             * output, retained DPB and current POC must stay identical. */
            if (!cedar_h264_sequence_equal(&a,&b) || word(child,44) != word(first,44) ||
                word(child,48) != word(first,48) || word(child,64) != word(first,64) ||
                memcmp(child+160,first+160,12) || memcmp(child+180,first+180,12)) return 0;
        }
        position+=child_bytes;previous=mb;
    }
    return position==bytes;
}

int cedar_broker_map_sealed_packet(int fd, const void **data, size_t *bytes)
{
    const int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    int seals = fcntl(fd, F_GET_SEALS);
    struct stat st;
    struct statfs fs;
    /* Check immutability before reading size: a later seal check would race
     * a shrink between fstat and mmap, exposing the broker to SIGBUS. */
    if (seals < 0 || (seals & required) != required) { errno=EPERM; return -1; }
    if (fstat(fd,&st) || fstatfs(fd,&fs)) return -1;
    if (!S_ISREG(st.st_mode) || fs.f_type != TMPFS_MAGIC || st.st_size < 549 ||
        (uint64_t)st.st_size > CEDAR_BROKER_MAX_PACKET) { errno=EINVAL; return -1; }
    void *mapping = mmap(NULL,(size_t)st.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    if (mapping == MAP_FAILED) return -1;
    *data=mapping; *bytes=(size_t)st.st_size;
    return 0;
}
