#!/bin/sh
# Boot build/jamos.img headless in QEMU with a given kernel command line,
# wait until it halts, then save the serial log and a screenshot.
# QEMU_IMAGE picks another image (e.g. build/noktests/jamos.img).
# QEMU_XHCI adds qemu-xhci properties (e.g. "msi=on,msix=off": an MSI-only
# xHCI like many Intel PCH controllers).
# QEMU_INPUT=<script> types into the serial port (the shell tests): the
# serial port becomes a socket chardev (still logged to <name>.log) that
# tools/serial-feed.py drives with the script (its format is in that file).
# The run ends when QEMU does (e.g. the script's `reboot`: -no-reboot) or
# at QEMU_TIMEOUT; it passes if every `wait` in the script matched and QEMU
# ended by itself.
# QEMU_TIMEOUT: seconds before a run is given up (150: a cap, a run ends
# as soon as the kernel halts or QEMU goes; the `init` run and the full
# ktest take about 30 s each, more on a busy machine).
# QEMU_USB adds USB devices after the boot stick (which takes xhci.0 port
# 1), e.g. "-device usb-hub,bus=xhci.0,port=2 -device usb-kbd,bus=xhci.0,port=2.1"
# (give every device a port=, or QEMU picks the next free one).
# QEMU_EXTRA: more QEMU arguments, e.g. "-rtc base=2026-01-15T01:02:03" (the
# shell test sets the real-time clock with it).
# QEMU_MONITOR names a script run against the QEMU monitor while it boots,
# one command per line:
#     expect <text>    wait (up to the timeout) until the serial log has <text>
#     send <command>   a monitor command: sendkey a, device_del kbd2, ...
#     sleep <seconds>
# QEMU_SAVE=<file> keeps the run's stick image, with what the guest wrote
# to it, as <file> (for a later run's QEMU_IMAGE: a second boot of the
# same stick).
# The stick is the device "stick" on the block node "usbstick" (a
# -blockdev, which outlives the device): a script pulls it with the monitor
# command `device_del stick` and plugs it back with
# `device_add usb-storage,id=stick,bus=xhci.0,port=1,drive=usbstick`.
# The boot splash (a plain boot's animation) is left out with the boot
# word `nosplash`, so the tests see the text log as before; QEMU_SPLASH=1
# keeps it (tools/splash-test.sh).
# Usage: tools/qemu-test.sh <outdir> <name> [cmdline...]
set -eu
out=$1 name=$2
shift 2
cmdline="$*"
[ "${QEMU_SPLASH:-0}" = 1 ] || cmdline="$cmdline nosplash"
ovmf=$(brew --prefix qemu)/share/qemu
mkdir -p "$out"

img="$out/$name.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
{
    printf 'timeout: 0\n/test\n    protocol: limine\n    path: boot():/boot/jamos.elf\n'
    printf '    module_path: boot():/boot/bootfs.img\n    cmdline: %s\n' "$cmdline"
} > "$out/$name.conf"
mcopy -o -i "$img@@1M" "$out/$name.conf" ::/boot/limine/limine.conf
cp "$ovmf/edk2-i386-vars.fd" "$out/$name.vars"

log="$out/$name.log" mon="build/.qemu-$name.sock"   # unix socket paths max out at 104 bytes
ser="build/.qemu-$name.ser"
rm -f "$log" "$mon" "$ser"
if [ -n "${QEMU_INPUT:-}" ]; then
    serial="-chardev socket,id=ser0,path=$ser,server=on,wait=on,logfile=$log -serial chardev:ser0"
else
    serial="-serial file:$log"
fi
qemu-system-x86_64 -M q35 -m "${QEMU_MEM:-2G}" -smp "${QEMU_SMP:-4}" -cpu "${QEMU_CPU:-max}" \
    -drive if=pflash,format=raw,readonly=on,file="$ovmf/edk2-x86_64-code.fd" \
    -drive if=pflash,format=raw,file="$out/$name.vars" \
    -device qemu-xhci,id=xhci${QEMU_XHCI:+,$QEMU_XHCI} \
    -blockdev driver=file,node-name=stickfile,filename="$img" \
    -blockdev driver=raw,node-name=usbstick,file=stickfile \
    -device usb-storage,id=stick,bus=xhci.0,port=1,drive=usbstick,bootindex=0 \
    ${QEMU_USB:-} ${QEMU_EXTRA:-} \
    -device edu,dma_mask=0xffffffff \
    $serial -display none -no-reboot \
    -monitor unix:"$mon",server,nowait &
qpid=$!
fpid=
if [ -n "${QEMU_INPUT:-}" ]; then
    QEMU_MON="$mon" SHOT_DIR="$out" \
        python3 tools/serial-feed.py "$ser" "$QEMU_INPUT" 2> "$out/$name.feed" &
    fpid=$!
fi

limit=$(( ${QEMU_TIMEOUT:-150} * 2 ))
if [ -n "${QEMU_MONITOR:-}" ]; then
    (
        while read -r what arg; do
            case $what in
            expect)
                j=0
                while [ $j -lt $((limit * 3)) ] && ! grep -qF -- "$arg" "$log" 2>/dev/null; do
                    sleep 0.2
                    j=$((j + 1))
                done ;;
            send) echo "$arg" | nc -U -w1 "$mon" >/dev/null 2>&1 || true ;;
            sleep) sleep "$arg" ;;
            esac
        done < "$QEMU_MONITOR"
    ) &
    mpid=$!
fi

i=0
while [ $i -lt $limit ] && kill -0 $qpid 2>/dev/null &&
      ! grep -qE "Halting|Idling|system halted" "$log" 2>/dev/null; do
    sleep 0.5
    i=$((i + 1))
done
sleep 0.3
echo "screendump $out/$name.ppm" | nc -U -w1 "$mon" >/dev/null || true
sleep 0.5
kill $qpid 2>/dev/null || true
wait $qpid 2>/dev/null || true
if [ -n "${mpid:-}" ]; then
    kill $mpid 2>/dev/null || true
    wait $mpid 2>/dev/null || true
fi
python3 -c "from PIL import Image; Image.open('$out/$name.ppm').save('$out/$name.png')" \
    2>/dev/null || true
if [ -n "${QEMU_SAVE:-}" ]; then
    cp "$img" "$QEMU_SAVE"
fi
rm -f "$img" "$out/$name.vars" "$out/$name.ppm" "$mon" "$ser"
if [ -n "$fpid" ]; then
    fst=0
    wait $fpid || fst=$?
    cat "$out/$name.feed"
    [ $fst -eq 0 ] && [ $i -lt $limit ] || { echo "$name: FAILED (input script)"; exit 1; }
    exit 0
fi
grep -qE "Halting|Idling|system halted" "$log" || { echo "$name: TIMEOUT"; exit 1; }
