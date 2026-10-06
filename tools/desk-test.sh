#!/bin/sh
# The desktop's plumbing (track D2b, docs/G1-PLAN.md "As built: D2b") end
# to end: one plain boot ("shell") with a usb-kbd on xhci port 2, a
# usb-mouse on port 3 and QEMU's e1000e with the network peer
# (QEMU_NET=1, untagged), running tools/shell-tests/desk.txt: netstack's
# "Connected" notice with the peer's DHCP lease; `notify -w` answered by a
# click on its card's button; `launch` refusing what isn't a desktop app;
# the search box starting Jamjar through init (the busy cursor until its
# window) and running a command in a new terminal. Screenshots in
# <outdir>/desk-*.png. The clicks are at OVMF's 1280x800.
# QEMU_SMP passes through. Usage: tools/desk-test.sh <outdir> [name]; exit 0
# on PASS.
set -eu
out=$1 name=${2:-desk}
mkdir -p "$out"
usb="-device usb-kbd,bus=xhci.0,port=2 -device usb-mouse,bus=xhci.0,port=3"
ok=1
QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/desk.txt QEMU_USB="$usb" \
    QEMU_NET=1 QEMU_NET_VLAN=none tools/qemu-test.sh "$out" "$name" shell \
    > "$out/$name.out" 2>&1 ||
    { echo "$name: the script failed"; tail -3 "$out/$name.out"; ok=0; }
log="$out/$name.log"
# The busy cursor went with each window (not by its 10 s limit), and only
# desktop apps were started.
[ "$(grep -ac "the cursor is not busy" "$log")" -eq 2 ] ||
    { echo "$name: the busy cursor didn't end on the windows"; ok=0; }
grep -aq "init: launch fractal: OK" "$log" && { echo "$name: init launched fractal"; ok=0; }
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $log)"
exit 1
