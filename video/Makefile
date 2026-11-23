# SPDX-License-Identifier: MIT
CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
WARN = -std=c11 -Wall -Wextra -Werror -Wformat=2 -Wshadow
VA_FLAGS = $(shell $(PKG_CONFIG) --cflags libva libdrm)
TRANSPORT = src/decoder-transport.c src/broker-wire.c
HEADERS = src/decoder-transport.h include/cedar-broker.h include/cedar-msvdx.h
.PHONY: all check clean
all: build/cedar_drv_video.so build/cedar-decoder-broker
build:
	mkdir -p $@
build/cedar_drv_video.so: src/va-driver.c $(TRANSPORT) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Wno-unused-parameter -fPIC -shared -Iinclude $(VA_FLAGS) src/va-driver.c $(TRANSPORT) -o $@ $(LDFLAGS) -pthread -ldl
build/cedar-decoder-broker: src/decoder-broker.c src/broker-wire.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Iinclude src/decoder-broker.c src/broker-wire.c -o $@ $(LDFLAGS)
build/test-va-image: tests/test-va-image.c src/va-driver.c $(TRANSPORT) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Wno-unused-parameter -Iinclude $(VA_FLAGS) tests/test-va-image.c $(TRANSPORT) -o $@ $(LDFLAGS) -pthread -ldl
build/test-broker: tests/test-broker.c $(TRANSPORT) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Iinclude tests/test-broker.c $(TRANSPORT) -o $@ $(LDFLAGS) -ldl
build/test-dmabuf-planes: tests/test-dmabuf-planes.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(shell $(PKG_CONFIG) --cflags egl glesv2 libdrm) $< -o $@ $(LDFLAGS) $(shell $(PKG_CONFIG) --libs egl glesv2)
build/test-va-export-frames: tests/test-va-export-frames.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(shell $(PKG_CONFIG) --cflags libavcodec libavformat libavutil libva) $< -o $@ $(LDFLAGS) $(shell $(PKG_CONFIG) --libs libavcodec libavformat libavutil libva)
build/test-kernel-storage: tests/test-kernel-storage.c include/cedar-msvdx.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Iinclude $< -o $@ $(LDFLAGS)
build/test-va-export: tests/test-va-export.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(shell $(PKG_CONFIG) --cflags libva-drm libdrm) $< -o $@ $(LDFLAGS) $(shell $(PKG_CONFIG) --libs libva-drm libdrm)
build/test-h264-dma: tests/test-h264-dma.c include/cedar-msvdx-dma.h include/cedar-msvdx.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Iinclude tests/test-h264-dma.c -o $@ $(LDFLAGS)
check: all build/test-va-image build/test-broker build/test-h264-dma
	./build/test-va-image
	./build/test-broker
	./build/test-h264-dma
	python3 tests/test-verify-decode.py
	python3 tests/test-h264-stream.py
clean:
	rm -rf build

PREFIX ?= /opt/cedar-msvdx
BINDIR ?= /usr/local/bin
.PHONY: install
# Stages userspace only. Firmware, modules, service activation and boot setup
# are explicit operator steps; DESTDIR is supported for package construction.
install: all
	install -Dm755 build/cedar_drv_video.so $(DESTDIR)$(PREFIX)/cedar_drv_video.so
	install -Dm755 build/cedar-decoder-broker $(DESTDIR)$(PREFIX)/cedar-decoder-broker
	install -d $(DESTDIR)$(BINDIR)
	for launcher in tools/cedar-*; do install -m755 "$$launcher" $(DESTDIR)$(BINDIR)/; done
