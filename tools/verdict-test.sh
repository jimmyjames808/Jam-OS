#!/bin/sh
# The shell tests' verdicts (tools/serial-feed.py: a failure line from a
# test program fails the script, whatever it waits for). One boot runs
# tools/shell-tests/verdict.txt, whose `utest only verdict_canary` the
# shell has told, by UTEST_CANARY, to behave as MODE:
#   ok      passes: the run passes (the control)
#   fail    a check fails: the run FAILS, naming the check's line (the
#           script's own waits would all have matched)
#   quiet   the same without the check's line (the kernel rate-limits what
#           a program prints, and a busy run drops some): the run FAILS on
#           the RESULTS lines, "utest: FAILED tests: verdict_canary" and the
#           summary
#   crash   utest faults and gives no summary: the run FAILS and says a
#           utest began and gave none
# Usage: tools/verdict-test.sh <outdir> ok|fail|quiet|crash; exit 0 on PASS.
set -eu
usage="usage: tools/verdict-test.sh <outdir> ok|fail|quiet|crash"
out=${1:?$usage}
mode=${2:?$usage}
case $mode in ok | fail | quiet | crash) ;; *) echo "verdict: unknown mode '$mode'"; exit 2 ;; esac
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
refuse() {
    ! grep -aqE -- "$1" "$out/$name.out" || { echo "verdict $mode: $2"; ok=0; }
}
case $mode in
ok)
    [ $st = 0 ] || { echo "verdict ok: the run failed"; ok=0; }
    refuse "reports a failed test|gave no summary" "the feed saw a failure" ;;
fail)
    [ $st != 0 ] || { echo "verdict fail: a failing utest passed the script"; ok=0; }
    want "serial-feed: the guest reports a failed test: .*utest: verdict_canary: FAILED at line"
    want "serial-feed: .*(no 'vd-1' before the guest reported a failed test|stopped, the guest reported a failed test)"
    want "$name: FAILED \(input script\)" ;;
quiet)
    [ $st != 0 ] || { echo "verdict quiet: a failing utest passed the script"; ok=0; }
    refuse "FAILED at line" "the check printed its line (the canary should be silent)"
    want "serial-feed: the guest reports a failed test: .*(utest: FAILED tests: verdict_canary|utest: 0 passed, 1 FAILED)"
    want "$name: FAILED \(input script\)" ;;
crash)
    [ $st != 0 ] || { echo "verdict crash: a crashed utest passed the script"; ok=0; }
    refuse "reports a failed test" "it printed a failure line (the canary should have crashed)"
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
