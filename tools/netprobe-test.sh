#!/bin/sh
# The RTL8125 driver's boot words in QEMU (which has no RTL8125; the probe
# and the send test themselves run on the PC only: docs/M9-PLAN.md). Three
# boots, one after the other:
#   1. `shell netprobe` (tools/shell-tests/netprobe.txt): devmgr gets the
#      word and says so, nothing is bound for it, no [rtl8125] line, the
#      shell works;
#   2. `shell netsend` (tools/shell-tests/netsend.txt): the same for the
#      send test's word;
#   3. a plain `shell` boot: no netprobe or netsend line and no rtl8125
#      anywhere in the log: without a word nothing of the driver runs.
# The driver itself without a VLAN or without its hardware is utest's
# rtl8125_stays_off; its transmit-register guard and tx.c's gate are
# rtl8125_write_guard and rtl8125_tx_gate, and tools/checknotx.sh (make
# check) checks that only tx.c, behind the gate, can transmit.
# QEMU_SMP passes through. Usage: tools/netprobe-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
ok=1

QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_INPUT=tools/shell-tests/netprobe.txt \
    tools/qemu-test.sh "$out" netprobe shell netprobe || ok=0
log=$out/netprobe.log
if grep -q -e '-> drv/rtl8125' -e '\[rtl8125\]' "$log"; then
    echo "netprobe-test: the netprobe boot bound or ran drv/rtl8125 without an RTL8125:"
    grep -e 'rtl8125' "$log"
    ok=0
fi

QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_INPUT=tools/shell-tests/netsend.txt \
    tools/qemu-test.sh "$out" netsend shell netsend || ok=0
log=$out/netsend.log
if grep -q -e '-> drv/rtl8125' -e '\[rtl8125\]' "$log"; then
    echo "netprobe-test: the netsend boot bound or ran drv/rtl8125 without an RTL8125:"
    grep -e 'rtl8125' "$log"
    ok=0
fi

off=$out/netprobe-off.txt
printf 'wait 120 Jam OS shell\nwait jam>\nsend reboot -f\nwait reboot: resetting\n' > "$off"
QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_INPUT=$off \
    tools/qemu-test.sh "$out" netprobe-off shell || ok=0
log=$out/netprobe-off.log
if grep -q -i -e 'netprobe' -e 'netsend' -e 'rtl8125' "$log"; then
    echo "netprobe-test: a plain boot mentions the network driver:"
    grep -i -e 'netprobe' -e 'netsend' -e 'rtl8125' "$log"
    ok=0
fi

if [ "$ok" = 1 ]; then
    echo "netprobe-test: PASS"
    exit 0
fi
echo "netprobe-test: FAIL (logs in $out)"
exit 1
