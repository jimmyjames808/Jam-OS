#!/bin/sh
# The shell tests' verdicts (tools/serial-feed.py: a failure line from a
# test program fails the script, whatever it waits for). One boot runs
# tools/shell-tests/verdict.txt, whose `utest only verdict_canary` the
# shell has told, by UTEST_CANARY, to behave as MODE:
#   ok      passes: the run passes (the control)
#   fail    a check fails: the run FAILS, and the script's output names the
#           line "utest: 0 passed, 1 FAILED" (the script's own waits would
#           all have matched)
#   crash   utest faults and gives no summary: the run FAILS and says a
#           utest began and gave none
# Usage: tools/verdict-test.sh <outdir> ok|fail|crash; exit 0 on PASS.
set -eu
out=${1:?usage: tools/verdict-test.sh <outdir> ok|fail|crash}
mode=${2:?usage: tools/verdict-test.sh <outdir> ok|fail|crash}
case $mode in ok | fail | crash) ;; *) echo "verdict: unknown mode '$mode'"; exit 2 ;; esac
mkdir -p "$out"
name=verdict-$mode
sed "s/@MODE@/$mode/" tools/shell-tests/verdict.txt > "$out/$name.txt"
st=0
QEMU_TIMEOUT=${QEMU_TIMEOUT:-120} QEMU_INPUT="$out/$name.txt" \
    tools/qemu-test.sh "$out" "$name" shell > "$out/$name.out" 2>&1 || st=$?
ok=1
want() {
    grep -aqE -- "$1" "$out/$name.out" || { echo "verdict $mode: no line matching '$1'"; ok=0; }
}
case $mode in
ok)
    [ $st = 0 ] || { echo "verdict ok: the run failed"; ok=0; }
    ! grep -aq "reports a failed test" "$out/$name.out" ||
        { echo "verdict ok: the feed saw a failure line"; ok=0; } ;;
fail)
    [ $st != 0 ] || { echo "verdict fail: a failing utest passed the script"; ok=0; }
    want "serial-feed: the guest reports a failed test: .*utest: 0 passed, 1 FAILED"
    want "serial-feed: .*no 'vd-1' before the guest reported a failed test|serial-feed: .*stopped, the guest reported a failed test"
    want "$name: FAILED \(input script\)" ;;
crash)
    [ $st != 0 ] || { echo "verdict crash: a crashed utest passed the script"; ok=0; }
    ! grep -aq "reports a failed test" "$out/$name.out" ||
        { echo "verdict crash: it printed a failure line (the canary should have crashed)"; ok=0; }
    want "serial-feed: a utest began and gave no summary"
    want "$name: FAILED \(input script\)" ;;
esac
if [ $ok = 1 ]; then
    echo "verdict $mode: PASS"
    exit 0
fi
echo "verdict $mode: FAIL (see $out/$name.out and $out/$name.log)"
grep -a "serial-feed: " "$out/$name.out" | head -8 || true
exit 1
