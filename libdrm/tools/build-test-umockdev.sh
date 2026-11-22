#!/bin/sh
# SPDX-License-Identifier: MIT
# Build the mock-device test dependency locally; never installs system files.
# Dependencies: curl, tar, gcc, meson, ninja, pkg-config, valac, GLib/udev/pcap headers.
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
work=${CEDAR_UMOCKDEV_BUILD:-$repo/build/umockdev-test}
mkdir -p "$work"
work=$(CDPATH= cd -- "$work" && pwd)
archive=$work/umockdev-0.19.9.tar.gz
if ! test -f "$archive"; then
    curl -fsSL https://github.com/martinpitt/umockdev/archive/697f870d3fc3492c15aabf58c69ec0da57c68f23.tar.gz -o "$archive.part"
    mv "$archive.part" "$archive"
fi
echo "df90c97e2ee3c0705ef7846835a0b8c37aa4bd2f6a3cd8102b374e706cc02fb5  $archive" | sha256sum -c -
if ! test -d "$work/src"; then
    mkdir "$work/src"
    tar -xf "$archive" -C "$work/src" --strip-components=1
    printf '0.19.9\n' > "$work/src/.version"
fi
if ! test -f "$work/obj/build.ninja"; then
    meson setup "$work/obj" "$work/src" --prefix="$work/install" --libdir=lib -Dgtk_doc=false
fi
ninja -C "$work/obj" -j "${CEDAR_LIBDRM_JOBS:-8}"
meson install -C "$work/obj" --no-rebuild
printf 'Test dependency prefix: %s\n' "$work/install"
