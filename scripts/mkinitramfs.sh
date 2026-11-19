#!/usr/bin/env bash
# Build the VM's root filesystem: busybox, initramfs/init and the static
# programs from user/, packed into build/initramfs.cpio.gz.
source "$(dirname "$0")/common.sh"

need cpio gzip
BUSYBOX="${BUSYBOX:-$(command -v busybox || true)}"
[ -n "$BUSYBOX" ] || die "busybox not found. Install it: sudo apt install busybox-static"
if file -L "$BUSYBOX" 2>/dev/null | grep -q "dynamically linked"; then
	die "$BUSYBOX is dynamically linked. Install busybox-static (sudo apt install busybox-static)."
fi

make -s -C "$TOP/user" CC="${CROSS_COMPILE}gcc" OUT="$USEROUT"

root="$BUILD/initramfs"
rm -rf "$root"
mkdir -p "$root"/{bin,dev,etc,proc,sys,tmp,root}
cp "$BUSYBOX" "$root/bin/busybox"
cp "$USEROUT"/* "$root/bin/"
cp "$TOP/initramfs/init" "$root/init"
chmod +x "$root/init"

(cd "$root" && find . | LC_ALL=C sort | cpio -o -H newc --owner=0:0 --quiet) | gzip -9 > "$INITRD"
echo ">> wrote $INITRD ($(du -h "$INITRD" | cut -f1))"
