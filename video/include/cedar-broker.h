/* SPDX-License-Identifier: MIT */
#ifndef CEDAR_BROKER_H
#define CEDAR_BROKER_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "cedar-msvdx.h"

/* Private, little-endian, local SOCK_SEQPACKET protocol. No native pointers,
 * arbitrary ioctls, register/MMU words or device paths. Frame fds expose only
 * image backing, with access rights checked by both broker and transport.
 * Server sends CAPS greeting/status immediately; clients read it before sending
 * requests. Afterwards transactions are synchronous, one request/reply at a time. */
#define CEDAR_BROKER_MAGIC 0x31425243U
#define CEDAR_BROKER_VERSION 1U
#define CEDAR_BROKER_INLINE_LIMIT (CEDAR_H264_FULL_LENGTH_OFFSET + 4U + 65535U)
#define CEDAR_BROKER_MAX_PACKET CEDAR_H264_PACKET_LIMIT
#define CEDAR_BROKER_SURFACE_PAGES (CEDAR_H264_STORAGE_PAGES + CEDAR_H264_COLOCATED_BYTES / 4096U)
#define CEDAR_BROKER_MAX_PAGES (64U * CEDAR_BROKER_SURFACE_PAGES)
enum cedar_broker_operation {
    CEDAR_BROKER_CAPS = 1,
    CEDAR_BROKER_SURFACE,
    CEDAR_BROKER_SUBMIT,
    CEDAR_BROKER_EXPORT,
    CEDAR_BROKER_SUBMIT_FD, /* empty body, one immutable sealed memfd */
    CEDAR_BROKER_EXPORT_BLANK
};
struct cedar_broker_header {
    uint32_t magic, version, operation, bytes;
    int32_t status; /* positive errno in replies; zero in requests */
    uint32_t reserved;
};
_Static_assert(sizeof(struct cedar_broker_header) == 24, "broker header ABI");
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "little-endian prototype");
/* recv closes all rights on framing failure. Caller owns the single received fd
 * on success and must reject/close it when inappropriate for the operation. */
int cedar_broker_send(int socket_fd, unsigned operation, int status,
                      const void *data, size_t bytes, int right);
ssize_t cedar_broker_recv(int socket_fd, struct cedar_broker_header *header,
                          void *data, size_t capacity, int *right);
int cedar_broker_packet_valid(const void *packet, size_t bytes);
int cedar_broker_map_sealed_packet(int fd, const void **data, size_t *bytes);
int cedar_broker_surface_valid(const struct cedar_surface_request *r);
#endif
