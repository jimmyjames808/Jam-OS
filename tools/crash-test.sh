#!/bin/sh
# The kernel's crash tests from the shell (`crash <name> yes`; they are not
# in the boot menu). Each one boots a plain "shell" system, runs
# the crash test from the shell, and must end on the panic screen (bp must
# come back to the prompt instead). The boot words (testpf, ...) still run
# them at boot: tools/qemu-test.sh <outdir> <name> testpf.
# QEMU_SMP passes through (lockirq, stuck and watchdog need 2 CPUs).
# Usage: tools/crash-test.sh <outdir> [name ...]  (default: all of them);
# exit 0 if every one did what it must.
set -u
out=$1
shift
all="bp panic pf ro rohhdm stack lockorder locknest lockirq mutexorder mutexspin stuck watchdog
     smap smep kexecread kexecbad"
names=${*:-$all}
mkdir -p "$out"
fails=0
for n in $names; do
    s="$out/crash-$n.txt"
    {
        echo "wait 120 Jam OS shell"
        echo "wait jam>"
        echo "send crash $n yes"
        echo "wait crash $n: here goes"
        if [ "$n" = bp ]; then
            echo "wait 30 came back, as a breakpoint must"
            echo "wait jam>"
            echo "send reboot -f"
            echo "wait reboot: resetting"
        else
            echo "wait 60 KERNEL PANIC"
            echo "wait 30 system halted"
        fi
    } > "$s"
    if QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_INPUT="$s" \
        tools/qemu-test.sh "$out" "crash-$n" shell > "$out/crash-$n.out" 2>&1; then
        why=$(grep -m1 -A2 "KERNEL PANIC" "$out/crash-$n.log" | tail -1 |
              sed "s/^\[[ 0-9.]*\] *//" | cut -c1-90)
        echo "crash $n: OK${why:+ ($why)}"
    else
        echo "crash $n: FAILED (see $out/crash-$n.log)"
        fails=$((fails + 1))
    fi
done
[ $fails -eq 0 ] && echo "crash tests: all passed" || echo "crash tests: $fails FAILED"
[ $fails -eq 0 ]
