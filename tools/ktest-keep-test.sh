#!/bin/sh
# The kernel test runner's failure paths, with the tests that fail on
# purpose (kernel/test/test_ktest.c, "ktest=review_ktest"):
#   keep     both failures are recorded (one in the test's thread, one in
#            a helper thread), the test after them still passes, and the
#            boot ends with its RESULTS box saying FINISHED WITH PROBLEMS
#   no keep  the first failure panics, and the panic screen carries the
#            note with the loop, the seed and the test
# Usage: tools/ktest-keep-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
ok=1
want() {
    grep -aqE "$2" "$out/$1.log" || { echo "$1: no line matching '$2'"; ok=0; }
}
QEMU_TIMEOUT=${QEMU_TIMEOUT:-120} tools/qemu-test.sh "$out" ktkeep ktest=review_ktest keep seed=5 loops=2 || true
want ktkeep "ktest: FAILED review_ktest_fails_in_its_thread: 1 \+ 1 == 3 failed: 2 vs 3 \(kernel/test/test_ktest.c:[0-9]+\) \[loop 1, seed 5, test [1-3] of 3\]"
want ktkeep "ktest: FAILED review_ktest_fails_in_a_helper: arg != NULL failed .*\[loop 2, seed 6, test [1-3] of 3\]"
want ktkeep "ktest: review_ktest_passes_after_them +ok"
want ktkeep "ktest: 2 loop\(s\), keep, seed 5: 2 passed, 0 skipped, 4 FAILED"
want ktkeep "run FINISHED WITH PROBLEMS"
QEMU_TIMEOUT=${QEMU_TIMEOUT:-120} tools/qemu-test.sh "$out" ktpanic ktest=review_ktest seed=5 || true
want ktpanic "JAM OS KERNEL PANIC"
want ktpanic "ktest review_ktest_fails_in_[a-z_]+: .* failed"
want ktpanic "ktest: loop 1 of 1, seed 5, test [1-3] of 3: review_ktest_fails_in_[a-z_]+; before it: "
want ktpanic "system halted"
if [ $ok = 1 ]; then
    echo "ktest-keep: PASS"
    exit 0
fi
echo "ktest-keep: FAIL (see $out/ktkeep.log, $out/ktpanic.log)"
exit 1
