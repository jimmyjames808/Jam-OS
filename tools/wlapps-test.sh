#!/bin/sh
# libfun's apps as windows on a compositor (tools/shell-tests/wlapps.txt):
# a plain boot ("shell"); `run wlapps` four times, each starting a headless
# compositor of its own and apps on it (user/tests/wlapps), with a
# screenshot of each composition (<outdir>/wlapps-*.png, shown on the
# borrowed screen). PASS when every verdict is PASS; SKIP (exit 0) when
# the compositor offers no windows yet (no xdg_wm_base); FAIL otherwise.
# QEMU_SMP passes through.
# Usage: tools/wlapps-test.sh <outdir> [name]; exit 0 on PASS or SKIP.
set -eu
out=$1 name=${2:-wlapps}
mkdir -p "$out"
ok=1
QEMU_TIMEOUT=${QEMU_TIMEOUT:-400} QEMU_INPUT=tools/shell-tests/wlapps.txt \
    tools/qemu-test.sh "$out" "$name" shell > "$out/$name.out" 2>&1 || ok=0
log="$out/$name.log"
grep -a "wlapps: verdict:\|wlapps: [0-9]* pixels\|libfun: no window" "$log" | \
    sed 's/^.*\] //' || true
pass=$(grep -ac "wlapps: verdict: PASS" "$log" || true)
skip=$(grep -ac "wlapps: verdict: SKIP" "$log" || true)
if [ $ok = 1 ] && [ "$pass" = 4 ]; then
    echo "$name: PASS"
    exit 0
fi
if [ $ok = 1 ] && [ "$skip" = 4 ]; then
    echo "$name: SKIP (the compositor offers no windows yet)"
    exit 0
fi
echo "$name: FAIL (see $log)"
exit 1
