#!/bin/sh
# Install and upgrade from a built apt repository, as a user would, in a Debian 13
# container (as root). Each scenario starts from a clean system:
#
#   upgrade   the previous release installed, then `apt upgrade`; then mesa-sgx:i386
#   i386      the previous release installed, then only `apt install mesa-sgx:i386`:
#             Mesa's two architectures move to the new release, and the kernel with them
#             when the new Mesa breaks the installed one (mesa/debian/BREAKS: a uAPI
#             change, as 0.1.x to 0.2.0); a release on the same uAPI leaves it
#   fresh     the docs/INSTALL.md install, both Mesa architectures
#   ci        CI-build versions that sort above the release (the 0.1.x CI artifacts did),
#             then the --allow-downgrades command the release notes give
#
# With i386, in an i386 container (a Cedarview whose BIOS turns 64-bit mode off: no
# amd64 at all), instead:
#   upgrade   when the previous release has an i386 kernel: it installed, then `apt upgrade`
#   fresh     the docs/INSTALL.md install: kernel, firmware, mesa-sgx, then
#             mesa/debian/check.sh: ours ahead of Debian's Mesa
#   breaks    a kernel older than the new Mesa's Breaks bound moves with mesa-sgx
#
#   tools/release/repo-test.sh SITE OLD_DIR [i386]
#
# SITE is apt-repo.sh's output, OLD_DIR the previous release's .debs.
set -eu
SITE=$(cd "$1" && pwd) OLD=$(cd "$2" && pwd) MODE=${3:-amd64}
HERE=$(cd "$(dirname "$0")" && pwd)
export DEBIAN_FRONTEND=noninteractive
chmod -R a+rX "$SITE"
W=$(mktemp -d)
chmod 755 "$W"

newest() {   # PACKAGE ARCH: its version in the repository's pool
	for f in "$SITE"/pool/main/*/*/"$1"_*_"$2".deb; do dpkg-deb -f "$f" Version; done | sort -V | tail -1
}
K=$(newest linux-image-7.3.3-sgx amd64) F=$(newest sgx545-firmware all) M=$(newest mesa-sgx amd64)
[ "$(newest mesa-sgx i386)" = "$M" ]
[ "$(newest linux-image-7.3.3-sgx i386)" = "$K" ]
echo "repository: kernel $K, firmware $F, mesa $M"

have() {   # PACKAGE VERSION
	got=$(dpkg-query -W -f '${Version}' "$1" 2>/dev/null || true)
	[ "$got" = "$2" ] || { echo "repo-test: $1 is '$got', want $2" >&2; exit 1; }
}
clean() {
	apt-get purge -y 'mesa-sgx:*' sgx545-firmware 'linux-image-7.3.3-sgx*' >/dev/null 2>&1 || true
	rm -rf /lib/modules/7.3.3-sgx /opt/sgx
}
old() {
	clean
	apt-get install -y --no-install-recommends "$OLD"/linux-image-7.3.3-sgx_*_amd64.deb \
		"$OLD"/sgx545-firmware_*_all.deb "$OLD"/mesa-sgx_*_amd64.deb
	dpkg -l 'linux-image-7.3.3-sgx' sgx545-firmware 'mesa-sgx*' | grep '^ii'
}

# the repository's own packages, renumbered the way the 0.1.x CI builds were
ci() {   # PACKAGE ARCH NEWVERSION
	rm -rf "$W/x" && mkdir "$W/x"
	dpkg-deb -R "$SITE"/pool/main/*/*/"$1"_*_"$2".deb "$W/x"
	sed -i "s/^Version: .*/Version: $3/; /^Breaks:/d" "$W/x/DEBIAN/control"
	dpkg-deb --root-owner-group -b "$W/x" "$W/ci-$1-$2.deb" >/dev/null
}
breaks_bound() {   # ARCH: the kernel version the repository's Mesa breaks below, or nothing
	for f in "$SITE"/pool/main/*/*/mesa-sgx_*_"$1".deb; do
		if [ "$(dpkg-deb -f "$f" Version)" = "$M" ]; then
			dpkg-deb -f "$f" Breaks | sed -n 's/.*linux-image-7\.3\.3-sgx (<< \([^)]*\)).*/\1/p'
		fi
	done
}

[ "$MODE" = i386 ] || dpkg --add-architecture i386
apt-get update -qq
apt-get install -y --no-install-recommends ca-certificates kmod >/dev/null
install -m 644 "$SITE/powervr-sgx.gpg" /usr/share/keyrings/powervr-sgx.gpg
printf 'Types: deb\nURIs: file:%s\nSuites: trixie\nComponents: main\nSigned-By: /usr/share/keyrings/powervr-sgx.gpg\n' \
	"$SITE" > /etc/apt/sources.list.d/powervr-sgx.sources
apt-get update

