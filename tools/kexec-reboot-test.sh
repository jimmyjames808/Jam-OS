#!/bin/sh
# `reboot` by kexec (docs/M8.5-PLAN.md, "Revision 2"): the screen goes to
# the splash background at once, init syncs, flushes the log, stops
# devmgr's drivers in order and starts the kernel's stored copy of the
# system: no firmware reset, so QEMU (-no-reboot) keeps running. Four runs:
#   kexec     a plain boot with the splash, `reboot` with the stick
#             unchanged: no file is read ("the stored kernel, no files
#             read"), the screen is all the splash background as the next
#             kernel starts (no text), the new kernel
#             says it was started by a reboot, plays the splash, brings up
#             every CPU and reaches the shell, which reads /data (the old
#             boot's log ends with the reboot's sync, the new one logs to
#             boot-0002); no banner about a panic
#   changed   the stick swapped (the monitor) for a copy whose
#             /esp/boot/jamos.elf is 4 KiB longer, as if flashed on the
#             Mac: `reboot` reads both files and kexec_loads them first
#             (the screen meanwhile all the splash background: the shell
#             blanked the console), then the new kernel comes up
#   load      the stick swapped the same way, then `kernel load`: both
#             files read and kexec_loaded at once; the `reboot` after it
#             reads nothing ("/esp unchanged") and starts them
#   firmware  `reboot -f`: the firmware reset, no kexec at all
#   fallback  crashkernel=0 (no stored kernel): `kernel load` says there is
#             none, and `reboot` falls back to the firmware by itself
# The kexec run has a sound card (intel-hda, as on the PC): the mixer
# holds a channel to the hda driver, and a reboot that left it running
# waited devmgr's whole stop timeout (30 s on the PC); devmgr must stop
# within 2 s. The new kernel starts every CPU itself (INIT-SIPI-SIPI:
# there is no loader to park them); QEMU_SMP and QEMU_XHCI pass through.
# Usage: tools/kexec-reboot-test.sh <outdir> [run ...]; exit 0 on PASS.
set -u
out=$1
shift
runs=${*:-kexec changed load firmware fallback}
mkdir -p "$out"
fails=0
cpus=${QEMU_SMP:-4}

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

run_kexec() {
    QEMU_EXTRA="${QEMU_EXTRA:--audiodev none,id=snd0 -device intel-hda,id=hda0 \
-device hda-output,bus=hda0.0,cad=0,audiodev=snd0}" QEMU_SPLASH=1 run kexec shell "wait 180 init: the shell is up" "wait jam>" \
        "seen 30 console: the screen is back" \
        "seen 60 logd: writing /data/logs/boot-0001.txt" \
        "seen 60 init: kexec: the stored kernel came from /esp/boot/jamos.elf" \
        "send reboot" \
        "wait 30 init: kexec: /esp unchanged: the stored kernel, no files read" \
        "wait 30 init: /data synced" "wait 60 driver(s) stopped; exiting" \
        "wait 30 kexec: starting the stored kernel" \
        "wait 60 loader:      Jam OS kexec" "shot kexec-between" \
        "wait 30 kexec: started by a reboot" \
        "wait 60 kexec: stored kernel armed" "wait 120 splash: first frame" \
        "wait 180 init: the shell is up" "wait jam>" \
        "send sleep 5 && ls /data/logs && uname" "wait boot-0001.txt" "wait jam>" \
        "seen 30 logd: writing /data/logs/boot-0002.txt" \
        "send reboot -f" "wait reboot: resetting" ||
        { fail kexec "the script (see $out/kexec-kexec.log)"; return; }
    log="$out/kexec-kexec.log"
    [ "$(grep -ac "reboot: resetting" "$log")" -eq 1 ] ||
        fail kexec "a firmware reset happened before the last one"
    grep -aq "kexec_load\|reading /esp" "$log" && fail kexec "a file was read"
    grep -aq "the last boot panicked" "$log" && fail kexec "a reboot was taken for a panic"
    sed -n '/kexec: \/esp unchanged/,/kexec: starting the stored kernel/p' "$log" |
        grep -aq "screen: back to the kernel's log" &&
        fail kexec "the kernel took the screen back during the reboot (its log flashes on the PC)"
    ms=$(grep -ao "devmgr stopped in [0-9]* ms" "$log" | head -1 | tr -dc 0-9)
    [ -n "$ms" ] && [ "$ms" -lt 2000 ] ||
        fail kexec "devmgr took ${ms:-?} ms to stop (a driver left waiting for its clients?)"
    [ "$(grep -ac "smp: $cpus of $cpus CPUs online" "$log")" -ge 2 ] ||
        fail kexec "the kexec'd kernel didn't bring up all $cpus CPUs"
    mtype -i "$out/kexec-kexec-stick.img@@64M" ::/logs/boot-0001.txt 2>/dev/null |
        grep -q "init: /data synced" || fail kexec "boot-0001.txt doesn't end with the reboot's sync"
    python3 tools/splash-check.py quiet "$out/kexec-between.png" ||
        fail kexec "the screen between the kernels isn't all the splash background"
}

