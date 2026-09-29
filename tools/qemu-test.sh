#!/bin/sh
# Boot build/jamos.img headless in QEMU with a given kernel command line,
# wait until it halts, then save the serial log and a screenshot.
# QEMU_IMAGE picks another image (e.g. build/noktests/jamos.img).
# QEMU_XHCI adds qemu-xhci properties (e.g. "msi=on,msix=off": an MSI-only
# xHCI like many Intel PCH controllers).
# QEMU_USB adds USB devices after the boot stick (which takes xhci.0 port
# 1), e.g. "-device usb-hub,bus=xhci.0,port=2 -device usb-kbd,bus=xhci.0,port=2.1"
# (give every device a port=, or QEMU picks the next free one).
# QEMU_MONITOR names a script run against the QEMU monitor while it boots,
# one command per line:
#     expect <text>    wait (up to the timeout) until the serial log has <text>
#     send <command>   a monitor command: sendkey a, device_del kbd2, ...
#     sleep <seconds>
# Usage: tools/qemu-test.sh <outdir> <name> [cmdline...]
set -eu
out=$1 name=$2
shift 2
cmdline="$*"
ovmf=$(brew --prefix qemu)/share/qemu
mkdir -p "$out"

img="$out/$name.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
printf 'timeout: 0\n/test\n    protocol: limine\n    path: boot():/boot/jamos.elf\n    module_path: boot():/boot/bootfs.img\n    cmdline: %s\n' \
    "$cmdline" > "$out/$name.conf"
mcopy -o -i "$img@@1M" "$out/$name.conf" ::/boot/limine/limine.conf
cp "$ovmf/edk2-i386-vars.fd" "$out/$name.vars"

log="$out/$name.log" mon="build/.qemu-$name.sock"   # unix socket paths max out at 104 bytes
rm -f "$log" "$mon"
qemu-system-x86_64 -M q35 -m "${QEMU_MEM:-2G}" -smp "${QEMU_SMP:-4}" -cpu "${QEMU_CPU:-max}" \
    -drive if=pflash,format=raw,readonly=on,file="$ovmf/edk2-x86_64-code.fd" \
    -drive if=pflash,format=raw,file="$out/$name.vars" \
    -device qemu-xhci,id=xhci${QEMU_XHCI:+,$QEMU_XHCI} \
    -drive if=none,id=usbstick,format=raw,file="$img" \
    -device usb-storage,bus=xhci.0,drive=usbstick,bootindex=0 \
    ${QEMU_USB:-} \
    -device edu,dma_mask=0xffffffff \
    -serial file:"$log" -display none -no-reboot \
    -monitor unix:"$mon",server,nowait &
qpid=$!

limit=$(( ${QEMU_TIMEOUT:-30} * 2 ))
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
while [ $i -lt $limit ] && ! grep -qE "Halting|Idling|system halted" "$log" 2>/dev/null; do
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
python3 -c "from PIL import Image; Image.open('$out/$name.ppm').save('$out/$name.png')" 2>/dev/null || true
rm -f "$img" "$out/$name.vars" "$out/$name.ppm" "$mon"
grep -qE "Halting|Idling|system halted" "$log" || { echo "$name: TIMEOUT"; exit 1; }
