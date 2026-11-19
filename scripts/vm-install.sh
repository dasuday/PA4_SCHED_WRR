#!/usr/bin/env bash
# OPTIONAL (not graded): build your scheduler into a full Ubuntu kernel and
# install it on this machine, next to the kernel you have now.
#
# Only do this in a virtual machine you can afford to break, after
# `make test` passes. GRUB keeps your current kernel: if the new one does
# not boot, choose the old one under "Advanced options for Ubuntu" (hold
# Shift, or press Esc, while the VM starts to see the GRUB menu).
#
#   scripts/vm-install.sh build     configure from /boot/config-$(uname -r) and build
#   scripts/vm-install.sh install   install the result (uses sudo)
#
# The configuration is your running kernel's, trimmed with localmodconfig to
# the modules loaded right now, so the build takes minutes rather than hours.
source "$(dirname "$0")/common.sh"

[ "$(id -u)" != 0 ] || die "run this as your normal user; it calls sudo when it needs to"
[ "$KARCH" = "$( [ "$(uname -m)" = aarch64 ] && echo arm64 || echo x86_64 )" ] ||
	die "vm-install only builds for the machine it runs on"
[ -f "$KSRC/Makefile" ] || die "no kernel in $KSRC; run: make get-kernel"
VMOUT="$BUILD/linux-vm"
kmake() { make -C "$KSRC" O="$VMOUT" "$@"; }

build() {
	local cfg="${VM_CONFIG:-/boot/config-$(uname -r)}"
	need make gcc flex bison bc perl
	[ -f "$cfg" ] || die "no $cfg to start from"
	for pkg in libelf-dev libssl-dev; do
		dpkg -s "$pkg" >/dev/null 2>&1 || die "missing package $pkg (sudo apt install $pkg)"
	done
	mkdir -p "$VMOUT"
	if [ ! -f "$VMOUT/.config" ]; then
		echo ">> starting from $cfg"
		cp "$cfg" "$VMOUT/.config"
		lsmod > "$VMOUT/lsmod.txt" || die "lsmod failed"
		# Ubuntu's config names its signing certificates and wants BTF
		# debug info; neither is needed (or available) here.
		"$KSRC/scripts/config" --file "$VMOUT/.config" \
			--set-str LOCALVERSION "-wrr" \
			--set-str SYSTEM_TRUSTED_KEYS "" \
			--set-str SYSTEM_REVOCATION_KEYS "" \
			--disable DEBUG_INFO_BTF \
			--disable DEBUG_INFO_DWARF5 \
			--disable DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT \
			--enable DEBUG_INFO_NONE
		{ yes '' || :; } | kmake LSMOD="$VMOUT/lsmod.txt" localmodconfig > /dev/null 2>&1
		kmake olddefconfig > /dev/null
	fi
	echo ">> building (log: $VMOUT/build.log); this takes a while"
	if ! kmake -j"$JOBS" > "$VMOUT/build.log" 2>&1; then
		grep -n -m 20 -E "error:|Error [0-9]" "$VMOUT/build.log" || tail -n 30 "$VMOUT/build.log"
		die "build failed"
	fi
	echo ">> built $(kmake -s kernelrelease). Next: scripts/vm-install.sh install"
}

install() {
	local rel
	[ -f "$VMOUT/vmlinux" ] || die "build it first: scripts/vm-install.sh build"
	rel=$(kmake -s kernelrelease)
	echo ">> this will install kernel $rel with sudo and update GRUB."
	read -r -p "   Continue? [y/N] " ok
	[ "$ok" = y ] || [ "$ok" = Y ] || exit 1
	sudo make -C "$KSRC" O="$VMOUT" modules_install
	sudo make -C "$KSRC" O="$VMOUT" install
	echo ">> installed $rel. Reboot; the newest kernel boots by default."
	echo "   After boot, check with: uname -r   (should print $rel)"
	echo "   Remove it later with: sudo rm -rf /boot/*$rel* /lib/modules/$rel && sudo update-grub"
}

case "${1:-}" in
build) build ;;
install) install ;;
*) die "usage: $0 build|install" ;;
esac
