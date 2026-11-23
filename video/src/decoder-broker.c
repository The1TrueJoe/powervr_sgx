/* SPDX-License-Identifier: MIT
 * Experimental single-client Cedarview decoder service. Explicit foreground
 * invocation only; not installed or enabled by default. */
#define _GNU_SOURCE
#include "cedar-broker.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/capability.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop(int signum) { (void)signum; stopping = 1; }

static int restrict_privileges(void)
{
    struct __user_cap_header_struct header={.version=_LINUX_CAPABILITY_VERSION_3};
    struct __user_cap_data_struct caps[2]={{0}};
    caps[CAP_SYS_RAWIO/32].effective=caps[CAP_SYS_RAWIO/32].permitted=1U<<(CAP_SYS_RAWIO%32);
    return prctl(PR_SET_DUMPABLE,0,0,0,0) || prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0) ||
        syscall(SYS_capset,&header,caps);
}

/* All ancestors must be root-owned and not group/world-writable. Refuse stale
 * sockets instead of deleting an unknown endpoint. Use a dedicated /run dir. */
static int safe_parent(const char *path)
{
    char copy[sizeof(((struct sockaddr_un *)0)->sun_path)];
    if (!path || path[0] != '/' || strlen(path) >= sizeof(copy)) return -1;
    strcpy(copy,path);
    char *last = strrchr(copy,'/');
    if (!last || !last[1]) return -1;
    *last = 0;
    int fd = open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (fd < 0) return -1;
    char *save = NULL;
    for (char *part = strtok_r(copy,"/",&save); part; part = strtok_r(NULL,"/",&save)) {
        if (!strcmp(part,".") || !strcmp(part,"..")) { close(fd); return -1; }
        int next = openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        close(fd); fd = next;
        if (fd < 0) return -1;
        struct stat st;
        if (fstat(fd,&st) || st.st_uid || (st.st_mode&0022)) { close(fd); return -1; }
    }
    close(fd);
    return 0;
}

static int device_open(struct cedar_msvdx_caps *caps)
{
    int fd = open("/dev/cedar-msvdx-test",O_RDWR|O_CLOEXEC);
    if (fd < 0) return -1;
    const unsigned required = CEDAR_CAP_H264_DPB4 | CEDAR_CAP_FRAME_DMABUF |
        CEDAR_CAP_INDEPENDENT_SURFACES | CEDAR_CAP_SURFACE_BINDING | CEDAR_CAP_LAZY_READBACK;
    if (ioctl(fd,CEDAR_MSVDX_GET_CAPS,caps)) { int e=errno; close(fd); errno=e; return -1; }
    if (caps->size != sizeof(*caps) || caps->version != 1 || caps->codec != 1 ||
        caps->width != CEDAR_H264_MAX_WIDTH || caps->height != CEDAR_H264_MAX_HEIGHT || caps->stride != CEDAR_H264_STORAGE_STRIDE ||
        (caps->features&required) != required) { close(fd); errno=EOPNOTSUPP; return -1; }
    return fd;
}

static int storage_quota(void)
{
    FILE *f = fopen("/sys/module/gma500_gfx/parameters/msvdx_surface_pages","re");
    if (!f) return -1;
    unsigned long pages = ULONG_MAX;
    int ok = fscanf(f,"%lu",&pages); fclose(f);
    if (ok != 1) { errno=EIO; return -1; }
    if (pages > CEDAR_BROKER_MAX_PAGES-CEDAR_BROKER_SURFACE_PAGES) { errno=EDQUOT; return -1; }
    return 0;
}

