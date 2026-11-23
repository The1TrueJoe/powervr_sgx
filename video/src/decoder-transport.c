/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "decoder-transport.h"
#include "cedar-broker.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int exchange_right(int fd, unsigned op, const void *input, size_t input_bytes,
                     void *output, size_t output_bytes, int *right, int input_right)
{
    if (cedar_broker_send(fd,op,0,input,input_bytes,input_right)) return -1;
    struct cedar_broker_header h;
    int received = -1;
    ssize_t n = cedar_broker_recv(fd,&h,output,output_bytes,&received);
    if (n < 0) return -1;
    bool bad = h.operation != op || (h.status ? (n || received >= 0) :
                   ((size_t)n != output_bytes || (right ? received < 0 : received >= 0)));
    if (bad || h.status) {
        if (received >= 0) close(received);
        errno = bad ? EPROTO : h.status; return -1;
    }
    if (right) *right = received;
    return 0;
}

static int exchange(int fd, unsigned op, const void *input, size_t input_bytes,
                     void *output, size_t output_bytes, int *right)
{
    return exchange_right(fd,op,input,input_bytes,output,output_bytes,right,-1);
}

int cedar_decoder_open(bool *broker)
{
    const char *path = getenv("CEDAR_DECODER_SOCKET");
    *broker = path && *path;
    if (!*broker) return open("/dev/cedar-msvdx",O_RDWR|O_CLOEXEC);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(address.sun_path)) { errno = ENAMETOOLONG; return -1; }
    memcpy(address.sun_path,path,strlen(path)+1);
    int fd = socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
    if (fd < 0) { perror("cedar-transport: socket creation"); return -1; }
    const char *step="initial socket timeouts";
    struct timeval timeout = { .tv_sec = 20 };
    if (setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) ||
        setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))) goto fail;
    step="brokered connect";
    if (connect(fd,(struct sockaddr *)&address,sizeof(address))) goto fail;
    /* A sandbox connect broker may replace the fd. Restore timeouts on the
     * connected socket too; fd substitution preserves neither socket option. */
    step="connected socket timeouts";
    if (setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) ||
        setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))) goto fail;
    struct ucred peer;
    socklen_t size = sizeof(peer);
    step="peer credentials";
    if (getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&size) || size != sizeof(peer)) goto fail;
    step="root peer identity";
    if (peer.uid != 0) {
        fprintf(stderr,"cedar-transport: peer uid=%u client uid=%u euid=%u\n",
                (unsigned)peer.uid,(unsigned)getuid(),(unsigned)geteuid());
        errno = EACCES; goto fail;
    }
    /* Read the server greeting before sending anything. A denied peer therefore
     * receives a deterministic errno rather than reset from unread requests. */
    struct cedar_msvdx_caps caps;
    struct cedar_broker_header h;
    int right=-1;
    step="server greeting";
    ssize_t n=cedar_broker_recv(fd,&h,&caps,sizeof(caps),&right);
    if (n<0) goto fail;
    bool bad=h.operation!=CEDAR_BROKER_CAPS || right>=0 ||
        (h.status ? n!=0 : (size_t)n!=sizeof(caps));
    if (right>=0) close(right);
    if (bad || h.status) { errno=bad ? EPROTO : h.status; goto fail; }
    return fd;
fail:;
    int error = errno;
    fprintf(stderr,"cedar-transport: %s failed: %s\n",step,strerror(error));
    close(fd); errno = error; return -1;
}

int cedar_decoder_caps(int fd, bool broker, struct cedar_msvdx_caps *caps)
{
    return broker ? exchange(fd,CEDAR_BROKER_CAPS,NULL,0,caps,sizeof(*caps),NULL) :
                    ioctl(fd,CEDAR_MSVDX_GET_CAPS,caps);
}
int cedar_decoder_surface(int fd, bool broker, struct cedar_surface_request *r)
{
    return broker ? exchange(fd,CEDAR_BROKER_SURFACE,r,sizeof(*r),r,sizeof(*r),NULL) :
                    ioctl(fd,CEDAR_MSVDX_SURFACE,r);
}
int cedar_decoder_export(int fd, bool broker, struct cedar_frame_export *frame)
{
    if (!broker) return ioctl(fd,CEDAR_MSVDX_EXPORT_FRAME,frame);
    int right = -1;
    if (exchange(fd,CEDAR_BROKER_EXPORT,NULL,0,frame,sizeof(*frame),&right)) return -1;
    if (frame->fd != -1 || (fcntl(right,F_GETFL)&O_ACCMODE) != O_RDONLY) {
        close(right); errno = EPROTO; return -1;
    }
    frame->fd = right;
    return 0;
}
int cedar_decoder_export_blank(int fd, bool broker, struct cedar_frame_export *frame)
{
    if (!cedar_frame_blank_valid(frame)) { errno = EINVAL; return -1; }
    if (!broker) return ioctl(fd, CEDAR_MSVDX_EXPORT_BLANK, frame);
    int right = -1;
    if (exchange(fd, CEDAR_BROKER_EXPORT_BLANK, frame, sizeof(*frame), frame, sizeof(*frame), &right)) return -1;
    if (frame->fd != -1 || right < 0 || (fcntl(right, F_GETFL) & O_ACCMODE) != O_RDONLY) {
        if (right >= 0) close(right);
        errno = EPROTO; return -1;
    }
    frame->fd = right;
    return 0;
}
ssize_t cedar_decoder_submit(int fd, bool broker, const void *packet, size_t bytes)
{
    if (broker && bytes > CEDAR_BROKER_INLINE_LIMIT) {
        if (bytes > CEDAR_BROKER_MAX_PACKET) { errno=EMSGSIZE; return -1; }
        int sealed=memfd_create("cedar-submit",MFD_CLOEXEC|MFD_ALLOW_SEALING);
        if (sealed<0) return -1;
        size_t copied=0;
        while (copied<bytes) {
            ssize_t n=write(sealed,(const char *)packet+copied,bytes-copied);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) { int error=n<0?errno:EIO; close(sealed);errno=error;return -1; }
            copied+=(size_t)n;
        }
        int result=fcntl(sealed,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL);
        if (!result) result=exchange_right(fd,CEDAR_BROKER_SUBMIT_FD,NULL,0,NULL,0,NULL,sealed);
        int error=errno;close(sealed);errno=error;
        return result ? -1 : (ssize_t)bytes;
    }
    return broker ? (exchange(fd,CEDAR_BROKER_SUBMIT,packet,bytes,NULL,0,NULL) ? -1 : (ssize_t)bytes) :
                    write(fd,packet,bytes);
}
