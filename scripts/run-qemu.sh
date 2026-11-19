#!/usr/bin/env bash
# Boot the kernel you built in QEMU with 4 CPUs and the busybox initramfs.
#
#   scripts/run-qemu.sh                 interactive shell (leave with: poweroff)
#   scripts/run-qemu.sh wrrtest=all     run the tests and exit
#
# Extra arguments are appended to the kernel command line. Set CPUS or MEM
# to change the VM (defaults 4 and 1G). QEMU uses KVM when /dev/kvm is
# usable and falls back to (slower) emulation otherwise.
source "$(dirname "$0")/common.sh"

KERNEL="$KOUT/$KIMAGE"
[ -f "$KERNEL" ] || die "no kernel at $KERNEL; run: make kernel"
[ -f "$INITRD" ] || die "no initramfs at $INITRD; run: make initramfs"
need "$QEMU"

CPUS="${CPUS:-4}"
MEM="${MEM:-1G}"
APPEND="console=$CONSOLE panic=-1 oops=panic softlockup_panic=1 loglevel=5 $*"

host=$(uname -m)
guest=x86_64
[ "$KARCH" = arm64 ] && guest=aarch64
if [ "$host" = "$guest" ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
	accel=(-accel kvm -cpu host)
elif [ "$KARCH" = arm64 ]; then
	accel=(-accel tcg -cpu max)
else
	accel=(-accel tcg)
fi
machine=()
[ "$KARCH" = arm64 ] && machine=(-machine virt,gic-version=3)

echo ">> $QEMU ${accel[*]} -smp $CPUS -m $MEM   (Ctrl-A X quits QEMU)"
exec "$QEMU" "${machine[@]}" "${accel[@]}" -smp "$CPUS" -m "$MEM" \
	-nographic -no-reboot \
	-kernel "$KERNEL" -initrd "$INITRD" -append "$APPEND"