static int request(int client, int device, const struct cedar_broker_header *h,
                    const void *data, bool *used, unsigned *submitted, unsigned *exported)
{
    int status = 0, right = -1;
    size_t bytes = 0;
    union {
        struct cedar_msvdx_caps caps;
        struct cedar_surface_request surface;
        struct cedar_frame_export frame;
    } response;
    memset(&response,0,sizeof(response));
    switch (h->operation) {
    case CEDAR_BROKER_CAPS:
        if (h->bytes) { status=EINVAL; break; }
        if (ioctl(device,CEDAR_MSVDX_GET_CAPS,&response.caps)) status=errno;
        else bytes=sizeof(response.caps);
        break;
    case CEDAR_BROKER_SURFACE:
        if (h->bytes != sizeof(response.surface)) { status=EINVAL; break; }
        memcpy(&response.surface,data,sizeof(response.surface));
        if (!cedar_broker_surface_valid(&response.surface)) { status=EINVAL; break; }
        if (response.surface.operation == CEDAR_SURFACE_CREATE && storage_quota()) { status=errno; break; }
        if (ioctl(device,CEDAR_MSVDX_SURFACE,&response.surface)) status=errno;
        else { bytes=sizeof(response.surface); *used=true; }
        break;
    case CEDAR_BROKER_SUBMIT:
    case CEDAR_BROKER_SUBMIT_FD: {
        if (!cedar_broker_packet_valid(data,h->bytes)) { status=EINVAL; break; }
        ssize_t n;
        do n=write(device,data,h->bytes); while (n<0 && errno==EINTR && !stopping);
        if (n < 0) status=errno;
        else if ((unsigned)n != h->bytes) status=EIO;
        else { ++*submitted; *used=true; }
        break;
    }
    case CEDAR_BROKER_EXPORT_BLANK:
        if (h->bytes != sizeof(response.frame)) { status=EINVAL; break; }
        memcpy(&response.frame,data,sizeof(response.frame));
        if (!cedar_frame_blank_valid(&response.frame)) { status=EINVAL; break; }
        if (storage_quota()) { status=errno; break; }
        if (ioctl(device,CEDAR_MSVDX_EXPORT_BLANK,&response.frame)) { status=errno; break; }
        right=response.frame.fd; response.frame.fd=-1;
        if (right < 0 || (fcntl(right,F_GETFL)&O_ACCMODE) != O_RDONLY) { status=EIO; break; }
        bytes=sizeof(response.frame); ++*exported; *used=true;
        break;
    case CEDAR_BROKER_EXPORT:
        if (h->bytes) { status=EINVAL; break; }
        response.frame.version=1; response.frame.fd=-1;
        if (ioctl(device,CEDAR_MSVDX_EXPORT_FRAME,&response.frame)) { status=errno; break; }
        right=response.frame.fd; response.frame.fd=-1;
        if (right < 0 || (fcntl(right,F_GETFL)&O_ACCMODE) != O_RDONLY) { status=EIO; break; }
        bytes=sizeof(response.frame); ++*exported;
        break;
    default:
        status=EOPNOTSUPP;
    }
    int result=cedar_broker_send(client,h->operation,status,&response,status ? 0 : bytes,status ? -1 : right);
    if (right >= 0) close(right); /* SCM_RIGHTS receiver owns the duplicate. */
    if (status) fprintf(stderr,"cedar-broker: op=%u errno=%d\n",h->operation,status);
    return result;
}

