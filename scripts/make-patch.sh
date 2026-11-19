#!/usr/bin/env bash
# Write your kernel changes (everything since the "starter" tag, committed
# or not) to submission/wrr.patch. That file plus your report is what you
# submit.
source "$(dirname "$0")/common.sh"

[ -d "$KSRC/.git" ] || die "$KSRC is not a git repository; run: make get-kernel"
cd "$KSRC"
git rev-parse -q --verify starter >/dev/null || die "no 'starter' tag in $KSRC"

untracked=$(git ls-files --others --exclude-standard -- kernel include arch init)
if [ -n "$untracked" ]; then
	echo ">> warning: these new files are NOT in the patch (git add them if they belong):"
	echo "$untracked" | sed 's/^/     /'
fi

mkdir -p "$TOP/submission"
out="$TOP/submission/wrr.patch"
git diff starter -- . > "$out"
[ -s "$out" ] || { rm -f "$out"; die "no changes since the starter code"; }

# Check that it applies to the starter code, using a scratch index (no
# files are checked out or changed).
tmp_index=$(mktemp)
trap 'rm -f "$tmp_index"' EXIT
GIT_INDEX_FILE="$tmp_index" git read-tree starter
if ! GIT_INDEX_FILE="$tmp_index" git apply --cached --check "$out"; then
	die "the patch does not apply to the starter code; ask for help"
fi
echo ">> wrote $out (it applies cleanly to the starter code)"
git diff --stat starter -- . | tail -n 20
