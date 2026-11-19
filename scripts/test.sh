#!/usr/bin/env bash
# Boot the VM, run the test suite, print the score and power off.
#
#   scripts/test.sh                      all tests
#   scripts/test.sh yield smp_spread     only these tests
#
# The full console log is saved in build/test.log. If the kernel crashes
# or hangs, the end of the log (with the oops or panic) is printed.
# Set TIMEOUT (seconds, default 900) to change how long to wait.
source "$(dirname "$0")/common.sh"

tests=all
[ $# -gt 0 ] && tests=$(IFS=,; echo "$*")
LOG="$BUILD/test.log"
mkdir -p "$BUILD"

seed=()
[ -n "${WRR_SEED:-}" ] && seed=("wrrseed=$WRR_SEED")

set +e
timeout --foreground "${TIMEOUT:-900}" "$TOP/scripts/run-qemu.sh" "wrrtest=$tests" "${seed[@]}" \
	< /dev/null 2>&1 | tee "$LOG"
status=${PIPESTATUS[0]}
set -e

echo
if grep -q "^WRR-TEST SCORE" "$LOG"; then
	grep "^WRR-TEST PASSED" "$LOG"
	grep "^WRR-TEST SCORE" "$LOG"
	grep -q "^\[ FAIL \]" "$LOG" && exit 1
	exit 0
fi

echo "!! The tests did not finish: the kernel crashed, hung or failed to boot."
[ "$status" = 124 ] && echo "!! (QEMU was killed after ${TIMEOUT:-900} s)"
first=$(grep -n -m 1 -E "cut here|WARNING:|BUG:|kernel BUG|Oops|general protection|Kernel panic" "$LOG" | cut -d: -f1 || true)
if [ -n "$first" ]; then
	echo "!! The FIRST problem the kernel reported (fix this one first), from $LOG:"
	sed -n "${first},$((first + 40))p" "$LOG"
	echo "   ..."
fi
echo "!! The last lines of $LOG:"
tail -n 15 "$LOG"
exit 2
