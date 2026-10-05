#!/bin/sh
# The compositor on QEMU's framebuffer: the hidden boot word `comptest`
# makes init start bin/compositor alone with the screen, running its test
# scene (user/services/init/comptest.c: windows of known pixels with no
# client, title bars, a translucent window, the arrow, then a full-screen
# window). Each step is held on the screen for 4 s; QEMU's monitor takes a
# screenshot of each, and tools/comp-check.py checks them against the steps
# the log describes. Then the compositor's `bench:` lines are printed (its
# frame costs; QEMU's numbers only show the code runs, the PC's count).
# QEMU_SMP passes through.
# Usage: tools/comp-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-comp}
mkdir -p "$out"
abs=$(cd "$out" && pwd)
mon="$out/$name.mon"
: > "$mon"
for step in 1 2 3; do
    printf 'expect compositor: testscene: holding 4000 ms after paint %s\nsleep 1\n' "$step" >> "$mon"
    printf 'send screendump %s/%s-%s.ppm\n' "$abs" "$name" "$step" >> "$mon"
done

ok=1
rm -f "$out/$name"-[123].ppm
if ! QEMU_MONITOR="$mon" QEMU_TIMEOUT=${QEMU_TIMEOUT:-200} \
     tools/qemu-test.sh "$out" "$name" comptest > "$out/$name.out" 2>&1; then
    echo "$name: the boot FAILED (see $out/$name.log)"
    tail -3 "$out/$name.out"
    ok=0
fi
log="$out/$name.log"
if ! grep -aq "comptest: the compositor exited with code 0" "$log"; then
    echo "$name: the compositor didn't end well:"
    grep -a "comptest:\|compositor: testscene: can't\|compositor: can't" "$log" | tail -3
    ok=0
fi
python3 tools/comp-check.py "$log" "$out/$name" || ok=0
grep -a "compositor: bench:" "$log" | sed 's/^.*compositor: bench:/bench:/' || true
rm -f "$out/$name"-[123].ppm
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $log)"
exit 1
