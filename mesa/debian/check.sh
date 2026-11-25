#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
#
# With mesa-sgx:<arch> installed (as root, Debian 13, Debian's Mesa beside it):
# the DRI driver X's glamor and GLX clients load on gma500 and the GBM backend
# are there, ld.so takes ours ahead of Debian's, and glvnd's EGL loads ours.
# No GPU needed.
#
#   mesa/debian/check.sh amd64|i386
set -eu
case $1 in
amd64) L=/opt/sgx/lib C='libc6,x86-64' J=/etc/glvnd/egl_vendor.d/10_sgx.json ;;
i386) L=/opt/sgx/lib/i386-linux-gnu C=libc6 J=/etc/glvnd/egl_vendor.d/10_sgx_i386.json ;;
*) echo "check.sh: amd64 or i386, not '$1'" >&2; exit 2 ;;
esac
test -e "$L/dri/gma500_dri.so"
test -e "$L/gbm/dri_gbm.so"
for lib in libgbm.so.1 libGLX_mesa.so.0 libEGL_mesa.so.0; do
	ldconfig -p | grep -m1 "$lib ($C)" | grep -q "=> $L/$lib" || {
		echo "check.sh: ld.so takes another $lib first:" >&2
		ldconfig -p | grep "$lib" >&2
		exit 1
	}
done
test -e "$(sed -n 's/.*"library_path" *: *"\([^"]*\)".*/\1/p' "$J")"
# eglinfo is the system's own architecture
if [ "$(dpkg --print-architecture)" = "$1" ] && command -v eglinfo >/dev/null; then
	LD_DEBUG=libs eglinfo > /dev/null 2> /tmp/check-ld.txt || true
	grep -q "init: $L/libEGL_mesa.so.0" /tmp/check-ld.txt
	grep -q "init: $L/libgallium-" /tmp/check-ld.txt
	rm -f /tmp/check-ld.txt
fi
echo "check.sh: mesa-sgx:$1 is the Mesa in use"
