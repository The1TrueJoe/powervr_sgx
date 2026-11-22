#!/bin/sh
# SPDX-License-Identifier: MIT
# Native build with an isolated prefix; use a matching Debian host/container
# or supply a Meson cross file through CEDAR_MPV_MESON_ARGS for the target ABI.
set -eu
case "${1:-}" in ''|--install) ;; *) echo 'usage: build-mpv-nv12.sh [--install]' >&2; exit 2 ;; esac
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=${CEDAR_MPV_BUILD:-$repo/build/mpv-nv12}
archive=$output/mpv-v0.40.0.tar.gz
source=$output/mpv-0.40.0
patch_dir=$repo/patches/mpv
mkdir -p "$output"
if ! test -f "$archive"; then
    curl -fL https://github.com/mpv-player/mpv/archive/refs/tags/v0.40.0.tar.gz -o "$archive.part"
    mv "$archive.part" "$archive"
fi
echo "10a0f4654f62140a6dd4d380dcf0bbdbdcf6e697556863dc499c296182f081a3  $archive" | sha256sum -c -
patch_sha=$(cat "$patch_dir"/*.patch | sha256sum | cut -d' ' -f1)
if test "$(cat "$output/patch.sha256" 2>/dev/null)" != "$patch_sha"; then
    rm -rf "$source" "$output/obj"
    tar -xzf "$archive" -C "$output"
    for patch_file in "$patch_dir"/*.patch; do
        patch -d "$source" -p1 --fuzz=0 < "$patch_file"
    done
    echo "$patch_sha" > "$output/patch.sha256"
fi
if ! test -f "$output/obj/build.ninja"; then
    # Intentional word splitting allows ordinary Meson -D options/cross files.
    meson setup "$output/obj" "$source" --buildtype=release --prefix=/opt/cedar-mpv \
        -Dlibmpv=false -Dbuild-date=false ${CEDAR_MPV_MESON_ARGS:-}
fi
ninja -C "$output/obj" mpv
if test "${1:-}" = --install; then
    install -Dm755 "$output/obj/mpv" /opt/cedar-mpv/bin/mpv
fi
printf 'Player: %s\nPatch SHA256: %s\n' "$output/obj/mpv" "$patch_sha"
