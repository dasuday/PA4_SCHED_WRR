#!/usr/bin/env bash
# Download Linux 6.8.12 into ./linux, make it a git repository, and apply
# the starter patch.
#
# The tree gets two tags:
#   base     the unmodified kernel
#   starter  the kernel plus the assignment's starter code
# Your submission is `git diff starter` (see scripts/make-patch.sh).
#
# Sources, tried in order (override with SOURCE=kernelorg|github|ubuntu):
#   kernelorg  https://cdn.kernel.org tarball (about 140 MB)
#   github     shallow git clone of the v6.8.12 tag from a GitHub mirror
#   ubuntu     /usr/src/linux-source-6.8.0 from `apt install linux-source-6.8.0`
#              (Ubuntu's 6.8 with Ubuntu's patches; also supported)
source "$(dirname "$0")/common.sh"

[ -e "$KSRC" ] && die "$KSRC already exists. Move it away first if you really want a fresh copy."
need git tar patch

TARBALL_URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz"
GIT_URL="https://github.com/gregkh/linux.git"
mkdir -p "$BUILD"

fetch_kernelorg() {
	need curl xz
	echo ">> downloading $TARBALL_URL"
	curl -fL --retry 3 -o "$BUILD/linux-$KVER.tar.xz" "$TARBALL_URL" || return 1
	mkdir -p "$KSRC"
	tar -xJf "$BUILD/linux-$KVER.tar.xz" -C "$KSRC" --strip-components=1
}

fetch_github() {
	echo ">> cloning tag v$KVER from $GIT_URL (shallow)"
	git -c advice.detachedHead=false clone -q --depth 1 --branch "v$KVER" "$GIT_URL" "$KSRC" || return 1
	rm -rf "$KSRC/.git"
}

fetch_ubuntu() {
	local tarball
	tarball=$(ls /usr/src/linux-source-6.8.0/linux-source-6.8.0.tar.bz2 2>/dev/null || true)
	if [ -z "$tarball" ]; then
		echo "   (run: sudo apt install linux-source-6.8.0)"
		return 1
	fi
	echo ">> extracting $tarball"
	mkdir -p "$KSRC"
	tar -xjf "$tarball" -C "$KSRC" --strip-components=1
	rm -rf "$KSRC/debian" "$KSRC/debian.master"
}

ok=
for src in ${SOURCE:-kernelorg github ubuntu}; do
	if "fetch_$src"; then
		ok=$src
		break
	fi
	echo ">> $src failed; trying the next source"
	rm -rf "$KSRC"
done
[ -n "$ok" ] || die "could not get the kernel source"

cd "$KSRC"
git init -q
git config user.name >/dev/null || git config user.name "CSPB 3753 student"
git config user.email >/dev/null || git config user.email "student@localhost"
# Build output never goes in here (we build with O=), but just in case:
printf '%s\n' '*.o' '*.cmd' '*.orig' '*.rej' >> .git/info/exclude
git add -A >/dev/null
git commit -q -m "Linux $KVER ($ok)"
git tag base

echo ">> applying kernel/starter.patch"
if ! git apply --index "$TOP/kernel/starter.patch" 2>/dev/null; then
	echo ">> exact apply failed; retrying with fuzz (different kernel source?)"
	patch -p1 --no-backup-if-mismatch < "$TOP/kernel/starter.patch" ||
		die "the starter patch does not apply to this kernel. Use SOURCE=kernelorg."
	git add -A
fi
git commit -q -m "SCHED_WRR starter code"
git tag starter
echo ">> done: $KSRC (tags: base, starter). Next: make kernel"
