#!/bin/sh
# Input early in boot: how soon the keyboard and mouse work, and that a
# slow device holds up nobody but itself. A plain boot ("shell", no
# splash) with
#   qemu-xhci port 1   the boot stick (usb-bus's root port 1, USB 3)
#   qemu-xhci port 2   a usb-hub (root port 6): 6.1 a keyboard, 6.2 a
#                      usb-storage disk, 6.3 a slow keyboard
#   qemu-xhci port 3   a usb-mouse (root port 7)
#   qemu-xhci port 4   a slow keyboard (root port 8)
# The slow ones have the serial number jamos-test-slow, which usb-bus
# treats as a device that doesn't answer (drivers/usb-bus/attach.c,
# test_slow): its first two attempts on a port each take a second and
# fail, so its port is tried again after 100 ms and then 200 ms, and it
# attaches on the third. Checked (tools/shell-tests/usb-early.txt): the
# console's line "input ready: the first keyboard and mouse N s after the
# kernel started" with N under EARLY_MAX (default 1.5 s; QEMU's kernel
# takes ~0.3 s to reach user space); that line before either slow
# device attached; both slow ports' two failed attempts and their retries
# at 100 and 200 ms, then attached, their keyboards ready too; `usb` lists
# all seven devices. It prints the times.
# QEMU_SMP passes through. Usage: tools/usb-early-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1 name=usb-early
mkdir -p "$out"
early_max=${EARLY_MAX:-1.5}
disk="$out/$name-disk.img"
dd if=/dev/zero of="$disk" bs=1m count=4 2>/dev/null
QEMU_TIMEOUT=${QEMU_TIMEOUT:-120} QEMU_INPUT=tools/shell-tests/usb-early.txt \
QEMU_USB="-device usb-hub,id=hub1,bus=xhci.0,port=2 \
-device usb-kbd,bus=xhci.0,port=2.1 \
-drive if=none,id=d2,format=raw,file=$disk -device usb-storage,bus=xhci.0,port=2.2,drive=d2 \
-device usb-kbd,bus=xhci.0,port=2.3,serial=jamos-test-slow \
-device usb-mouse,bus=xhci.0,port=3 \
-device usb-kbd,bus=xhci.0,port=4,serial=jamos-test-slow" \
    tools/qemu-test.sh "$out" "$name" shell > "$out/$name.out" 2>&1 ||
    { echo "$name: the script failed"; tail -3 "$out/$name.out"; }
log="$out/$name.log"
ok=1
need() {
    grep -aqF -- "$1" "$log" || { echo "$name: no line with \"$1\""; ok=0; }
}
at() {   # the uptime (s) of the first log line with this text
    grep -aF -- "$1" "$log" | head -1 | sed -n 's/^\[ *\([0-9.]*\)\].*/\1/p'
}
for p in 8 6.3; do
    need "usb $p: attempt 1 failed: trying again in 100 ms"
    need "usb $p: attempt 2 failed: trying again in 200 ms"
    need "usb $p: 0627:0001"
done
need "usb-bus: 7 devices (1 hub)"
ready=$(grep -aE "input ready: the first keyboard and mouse [0-9.]+ s" "$log" | head -1 |
        sed -n 's/.*mouse \([0-9.]*\) s after.*/\1/p')
t_slow8=$(at "usb 8: 0627:0001 high-speed") t_slow63=$(at "usb 6.3: 0627:0001 full-speed")
t_ready=$(at "console: input ready: the first keyboard and mouse")
n=$(grep -ac "console: keyboard 0627:0001 ready" "$log" || true)
[ "$n" -ge 3 ] || { echo "$name: $n keyboards ready, want 3 (6.1 and the two slow ones)"; ok=0; }
python3 - "$early_max" "${ready:-99}" "${t_ready:-99}" "${t_slow8:-0}" "${t_slow63:-0}" <<'PY' || ok=0
import sys
early_max, ready, t_ready, slow8, slow63 = map(float, sys.argv[1:])
bad = False
if ready > early_max:
    print(f"usb-early: the first keyboard and mouse ready at {ready} s, want under {early_max} s")
    bad = True
if not (t_ready < slow8 and t_ready < slow63):
    print(f"usb-early: input ready at {t_ready} s, not before the slow devices ({slow8}, {slow63})")
    bad = True
sys.exit(1 if bad else 0)
PY
echo "$name: the first keyboard and mouse ready ${ready:-?} s after the kernel started;" \
     "the slow devices attached at ${t_slow8:-?} s (port 8) and ${t_slow63:-?} s (6.3)"
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $log)"
exit 1
