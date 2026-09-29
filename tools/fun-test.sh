#!/bin/sh
# The fun apps from the shell (bin/life, bin/tetris, bin/fractal): a plain
# boot ("shell"), tools/shell-tests/fun.txt runs each app's --selftest,
# then plays each for a few seconds over the serial terminal, quits it
# with q and checks the prompt comes back. Screenshots: <outdir>/fun-*.png.
# QEMU_SMP passes through. Usage: tools/fun-test.sh <outdir> [name]; exit 0
# on PASS.
set -eu
out=$1 name=${2:-fun}
mkdir -p "$out"
if QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/fun.txt \
   tools/qemu-test.sh "$out" "$name" shell; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $out/$name.log)"
exit 1
