#!/bin/sh
# `reboot -f`, the reset through the firmware (kernel/dev/reboot.c,
# user/services/init/ctl.c): QEMU runs with -no-reboot, so a reset that
# works ends QEMU, and a run passes only if QEMU ended by itself before
# QEMU_TIMEOUT (a machine left hanging fails). Runs:
#   data    a plain boot with /data mounted and logd writing, a file
#           written, `reboot -f`: init syncs /data, flushes the log and
#           stops devmgr's drivers in order, the kernel halts the other
#           CPUs, turns bus mastering off and tries the ACPI reset
#           register (QEMU's q35: 0xCF9 = 0x0f), which ends QEMU before
#           any other method is tried. Then a second boot of the same
#           stick: the file is there, /data was left clean (no "dirty"),
#           logd writes boot-0002, and boot-0001.txt has the sync's line
#           (on the Mac, with mtools)
#   cf9     reset=cf9: 0xCF9's full reset (0x02, then 0x0e) alone does it
#   8042    reset=8042: the keyboard controller's pulse reset does it
#   triple  reset=triple: the triple fault does it
# Usage: tools/reboot-firmware-test.sh <outdir> [run ...]; exit 0 on PASS.
set -u
out=$1
shift
runs=${*:-data cf9 8042 triple}
mkdir -p "$out"
fails=0

fail() {
    echo "reboot-firmware $1: FAILED: $2"
    fails=$((fails + 1))
}

run() {
    c=$1 cmdline=$2
    shift 2
    printf '%s\n' "$@" > "$out/rbf-$c.txt"
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-120} QEMU_INPUT="$out/rbf-$c.txt" \
        tools/qemu-test.sh "$out" "rbf-$c" $cmdline > "$out/rbf-$c.out" 2>&1
}

# The kernel's lines after init's, in order, before a method's.
asked="wait 30 reboot: asked by init"
resetting="wait 30 reboot: resetting: "
quiet="wait 30 other CPU(s) halted, bus mastering off on"

run_data() {
    stick="$out/rbf-data-stick.img"
    QEMU_SAVE="$stick" run data shell "wait 120 Jam OS shell" "wait {prompt}" \
        "seen 60 init: /data mounted" "seen 60 logd: writing /data/logs/boot-0001.txt" \
        "send T=k && write /data/rbf.txt written before the reset" "wait {prompt}" \
        "send reboot -f" "wait 30 rebooting through the firmware" \
        "wait 30 init: /data synced in" "wait 60 init: firmware reset: devmgr stopped in" \
        "$asked" "$resetting" "$quiet" \
        "wait 30 reboot: trying the ACPI reset register (io 0xcf9 = 0xf)" ||
        { fail data "the script (see $out/rbf-data.log)"; return; }
    log="$out/rbf-data.log"
    grep -aq "reboot: trying 0xCF9\|reboot: trying the 8042\|reboot: trying a triple" "$log" &&
        fail data "the ACPI reset register didn't reset the machine"
    grep -aq "driver(s) stopped; exiting" "$log" || fail data "devmgr's drivers weren't stopped"
    grep -aq "devmgr didn't stop in order" "$log" && fail data "devmgr had to be killed"
    mtype -i "$stick@@64M" ::/logs/boot-0001.txt 2>/dev/null | grep -q "init: /data synced" || fail data "boot-0001.txt doesn't have the reset's sync"
    QEMU_IMAGE="$stick" run data2 shell "wait 120 Jam OS shell" "wait {prompt}" \
        "seen 60 init: /data mounted" "seen 60 logd: writing /data/logs/boot-0002.txt" \
        "send T=k && cat /data/rbf.txt && echo ok-\$T" "wait written before the reset" \
        "wait ok-k" "send reboot -f" "wait 30 reboot: trying the ACPI reset register" ||
        fail data "the second boot (see $out/rbf-data2.log)"
    grep -aq "fat /data: the volume is dirty" "$out/rbf-data2.log" &&
        fail data "/data wasn't left clean"
    rm -f "$stick"
}

# A method alone: reset=<word> starts the list there; the next method's
# line must never come.
run_method() {
    word=$1 line=$2 next=$3
    run "$word" "shell reset=$word" "wait 120 Jam OS shell" "wait {prompt}" "send reboot -f" \
        "wait 60 init: firmware reset: devmgr stopped in" \
        "$asked" "$resetting" "$quiet" "wait 30 $line" ||
        { fail "$word" "the script (see $out/rbf-$word.log)"; return; }
    grep -aq "reboot: resetting: reset=$word: " "$out/rbf-$word.log" ||
        fail "$word" "the reset line doesn't name the boot word"
    [ -z "$next" ] || ! grep -aq "$next" "$out/rbf-$word.log" ||
        fail "$word" "it didn't reset the machine (the next method was tried)"
}

run_cf9() {
    run_method cf9 "reboot: trying 0xCF9's full reset (0x02, then 0x0e" "reboot: trying the 8042"
}

run_8042() {
    run_method 8042 "reboot: trying the 8042 keyboard controller's reset" "reboot: trying a triple"
}

run_triple() {
    run_method triple "reboot: trying a triple fault, the last way" ""
}

for r in $runs; do
    had=$fails
    "run_$r"
    [ $fails -eq $had ] && echo "reboot-firmware $r: OK"
done
if [ $fails -eq 0 ]; then
    echo "reboot-firmware: PASS"
    exit 0
fi
echo "reboot-firmware: FAIL ($fails)"
exit 1
