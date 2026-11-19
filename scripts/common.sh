# Shared settings for the scripts in this directory. Source it; don't run it.

set -euo pipefail

TOP="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KVER="${KVER:-6.8.12}"            # the kernel this assignment is written for
KSRC="${KSRC:-$TOP/linux}"        # kernel source tree (a git repo; your work goes here)
BUILD="${BUILD:-$TOP/build}"      # everything generated lives here

# Build the guest for the machine we're on: x86_64 or arm64.
case "${WRR_ARCH:-$(uname -m)}" in
x86_64 | amd64)
	KARCH=x86_64
	KIMAGE=arch/x86/boot/bzImage
	QEMU=qemu-system-x86_64
	CONSOLE=ttyS0
	;;
aarch64 | arm64)
	KARCH=arm64
	KIMAGE=arch/arm64/boot/Image
	QEMU=qemu-system-aarch64
	CONSOLE=ttyAMA0
	;;
*)
	echo "unsupported architecture: $(uname -m) (need x86_64 or aarch64)" >&2
	exit 1
	;;
esac

KOUT="$BUILD/linux-$KARCH"        # out-of-tree kernel build directory
INITRD="$BUILD/initramfs-$KARCH.cpio.gz"
USEROUT="$BUILD/user-$KARCH"     # static user programs for the VM
JOBS="${JOBS:-$(nproc)}"

# Cross-compiling (e.g. WRR_ARCH=arm64 on an x86_64 machine) needs a prefix.
if [ "$KARCH" = arm64 ] && [ "$(uname -m)" != aarch64 ]; then
	CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
fi
CROSS_COMPILE="${CROSS_COMPILE:-}"
export CROSS_COMPILE

die() {
	echo "error: $*" >&2
	exit 1
}

need() {
	for tool in "$@"; do
		command -v "$tool" >/dev/null 2>&1 || die "'$tool' not found. Install the packages listed in the README (Setup)."
	done
}
