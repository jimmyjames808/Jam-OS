#!/bin/sh
# Boot build/jamos.img headless in QEMU with a given kernel command line,
# wait until it halts, then save the serial log and a screenshot.
# QEMU_IMAGE picks another image (e.g. build/noktests/jamos.img).
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
    -device qemu-xhci,id=xhci \
    -drive if=none,id=usbstick,format=raw,file="$img" \
    -device usb-storage,bus=xhci.0,drive=usbstick,bootindex=0 \
    -device edu,dma_mask=0xffffffff \
    -serial file:"$log" -display none -no-reboot \
    -monitor unix:"$mon",server,nowait &
qpid=$!

i=0
limit=$(( ${QEMU_TIMEOUT:-30} * 2 ))
while [ $i -lt $limit ] && ! grep -qE "Halting|Idling|system halted" "$log" 2>/dev/null; do
    sleep 0.5
    i=$((i + 1))
done
sleep 0.3
echo "screendump $out/$name.ppm" | nc -U -w1 "$mon" >/dev/null || true
sleep 0.5
kill $qpid 2>/dev/null || true
wait $qpid 2>/dev/null || true
python3 -c "from PIL import Image; Image.open('$out/$name.ppm').save('$out/$name.png')" 2>/dev/null || true
rm -f "$img" "$out/$name.vars" "$out/$name.ppm" "$mon"
grep -qE "Halting|Idling|system halted" "$log" || { echo "$name: TIMEOUT"; exit 1; }
