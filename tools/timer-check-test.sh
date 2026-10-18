#!/bin/sh
# The boot's timer check (smp_report: every CPU's ticks over 1 s) measures
# again when a CPU is off under a hypervisor, up to three windows, the last
# one the verdict (a host that did not run a vCPU for most of a second is
# over by the next window; a wrong timer is wrong in every one). Two
# `selftest` boots (a mode that runs the check at once):
#   plain       "timer: ... all ok" (on the first window or, on a loaded
#               PC that really stalled one, a later one)
#   timerstall  the boot CPU takes no interrupt for 600 ms of the first
#               window: "timer: window 1: 1 CPU(s) off ... measuring
#               again", then "all ok (window 2)"
# Usage: tools/timer-check-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
ok=1
want() {
    grep -aqE "$2" "$out/$1.log" || { echo "$1: no line matching '$2'"; ok=0; }
}
QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} tools/qemu-test.sh "$out" tcplain selftest ||
    { echo "tcplain: the boot failed"; ok=0; }
want tcplain "timer: .* all ok"
QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} tools/qemu-test.sh "$out" tcstall "selftest timerstall" ||
    { echo "tcstall: the boot failed"; ok=0; }
want tcstall "timer: window 1: 1 CPU\(s\) off, [0-9.]+ ms \(the host did not run them\?\): measuring again"
want tcstall "timer: .* all ok \(window [23]\)"
if [ $ok = 1 ]; then
    echo "timer-check: PASS (a window with a CPU off is measured again)"
    exit 0
fi
echo "timer-check: FAIL (see $out/tcplain.log, $out/tcstall.log)"
exit 1
