#!/usr/bin/env bash
# Configure (first time only) and build the kernel for the QEMU test VM.
#
# The build happens out of tree, in build/linux-<arch>, so ./linux stays a
# clean git working tree. Only the files you change get rebuilt, so after
# the first build (a few minutes) a rebuild usually takes seconds.
#
#   scripts/build-kernel.sh            build
#   scripts/build-kernel.sh config     redo the configuration
#   scripts/build-kernel.sh menuconfig change options by hand
source "$(dirname "$0")/common.sh"

[ -f "$KSRC/Makefile" ] || die "no kernel in $KSRC; run: make get-kernel"
need make "${CROSS_COMPILE}gcc" flex bison bc
mkdir -p "$KOUT"
kmake() { make -C "$KSRC" O="$KOUT" ARCH="$KARCH" "$@"; }

configure() {
	echo ">> configuring: tinyconfig + kernel/qemu-$KARCH.config"
	rm -f "$KOUT/.config"
	kmake tinyconfig >/dev/null
	"$KSRC/scripts/kconfig/merge_config.sh" -m -O "$KOUT" "$KOUT/.config" \
		"$TOP/kernel/qemu-$KARCH.config" >/dev/null 2>&1
	kmake olddefconfig >/dev/null 2>&1
	# Warn about options that did not stick (missing dependencies).
	grep -o '^CONFIG_[A-Z0-9_]*=[^ ]*' "$TOP/kernel/qemu-$KARCH.config" | while read -r opt; do
		grep -qx "$opt" "$KOUT/.config" || echo "   note: $opt is not set in the final config"
	done
}

case "${1:-build}" in
config) configure; exit 0 ;;
menuconfig) kmake menuconfig; exit 0 ;;
build) ;;
*) die "usage: $0 [build|config|menuconfig]" ;;
esac

[ -f "$KOUT/.config" ] || configure

echo ">> building $KARCH kernel with $JOBS jobs (log: $KOUT/build.log)"
start=$(date +%s)
if ! kmake -j"$JOBS" "$(basename "$KIMAGE")" > "$KOUT/build.log" 2>&1; then
	echo ">> BUILD FAILED. The first errors:"
	grep -n -m 20 -E "error:|Error [0-9]" "$KOUT/build.log" || tail -n 30 "$KOUT/build.log"
	exit 1
fi
# Show compiler warnings in the files this assignment touches (the rest of
# the kernel has a few harmless ones of its own).
changed=$(git -C "$KSRC" diff --name-only base 2>/dev/null || true)
[ -n "$changed" ] || changed="kernel/sched/"
warnings=$(grep -F "$changed" "$KOUT/build.log" | grep -E "warning:|error:" || true)
if [ -n "$warnings" ]; then
	echo ">> compiler warnings in files you changed (fix these; they are often bugs):"
	echo "$warnings" | head -n 30
fi
echo ">> built $KOUT/$KIMAGE in $(( $(date +%s) - start ))s"