run_changed() {
    stick2="$out/kexec-changed-stick2.img"
    cp build/jamos.img "$stick2"
    cp build/jamos.elf "$out/jamos-longer.elf"
    head -c 4096 /dev/zero >> "$out/jamos-longer.elf"
    mcopy -o -i "$stick2@@1M" "$out/jamos-longer.elf" ::/boot/jamos.elf ||
        { fail changed "can't write the second stick's kernel"; return; }
    run changed shell "wait 120 Jam OS shell" "wait jam>" \
        "seen 60 init: kexec: the stored kernel came from /esp/boot/jamos.elf" \
        "monitor device_del stick" "wait 30 init: /esp is gone" \
        "monitor drive_add 0 if=none,id=stick2,file=$stick2,format=raw" \
        "monitor device_add usb-storage,id=stick,bus=xhci.0,port=1,drive=stick2" \
        "wait 60 init: /esp mounted" "sleep 1" "send reboot" \
        "wait 30 init: kexec: /esp's kernel or boot image changed: reading" \
        "sleep 2" "shot kexec-blank" \
        "wait 120 kexec_load: OK" "wait 60 kexec: starting the stored kernel" \
        "wait 60 loader:      Jam OS kexec" "wait 120 init: the shell is up" "wait jam>" \
        "send reboot -f" "wait reboot: resetting" ||
        fail changed "the script (see $out/kexec-changed.log)"
    [ "$(grep -ac "reboot: resetting" "$out/kexec-changed.log")" -eq 1 ] ||
        fail changed "a firmware reset happened before the last one"
    python3 tools/splash-check.py quiet "$out/kexec-blank.png" ||
        fail changed "the screen while reboot reads the files isn't all the splash background"
    rm -f "$stick2" "$out/jamos-longer.elf"
}

run_load() {
    stick2="$out/kexec-load-stick2.img"
    cp build/jamos.img "$stick2"
    cp build/jamos.elf "$out/jamos-longer.elf"
    head -c 4096 /dev/zero >> "$out/jamos-longer.elf"
    mcopy -o -i "$stick2@@1M" "$out/jamos-longer.elf" ::/boot/jamos.elf ||
        { fail load "can't write the second stick's kernel"; return; }
    run load shell "wait 120 Jam OS shell" "wait jam>" \
        "seen 60 init: kexec: the stored kernel came from /esp/boot/jamos.elf" \
        "monitor device_del stick" "wait 30 init: /esp is gone" \
        "monitor drive_add 0 if=none,id=stick2,file=$stick2,format=raw" \
        "monitor device_add usb-storage,id=stick,bus=xhci.0,port=1,drive=stick2" \
        "wait 60 init: /esp mounted" "sleep 1" "send kernel load" \
        "wait 120 kexec_load: OK" "wait 30 kernel: loaded" "wait jam>" \
        "send reboot" "wait 30 init: kexec: /esp unchanged: the stored kernel, no files read" \
        "wait 60 kexec: starting the stored kernel" \
        "wait 60 loader:      Jam OS kexec" "wait 120 init: the shell is up" "wait jam>" \
        "send reboot -f" "wait reboot: resetting" ||
        fail load "the script (see $out/kexec-load.log)"
    log="$out/kexec-load.log"
    [ "$(grep -ac "kexec_load: " "$log")" -eq 1 ] ||
        fail load "the files were read and loaded more than once"
    [ "$(grep -ac "reboot: resetting" "$log")" -eq 1 ] ||
        fail load "a firmware reset happened before the last one"
    rm -f "$stick2" "$out/jamos-longer.elf"
}

run_firmware() {
    run firmware shell "wait 120 Jam OS shell" "wait jam>" "send reboot -f" \
        "wait 10 rebooting through the firmware" "wait 30 reboot: resetting" ||
        fail firmware "the script (see $out/kexec-firmware.log)"
    grep -aq "kexec_load\|kexec: starting\|init: kexec:.*stored kernel," "$out/kexec-firmware.log" &&
        fail firmware "reboot -f tried kexec"
}

run_fallback() {
    run fallback "shell crashkernel=0" "wait 120 Jam OS shell" "wait jam>" \
        "seen 60 init: /esp mounted" "send kernel load" \
        "wait 120 kernel: not loaded: there is no stored kernel" "wait jam>" \
        "send reboot" "wait 60 init: kexec: " \
        "wait 60 the jump failed (ERR_NOT_SUPPORTED)" \
        "wait 10 init: rebooting through the firmware instead" "wait 30 reboot: resetting" ||
        fail fallback "the script (see $out/kexec-fallback.log)"
    grep -aq "kexec: starting" "$out/kexec-fallback.log" && fail fallback "it kexec'd anyway"
}

for r in $runs; do
    had=$fails
    "run_$r"
    [ $fails -eq $had ] && echo "kexec-reboot $r: OK"
    rm -f "$out/kexec-$r-stick.img"
done
if [ $fails -eq 0 ]; then
    echo "kexec-reboot: PASS"
    exit 0
fi
echo "kexec-reboot: FAIL ($fails)"
exit 1
