#!/bin/sh
# The fun apps from the shell (bin/life, bin/tetris, bin/fractal): a plain
# boot ("shell"), tools/shell-tests/fun.txt runs each app's --selftest,
# then plays each over the serial terminal, taking screenshots of the
# screen it borrowed (console.lend_screen), quits it with q and checks the
# prompt comes back; then kills one (Ctrl+C) and crashes one while they
# hold the screen, and checks the console takes it back. Screenshots:
# <outdir>/fun-*.png. QEMU_SMP passes through; FUN_HD=1 runs at the PC's
# 2560x1440 (QEMU's VGA with that mode) instead of OVMF's 1280x800.
# Usage: tools/fun-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-fun}
mkdir -p "$out"
if [ "${FUN_HD:-}" = 1 ]; then
    QEMU_EXTRA="${QEMU_EXTRA:-} -vga none -device VGA,xres=2560,yres=1440,vgamem_mb=64"
    export QEMU_EXTRA
fi
if QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} QEMU_INPUT=tools/shell-tests/fun.txt \
   tools/qemu-test.sh "$out" "$name" shell; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $out/$name.log)"
exit 1
