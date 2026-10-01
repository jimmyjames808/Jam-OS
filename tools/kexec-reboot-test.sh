#!/bin/sh
# `reboot` by kexec (docs/M8.5-PLAN.md): init reads the kernel and boot
# image from /esp, loads them (kexec_load), syncs, flushes the log, stops
# devmgr's drivers in order and jumps (kexec_reboot): no firmware reset,
# so QEMU (-no-reboot) keeps running. Two runs:
#   kexec     a plain boot, `reboot`: the new kernel says it came from
#             kexec, arms its own crash kernel (from the kernel file the
#             old one handed it as a module), reaches the shell, which reads
#             /data (the old boot's log ends with the reboot's sync, the new
#             boot logs to boot-0002); then a panic in the new kernel is
#             saved by its crash kernel as boot-0002-crash.txt
#   fallback  crashkernel=0 (no region to load into): `reboot` falls back
#             to the firmware by itself; and `reboot -f` is the firmware
#             reset on a normal boot
# The new kernel starts every CPU itself (INIT-SIPI-SIPI: there is no
# loader to park them); QEMU_SMP and QEMU_XHCI pass through.
# Usage: tools/kexec-reboot-test.sh <outdir>; exit 0 on PASS.
set -u
out=$1
mkdir -p "$out"
fails=0

fail() {
    echo "kexec-reboot $1: FAILED: $2"
    fails=$((fails + 1))
}

run() {
    c=$1 cmdline=$2
    shift 2
    printf '%s\n' "$@" > "$out/kexec-$c.txt"
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-400} QEMU_SAVE="$out/kexec-$c-stick.img" \
        QEMU_INPUT="$out/kexec-$c.txt" tools/qemu-test.sh "$out" "kexec-$c" $cmdline \
        > "$out/kexec-$c.out" 2>&1
}

run kexec shell "wait 120 Jam OS shell" "wait jam>" \
    "wait 60 logd: writing /data/logs/boot-0001.txt" "send reboot" "wait 180 init: kexec: /esp/boot/jamos.elf and /esp/boot/bootfs.img" \
    "wait 30 init: /data synced" "wait 60 driver(s) stopped; exiting" \
    "wait 30 kexec: starting the loaded kernel" "wait 60 loader:      Jam OS kexec, cmdline" \
    "wait 60 kexec: crash kernel armed" "wait 120 init: the shell is up" "wait jam>" \
    "send sleep 5 && ls /data/logs && uname" "wait boot-0001.txt" "wait jam>" \
    "seen 30 logd: writing /data/logs/boot-0002.txt" \
    "send crash panic yes" "wait 60 KERNEL PANIC" \
    "wait 60 loader:      Jam OS kexec (crash kernel)" \
    "wait 120 crash: the crashed kernel's log is saved as /data/logs/boot-0002-crash.txt" \
    "wait 60 system halted" ||
    fail kexec "the script (see $out/kexec-kexec.log)"
log="$out/kexec-kexec.log"
grep -q "reboot: resetting" "$log" && fail kexec "a firmware reset happened"
cpus=${QEMU_SMP:-4}
[ "$(grep -c "smp: $cpus of $cpus CPUs online" "$log")" -ge 2 ] ||
    fail kexec "the kexec'd kernel didn't bring up all $cpus CPUs"
data="$out/kexec-kexec-stick.img@@64M"
mtype -i "$data" ::/logs/boot-0001.txt 2>/dev/null | grep -q "init: /data synced" ||
    fail kexec "boot-0001.txt doesn't end with the reboot's sync"
mtype -i "$data" ::/logs/boot-0002-crash.txt 2>/dev/null |
    grep -q "the end of the kernel log of boot-0002" ||
    fail kexec "boot-0002-crash.txt isn't the kexec'd kernel's log"

run fallback "shell crashkernel=0" "wait 120 Jam OS shell" "wait jam>" \
    "seen 60 init: /esp mounted" "send reboot" "wait 180 init: kexec: " \
    "wait 10 ERR_NOT_SUPPORTED" \
    "wait 10 init: rebooting through the firmware instead" "wait 30 reboot: resetting" ||
    fail fallback "the script (see $out/kexec-fallback.log)"
grep -q "kexec: starting" "$out/kexec-fallback.log" && fail fallback "it kexec'd anyway"

run firmware shell "wait 120 Jam OS shell" "wait jam>" "send reboot -f" \
    "wait 10 rebooting through the firmware" "wait 30 reboot: resetting" ||
    fail firmware "the script (see $out/kexec-firmware.log)"
grep -q "kexec_load" "$out/kexec-firmware.log" && fail firmware "reboot -f tried kexec"

rm -f "$out"/kexec-*-stick.img
if [ $fails -eq 0 ]; then
    echo "kexec-reboot: PASS"
    exit 0
fi
echo "kexec-reboot: FAIL ($fails)"
exit 1