int main(int argc, char **argv)
{
    const char *path=NULL, *uid_text=NULL;
    for (int i=1; i<argc; i++) {
        if (!strcmp(argv[i],"--socket") && i+1<argc) path=argv[++i];
        else if (!strcmp(argv[i],"--uid") && i+1<argc) uid_text=argv[++i];
        else { fprintf(stderr,"Usage: %s --socket /run/PRIVATE/socket --uid UID\n",argv[0]); return 2; }
    }
    char *end=NULL;
    unsigned long allowed=uid_text ? strtoul(uid_text,&end,10) : ULONG_MAX;
    if (geteuid() || !uid_text || !*uid_text || !end || *end ||
        allowed >= UINT_MAX || safe_parent(path)) {
        fprintf(stderr,"cedar-broker: require root, explicit allowed uid, secure absolute socket parent\n"); return 2;
    }
    struct sockaddr_un address={.sun_family=AF_UNIX};
    strcpy(address.sun_path,path);
    struct stat st;
    if (!lstat(path,&st) || errno != ENOENT) { fprintf(stderr,"cedar-broker: socket path already exists or is inaccessible\n"); return 2; }
    int listener=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if (listener < 0) { perror("socket"); return 1; }
    umask(0077);
    if (bind(listener,(struct sockaddr *)&address,sizeof(address))) { perror("bind"); close(listener); return 1; }
    if (chown(path,(uid_t)allowed,(gid_t)-1) || chmod(path,0600) || listen(listener,8)) {
        perror("listen permissions"); close(listener); unlink(path); return 1;
    }
    if (restrict_privileges()) {
        perror("restrict privileges"); close(listener); unlink(path); return 1;
    }
    struct sigaction action={.sa_handler=stop};
    sigemptyset(&action.sa_mask); sigaction(SIGTERM,&action,NULL); sigaction(SIGINT,&action,NULL);
    unsigned char *data = malloc(CEDAR_BROKER_MAX_PACKET);
    if (!data) { perror("packet allocation"); close(listener); unlink(path); return 1; }
    int client=-1, device=-1;
    bool used=false;
    unsigned submitted=0, exported=0;
    fprintf(stderr,"cedar-broker: ready uid=%lu single-client page-limit=%u\n",allowed,CEDAR_BROKER_MAX_PAGES);
    fflush(stderr);
    int result=0;
    while (!stopping) {
        struct pollfd fds[]={{listener,POLLIN,0},{client,POLLIN,0}};
        int n=poll(fds,2, client>=0 && !used ? 5000 : -1);
        if (n<0) { if (errno==EINTR) continue; perror("poll"); result=1; break; }
        bool disconnect=client>=0 && !used && !n;
        if (client>=0 && fds[1].revents&(POLLIN|POLLHUP|POLLERR)) {
            struct cedar_broker_header h;
            int right=-1;
            ssize_t bytes=cedar_broker_recv(client,&h,data,CEDAR_BROKER_MAX_PACKET,&right);
            if (bytes<0) disconnect=true;
            else if (h.status || (h.operation == CEDAR_BROKER_SUBMIT_FD ? (right<0 || h.bytes) : right>=0)) {
                if (right>=0) close(right);
                cedar_broker_send(client,h.operation,EPROTO,NULL,0,-1); disconnect=true;
            } else {
                const void *payload=data;
                size_t mapped_bytes=0;
                if (h.operation == CEDAR_BROKER_SUBMIT_FD &&
                    cedar_broker_map_sealed_packet(right,&payload,&mapped_bytes)) {
                    cedar_broker_send(client,h.operation,errno,NULL,0,-1);
                } else {
                    if (mapped_bytes) h.bytes=(uint32_t)mapped_bytes;
                    if (request(client,device,&h,payload,&used,&submitted,&exported)) disconnect=true;
                    if (mapped_bytes) munmap((void *)payload,mapped_bytes);
                }
                if (right>=0) close(right);
            }
        }
        if (disconnect && client>=0) {
            close(client); client=-1;
            if (device>=0) close(device);
            device=-1;
            fprintf(stderr,"cedar-broker: disconnect submitted=%u exported=%u\n",submitted,exported);
        }
        if (fds[0].revents&POLLIN) {
            int accepted=accept4(listener,NULL,NULL,SOCK_CLOEXEC);
            if (accepted>=0) {
                struct ucred peer; socklen_t size=sizeof(peer);
                int error=0;
                if (getsockopt(accepted,SOL_SOCKET,SO_PEERCRED,&peer,&size) || size!=sizeof(peer) ||
                    peer.uid!=(uid_t)allowed) error=EACCES;
                else if (client>=0) error=EBUSY;
                struct timeval timeout={.tv_sec=2};
                if (setsockopt(accepted,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))) error=EIO;
                struct cedar_msvdx_caps caps;
                int accepted_device=-1;
                if (!error && (accepted_device=device_open(&caps))<0) error=errno;
                if (error) { cedar_broker_send(accepted,CEDAR_BROKER_CAPS,error,NULL,0,-1); close(accepted); }
                else if (cedar_broker_send(accepted,CEDAR_BROKER_CAPS,0,&caps,sizeof(caps),-1)) {
                    close(accepted_device); close(accepted);
                } else {
                    client=accepted; device=accepted_device; used=false; submitted=exported=0;
                }
            }
        }
    }
    if (client>=0) close(client);
    if (device>=0) close(device);
    close(listener); unlink(path);
    free(data);
    return result;
}
