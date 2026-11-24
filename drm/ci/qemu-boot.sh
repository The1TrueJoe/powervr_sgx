#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
#
# Boot a kernel package in QEMU to a shell and load gma500_gfx and powervr_sgx.
# QEMU has no Cedarview GPU, so neither binds a device: this checks that the
# kernel boots and its modules load. The root is a Debian 13 container of the
# package's architecture, packed as the initramfs.
#
#   qemu-boot.sh <linux-image .deb> <amd64|i386>
set -eu
DEB=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
ARCH=$2
W=$(mktemp -d)
case $ARCH in
amd64) Q=qemu-system-x86_64 PLAT=linux/amd64 CPU=max ;;
# an Atom with PAE and MOVBE, as the i386 flavor needs
i386) Q=qemu-system-i386 PLAT=linux/386 CPU=n270 ;;
*) echo "qemu-boot: amd64 or i386, not '$ARCH'" >&2; exit 2 ;;
esac
cp "$DEB" "$W/k.deb"
cat > "$W/init" <<'EOF'
#!/bin/sh
PATH=/usr/sbin:/usr/bin:/sbin:/bin
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev
echo "qemu-boot: $(uname -srm), shell $0"
grep -E '^(MemTotal|HighTotal|LowTotal)' /proc/meminfo
ok=1
for m in gma500_gfx powervr_sgx; do
	modprobe $m && [ -d /sys/module/$m ] || ok=0
done
lsmod | grep -E '^(gma500_gfx|powervr_sgx|drm|gpu_sched|drm_gpuvm) '
dmesg | grep -iE 'powervr|gma500|NX \(|PAE|highmem' || true
[ $ok = 1 ] && echo QEMU-BOOT-OK
echo o > /proc/sysrq-trigger
sleep 30
EOF
chmod 755 "$W/init"

# The root: Debian 13 with kmod, the kernel's modules for the two drivers and
# what they need, and /init.
docker run --rm --platform "$PLAT" -v "$W:/w" debian:trixie sh -euc '
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -qq
	apt-get install -y -qq --no-install-recommends kmod cpio /w/k.deb >/dev/null
	kv=$(ls /lib/modules)
	cp /boot/vmlinuz-$kv /w/vmlinuz
	depmod "$kv"
	keep=$(modprobe -S "$kv" --show-depends gma500_gfx powervr_sgx | awk "\$1 == \"insmod\" {print \$2}")
	echo "$keep"
	mkdir /tmp/m
	for f in $keep; do mkdir -p "/tmp/m$(dirname "$f")"; cp "$f" "/tmp/m$f"; done
	rm -rf /lib/modules/$kv/kernel /boot /usr/share/doc /usr/share/man /usr/share/locale /var/lib/apt/lists/*
	mv /tmp/m/lib/modules/$kv/kernel /lib/modules/$kv/ || mv /tmp/m/usr/lib/modules/$kv/kernel /lib/modules/$kv/
	rm -rf /tmp/m
	depmod "$kv"
	cp /w/init /init
	cd / && find . -xdev ! -path ./w ! -path "./w/*" | cpio -o -H newc --quiet | gzip -1 > /w/initrd.gz'
ls -l "$W"

accel=tcg
[ -w /dev/kvm ] && accel=kvm
timeout 900 $Q -accel $accel -cpu $CPU -smp 2 -m 1536 -nographic -no-reboot \
	-kernel "$W/vmlinuz" -initrd "$W/initrd.gz" \
	-append "console=ttyS0 rdinit=/init panic=-1 sysrq_always_enabled" | tee "$W/console.log" || true
cp "$W/console.log" qemu-console.log
grep -q QEMU-BOOT-OK "$W/console.log"