if [ "$MODE" = i386 ]; then
	[ "$(dpkg --print-architecture)" = i386 ] && [ -z "$(dpkg --print-foreign-architectures)" ]
	# Debian's Mesa too, which ours must come ahead of; eglinfo
	apt-get install -y --no-install-recommends libgbm1 libegl-mesa0 libglx-mesa0 mesa-utils >/dev/null

	if ls "$OLD"/linux-image-7.3.3-sgx_*_i386.deb >/dev/null 2>&1; then
		echo "== i386 upgrade"
		clean
		apt-get install -y --no-install-recommends "$OLD"/linux-image-7.3.3-sgx_*_i386.deb \
			"$OLD"/sgx545-firmware_*_all.deb "$OLD"/mesa-sgx_*_i386.deb
		apt-get upgrade -y
		have linux-image-7.3.3-sgx:i386 "$K"; have sgx545-firmware "$F"; have mesa-sgx:i386 "$M"
	fi

	echo "== i386 fresh"
	clean
	apt-get install -y linux-image-7.3.3-sgx sgx545-firmware mesa-sgx
	have linux-image-7.3.3-sgx:i386 "$K"; have sgx545-firmware "$F"; have mesa-sgx:i386 "$M"
	dpkg -l 'linux-image-7.3.3-sgx' sgx545-firmware 'mesa-sgx*' | grep '^ii'
	test -e /boot/vmlinuz-7.3.3-sgx
	find /lib/modules/7.3.3-sgx -name 'powervr_sgx.ko*' | grep -q .
	test -e /lib/firmware/powervr_sgx/sgx545_ukernel.bin
	sh "$HERE/../../mesa/debian/check.sh" i386

	bound=$(breaks_bound i386)
	if [ -n "$bound" ]; then
		echo "== i386 breaks"
		clean
		# a kernel from before the bound, then only Mesa
		ci linux-image-7.3.3-sgx i386 7.3.3-sgx0~old1
		dpkg --compare-versions 7.3.3-sgx0~old1 lt "$bound"
		apt-get install -y --no-install-recommends "$W"/ci-linux-image-7.3.3-sgx-i386.deb
		apt-get install -y mesa-sgx
		have linux-image-7.3.3-sgx:i386 "$K"; have mesa-sgx:i386 "$M"
	fi

	clean
	echo "repo-test: all i386 scenarios passed"
	exit 0
fi

echo "== upgrade"
old
apt-get upgrade -y
have linux-image-7.3.3-sgx "$K"; have sgx545-firmware "$F"; have mesa-sgx:amd64 "$M"
apt-get install -y mesa-sgx:i386
have mesa-sgx:i386 "$M"; have mesa-sgx:amd64 "$M"

echo "== i386"
old
apt-get install -y mesa-sgx:i386
have mesa-sgx:i386 "$M"; have mesa-sgx:amd64 "$M"
bound=$(breaks_bound amd64)
oldk=$(dpkg-deb -f "$(ls "$OLD"/linux-image-7.3.3-sgx_*_amd64.deb | head -1)" Version)
if [ -n "$bound" ] && dpkg --compare-versions "$oldk" lt "$bound"; then
	have linux-image-7.3.3-sgx "$K"
else
	have linux-image-7.3.3-sgx "$oldk"
fi

echo "== fresh"
clean
apt-get install -y linux-image-7.3.3-sgx sgx545-firmware mesa-sgx mesa-sgx:i386
have linux-image-7.3.3-sgx "$K"; have sgx545-firmware "$F"; have mesa-sgx:amd64 "$M"; have mesa-sgx:i386 "$M"
test -e /opt/sgx/lib/dri/gma500_dri.so
test -e /opt/sgx/lib/i386-linux-gnu/dri/gma500_dri.so

echo "== ci"
clean
mv=${M%%+*}
ci linux-image-7.3.3-sgx amd64 7.3.3-sgx1
ci sgx545-firmware all 0~git20261112.694a777
ci mesa-sgx amd64 "$mv+sgx20261112.694a777"
ci mesa-sgx i386 "$mv+sgx20261112.694a777"
apt-get install -y --no-install-recommends "$W"/ci-*.deb
dpkg -l 'linux-image-7.3.3-sgx' sgx545-firmware 'mesa-sgx*' | grep '^ii'
apt-get upgrade -y
have mesa-sgx:amd64 "$mv+sgx20261112.694a777"   # apt keeps the higher CI version
apt-get install -y --allow-downgrades linux-image-7.3.3-sgx="$K" sgx545-firmware="$F" \
	mesa-sgx:amd64="$M" mesa-sgx:i386="$M"
have linux-image-7.3.3-sgx "$K"; have sgx545-firmware "$F"; have mesa-sgx:amd64 "$M"; have mesa-sgx:i386 "$M"

clean
echo "repo-test: all scenarios passed"
