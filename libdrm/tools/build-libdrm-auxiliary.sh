#!/bin/sh
# SPDX-License-Identifier: MIT
# Native Debian build, isolated prefix; never replaces Debian's libdrm.
# Requires curl, xz-utils, patch, gcc, meson, ninja-build, pkg-config.
# --install copies only into /opt/cedar-libdrm; default builds without installing.
set -eu
case "${1:-}" in ''|--install) ;; *) echo 'usage: build-libdrm-auxiliary.sh [--install]' >&2; exit 2 ;; esac
. /etc/os-release
test "$ID" = debian && test "$VERSION_ID" = 13 || {
    echo 'Build natively on Debian 13 for the target glibc ABI.' >&2; exit 2;
}
repo=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
patch_dir=$repo/patches/libdrm
patch_sha=$(cat "$patch_dir"/*.patch | sha256sum | cut -d' ' -f1)
build_root=${CEDAR_LIBDRM_BUILD:-$repo/build/libdrm-auxiliary}
work=$build_root/native-$(printf '%s' "$patch_sha" | cut -c1-12)
archive=$build_root/libdrm-2.4.124.tar.xz
mkdir -p "$work"
if ! test -f "$archive"; then
    curl -fsSL https://deb.debian.org/debian/pool/main/libd/libdrm/libdrm_2.4.124.orig.tar.xz -o "$archive.part"
    mv "$archive.part" "$archive"
fi
echo "ac36293f61ca4aafaf4b16a2a7afff312aa4f5c37c9fbd797de9e3c0863ca379  $archive" | sha256sum -c -
if ! test -f "$work/.patched"; then
    mkdir -p "$work/src"
    tar -xf "$archive" -C "$work/src" --strip-components=1
    for patch_file in "$patch_dir"/*.patch; do
        patch -d "$work/src" -p1 --fuzz=0 < "$patch_file"
    done
    printf '%s\n' "$patch_sha" > "$work/.patched"
fi
if ! test -f "$work/build/build.ninja"; then
    meson setup "$work/build" "$work/src" --prefix=/opt/cedar-libdrm --libdir=lib \
        -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled \
        -Dvmwgfx=disabled -Dfreedreno=disabled -Dvc4=disabled -Detnaviv=disabled \
        -Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled
fi
ninja -C "$work/build" -j "${CEDAR_LIBDRM_JOBS:-8}"
if test "${1:-}" = --install; then
    # Stage first, then replace the versioned library with rename(2): never
    # truncate an inode already mapped by the running compositor.
    DESTDIR="$work/stage" meson install -C "$work/build" --no-rebuild
    staged=$work/stage/opt/cedar-libdrm
    mkdir -p /opt/cedar-libdrm/lib/pkgconfig
    cp -a "$staged/include" /opt/cedar-libdrm/
    cp -a "$staged/lib/pkgconfig/." /opt/cedar-libdrm/lib/pkgconfig/
    install -m 0755 "$staged/lib/libdrm.so.2.124.0" /opt/cedar-libdrm/lib/.libdrm.so.new
    mv -f /opt/cedar-libdrm/lib/.libdrm.so.new /opt/cedar-libdrm/lib/libdrm.so.2.124.0
    ln -sfn libdrm.so.2.124.0 /opt/cedar-libdrm/lib/libdrm.so.2
    ln -sfn libdrm.so.2 /opt/cedar-libdrm/lib/libdrm.so
fi
printf 'Build: %s\nPatch SHA256: %s\n' "$work/build" "$patch_sha"
