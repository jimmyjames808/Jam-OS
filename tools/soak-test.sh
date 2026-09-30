#!/bin/sh
# The soak test in QEMU, short: one shell boot that runs
# `soak loops=<n> seed=<s>` (tools/shell-tests/soak-plug.txt): the kernel
# tests <n> times, each time in another shuffled order, with the kernel's
# load (the stress workers) and bin/soakload's (files on /data and on a
# second, writable stick; memory; channel calls; programs) running, utest
# between the loops, and meanwhile the second stick and then the boot stick
# pulled and plugged back through QEMU's monitor. One more shuffled loop
# without load runs before all that and one after it. (The second stick is a
# -blockdev node, like the boot stick: a -drive would be deleted with its
# device at the first pull, and the device_add after it would find nothing.)
#
#   SOAK_LOOPS  loops (default 3)
#   SOAK_SEED   the first loop's seed (default 1000 + the CPU count); a
#               failure's line in SOAK RESULTS says how to replay its loop
#   SOAK_LOAD   kernel load workers (default one per CPU: under TCG every
#               worker costs the tests a host core; the PC runs two per CPU)
#   QEMU_SMP    CPUs (default 4); run it at 4 and at 8
#   QEMU_XHCI   passes through
#
# About 3 minutes at 4 CPUs on a quiet Mac; slower next to other QEMUs.
# Usage: tools/soak-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-soak}
mkdir -p "$out"
loops=${SOAK_LOOPS:-3}
seed=${SOAK_SEED:-$((1000 + ${QEMU_SMP:-4}))}
load=${SOAK_LOAD:-${QEMU_SMP:-4}}
s2="$out/$name-s2.img"
script="$out/$name.txt"
tmp="$out/$name-files"
mkdir -p "$tmp"
echo "a file on the second stick" > "$tmp/second.txt"
python3 -c "import random, sys; sys.stdout.buffer.write(random.Random(3).randbytes(200 << 10))" \
    > "$tmp/noise.bin"
python3 tools/mkstick.py "$s2" 64 0c
mformat -i "$s2@@1M" -T $((63 * 2048)) -F -v SOAK2 ::
mcopy -i "$s2@@1M" "$tmp/second.txt" "$tmp/noise.bin" ::/
sed -e "s/@LOOPS@/$loops/" -e "s/@SEED@/$seed/" -e "s/@LOAD@/$load/" tools/shell-tests/soak-plug.txt > "$script"

ok=1
if ! QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} QEMU_INPUT="$script" \
     QEMU_USB="-blockdev driver=file,node-name=s2file,filename=$s2 -blockdev driver=raw,node-name=s2img,file=s2file" \
     tools/qemu-test.sh "$out" "$name" shell; then
    echo "$name: the shell script FAILED (see $out/$name.log)"
    ok=0
fi
log="$out/$name.log"
want() {
    grep -aqE "$1" "$log" || { echo "$name: no line matching '$1'"; ok=0; }
}
never() {
    ! grep -aqE "$1" "$log" || { echo "$name: a line matching '$1':"; grep -aE "$1" "$log" | head -5; ok=0; }
}
want "soak: PASSED after"
# an idle loop before the load and one after it: loops + 2 in all
want "kernel tests: $((loops + 2)) loop\(s\), seeds $seed to $((seed + loops + 1)): [0-9]+ passed, [0-9]+ skipped, 0 FAILED"
want "utest between the loops: $loops run\(s\), 0 FAILED"
want "file load: [1-9][0-9]* cycle\(s\) written, read back and compared, 0 FAILED"
# Channel messages nobody reads must not pile up: a program's namespace
# notices once did (logd's grew by ~20 KB per utest run). What stays queued
# at the end is a few messages in flight.
bytes=$(grep -aE "jobs: message bytes" "$log" | tail -1 | sed -E 's/.*\((-?[0-9]+)\).*/\1/')
if [ -z "$bytes" ] || [ "$bytes" -gt 32768 ]; then
    echo "$name: the job tree's message bytes grew by ${bytes:-?} (more than 32768)"
    ok=0
fi
never "KERNEL PANIC"
never "ktest: FAILED"
never "stress: FAILED"
never "soakload: FAILED"
# the second stick still holds its own files, whatever was cut short on it
mtype -i "$s2@@1M" ::/second.txt 2>/dev/null | grep -q "a file on the second stick" ||
    { echo "$name: the second stick lost second.txt"; ok=0; }
mcopy -n -i "$s2@@1M" ::/noise.bin "$tmp/noise.back" 2>/dev/null && cmp -s "$tmp/noise.bin" "$tmp/noise.back" ||
    { echo "$name: the second stick's noise.bin changed"; ok=0; }
rm -rf "$tmp" "$script"
grep -aE "soak: (PASSED|FAILED) after|kernel tests:|FAILED [a-z(]|utest between|file load:|soak: user load|jobs: " "$log" |
    sed -e 's/^\[[ 0-9.]*\] *//' || true
if [ $ok = 1 ]; then
    rm -f "$s2"
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $log)"
exit 1
