/* SPDX-License-Identifier: MIT */
#ifndef CEDAR_DECODER_TRANSPORT_H
#define CEDAR_DECODER_TRANSPORT_H
#include <stdbool.h>
#include <sys/types.h>
#include "cedar-msvdx.h"
int cedar_decoder_open(bool *broker);
int cedar_decoder_caps(int fd, bool broker, struct cedar_msvdx_caps *caps);
int cedar_decoder_surface(int fd, bool broker, struct cedar_surface_request *request);
int cedar_decoder_export_blank(int fd, bool broker, struct cedar_frame_export *frame);
int cedar_decoder_export(int fd, bool broker, struct cedar_frame_export *frame);
ssize_t cedar_decoder_submit(int fd, bool broker, const void *packet, size_t bytes);
#endif
