#!/bin/sh
# The quiet boot (docs/G1-PLAN.md, the polish track): the first terminal
# starts at the shell's banner and prompt, with no kernel or service log
# line on its screen, and the log stays in `dmesg`, /data/logs and the
# serial port. Three plain boots on the compositor (the default), each
# running tools/shell-tests/quiet.txt, whose screenshot of the first
# terminal tools/quiet-check.py reads:
#   boot      `nosplash` (as every shell script boots): text on the first
#             two rows of the terminal (the banner and the prompt) and
#             nowhere else, though the boot's late lines (usb-bus's, the
#             timer check's) come while it is up;
#   splash    the everyday boot (QEMU_SPLASH=1): the same after the splash;
#   verbose   the boot word `verbose` (Developer > "Jam OS (text log, no
#             splash)"): the log on the terminal's screen as it comes, ten
#             rows of text at least.
# Each boot's serial log must have the kernel's and the services' lines all
# the same (the log is not lost, only off the screen). QEMU_SMP passes
# through. Usage: tools/quiet-test.sh <outdir>; exit 0 on PASS.
set -u
out=$1
mkdir -p "$out"
ok=1
for b in boot splash verbose; do
    sed "s/{name}/$b/" tools/shell-tests/quiet.txt > "$out/quiet-$b.txt"
    splash=0 words=shell
    [ $b = splash ] && splash=1
    [ $b = verbose ] && words="shell verbose"
    if ! QEMU_SPLASH=$splash QEMU_TIMEOUT=${QEMU_TIMEOUT:-200} QEMU_INPUT="$out/quiet-$b.txt" \
         tools/qemu-test.sh "$out" "quiet-$b-run" $words > "$out/quiet-$b.out" 2>&1; then
        echo "quiet-test: $b: the shell script FAILED (see $out/quiet-$b-run.log)"
        tail -3 "$out/quiet-$b.out"
        ok=0
        continue
    fi
    for line in "usb-bus: 1 device" "init: the shell is up" "devmgr: disk 1 partition 2"; do
        grep -aq "$line" "$out/quiet-$b-run.log" ||
            { echo "quiet-test: $b: \"$line\" is not in the serial log"; ok=0; }
    done
    if [ $b = verbose ]; then
        python3 tools/quiet-check.py "$out/quiet-$b.png" --min-rows 10 || ok=0
    else
        python3 tools/quiet-check.py "$out/quiet-$b.png" --rows 2 || ok=0
    fi
done
if [ $ok = 1 ]; then
    echo "quiet-test: PASS"
    exit 0
fi
echo "quiet-test: FAIL"
exit 1
