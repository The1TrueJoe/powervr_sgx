#!/bin/sh
# SPDX-License-Identifier: MIT
# Build the mock-device test dependency locally; never installs system files.
# Dependencies: curl, tar, gcc, meson, ninja, pkg-config, valac, GLib/udev/pcap headers.
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
work=${CEDAR_UMOCKDEV_BUILD:-$repo/build/umockdev-test}
mkdir -p "$work"
work=$(CDPATH= cd -- "$work" && pwd)
archive=$work/umockdev-0.19.9.tar.xz
if ! test -f "$archive"; then
    curl -fsSL https://github.com/martinpitt/umockdev/releases/download/0.19.9/umockdev-0.19.9.tar.xz -o "$archive.part"
    mv "$archive.part" "$archive"
fi
echo "666e050f76eb61921e6d79e309c5833cef639de3ec1f9c35c74ebce1681f7c34  $archive" | sha256sum -c -
if ! test -f "$work/src/.unpacked"; then
    rm -rf "$work/src" "$work/obj"
    mkdir "$work/src"
    tar -xf "$archive" -C "$work/src" --strip-components=1
    touch "$work/src/.unpacked"
fi
if ! test -f "$work/obj/build.ninja"; then
    meson setup "$work/obj" "$work/src" --prefix="$work/install" --libdir=lib -Dgtk_doc=false
fi
ninja -C "$work/obj" -j "${CEDAR_LIBDRM_JOBS:-8}"
meson install -C "$work/obj" --no-rebuild
printf 'Test dependency prefix: %s\n' "$work/install"
