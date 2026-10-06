#!/bin/sh
# The kernel's crash tests from the shell (`crash <name> yes`; they are not
# in the boot menu). Each one boots a plain "shell" system, runs
# the crash test from the shell, and must panic (bp must come back to the
# prompt instead). The panic starts the stored kernel: its boot's shell
# says what happened ("the last boot panicked: ..."), and `reboot -f` ends
# the run. kexecbad damages the stored kernel first, so its panic must
# halt on the panic screen instead; kexecstall and kexecfault break the jump
# after it was decided, and must end in a firmware reset (QEMU's
# -no-reboot ends the run there), not a dark hang. The boot words (testpf, ...) still run
# them at boot: tools/qemu-test.sh <outdir> <name> testpf.
# QEMU_SMP passes through (lockirq, stuck, watchdog and kexecstall need 2 CPUs).
# Usage: tools/crash-test.sh <outdir> [name ...]  (default: all of them);
# exit 0 if every one did what it must.
set -u
out=$1
shift
all="bp panic pf ro rohhdm stack lockorder lockself locknest lockirq mutexorder mutexspin stuck watchdog
     smap smep kexecread kexecbad kexecstall kexecfault"
names=${*:-$all}
mkdir -p "$out"
fails=0
for n in $names; do
    s="$out/crash-$n.txt"
    {
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "send crash $n yes"
        echo "wait crash $n: here goes"
        case $n in
        bp)
            echo "wait 30 came back, as a breakpoint must"
            echo "wait {prompt}"
            echo "send reboot -f"
            echo "wait reboot: resetting" ;;
        kexecbad)
            echo "wait 60 KERNEL PANIC"
            echo "wait 30 no restart: the stored kernel's checksum no longer matches"
            echo "wait 30 system halted" ;;
        kexecstall)
            echo "wait 60 KERNEL PANIC"
            echo "wait 30 didn't hand the jump over within 10 s: a firmware reboot instead"
            echo "wait 10 reboot: resetting" ;;
        kexecfault)
            echo "wait 60 KERNEL PANIC"
            echo "wait 30 a fault on the panicking CPU after it decided to start the stored kernel: a firmware reboot instead"
            echo "wait 10 reboot: resetting" ;;
        *)
            echo "wait 60 KERNEL PANIC"
            echo "wait 30 starting the stored kernel"
            echo "wait 60 loader:      Jam OS kexec"
            echo "wait 120 the last boot panicked: "
            echo "wait {prompt}"
            echo "send reboot -f"
            echo "wait reboot: resetting" ;;
        esac
    } > "$s"
    if QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_INPUT="$s" \
        tools/qemu-test.sh "$out" "crash-$n" shell > "$out/crash-$n.out" 2>&1; then
        why=$(grep -am1 -A2 "KERNEL PANIC" "$out/crash-$n.log" | tail -1 |
              sed "s/^\[[ 0-9.]*\] *//" | cut -c1-90)
        echo "crash $n: OK${why:+ ($why)}"
    else
        echo "crash $n: FAILED (see $out/crash-$n.log)"
        fails=$((fails + 1))
    fi
done
[ $fails -eq 0 ] && echo "crash tests: all passed" || echo "crash tests: $fails FAILED"
[ $fails -eq 0 ]
