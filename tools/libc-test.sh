#!/bin/sh
# libc-test's functional and regression sets on Jam OS (docs/M13-PLAN.md,
# track P2): a plain boot to the shell, which runs usr/bin/libc-test
# (ports/libc-test/: every test as a child of its own, the results
# against expected-failures.txt) as tools/shell-tests/libc-test.txt says.
# Passes when the program ends with "libc-test: DONE ok": every failure on
# the list, none off it, none of the list's tests passing. Prints the
# lines that are not PASS or XFAIL, and the counts.
#
# Usage: tools/libc-test.sh <outdir> [name] [test or prefix ...]
# Tests: with names (functional/strtod, regression) it runs only those.
# Log: <outdir>/<name>.log. Run it on the PC (docs/TESTING.md):
#   JAMOS_REMOTE=jamos-pc tools/remote-test.sh tools/libc-test.sh build/test/lct
set -eu
out=$1 name=${2:-libc-test}
if [ $# -ge 2 ]; then shift 2; else shift $#; fi
mkdir -p "$out"
script="$out/$name.txt"
sed "s|^send libc-test\$|send libc-test $*|" tools/shell-tests/libc-test.txt > "$script"
run=0
QEMU_TIMEOUT=${QEMU_TIMEOUT:-1500} QEMU_INPUT="$script" tools/qemu-test.sh "$out" "$name" shell || run=1
log="$out/$name.log"
grep -a -E '^(FAIL|XPASS|SKIP|    \| |libc-test:)' "$log" | tr -d '\r' || true
if [ $run = 0 ] && grep -aq '^libc-test: DONE ok' "$log"; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $log)"
exit 1
