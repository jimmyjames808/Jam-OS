#!/bin/sh
# The receive path over a long run in QEMU (tools/shell-tests/rxsoak.txt):
# the e1000e driver, netstack with net.address from the stick's settings,
# and tools/netpeer.py flooding the guest for about three minutes
# (--flood: FLOOD frames a second, the mix of a busy trunk port, on VLAN
# 21 and off it) while it pings the guest every 0.2 s. Thousands of
# frames wrap the driver's 256 descriptors and the 256-slot netdev rx ring
# many times over (the PC's receive stopped after ~75 s on its trunk:
# docs/M9-PLAN.md "R1: receive on the PC"). The run must pass the script
# (the guest's own pings answered at the end), the peer's and the pcap's
# VLAN checks, and the peer's pings sent LATE seconds or more after the
# first must still be answered (at least 90% of them).
# Usage: tools/rxsoak-test.sh <outdir>
set -eu
out=$1
mkdir -p "$out"
FLOOD=${RXSOAK_FLOOD:-60}
LATE=150
FLOOD_MIN=6000
img="$out/rxsoak.base.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\n' > "$out/rxsoak.settings"
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/rxsoak.settings" ::/etc/settings
ok=1
QEMU_IMAGE="$img" QEMU_NET=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-360} \
    QEMU_NET_PEER="--ping 10.2.21.5 --ping-every 0.2 --flood $FLOOD --late-after $LATE" \
    QEMU_INPUT=tools/shell-tests/rxsoak.txt tools/qemu-test.sh "$out" rxsoak shell || ok=0
set -- $(python3 -c "
import json, sys
s = json.load(open(sys.argv[1]))
print(s.get('flood_sent', 0), s.get('flood_vlan', 0), s['pings'], s['ping_replies'],
      s.get('pings_late', 0), s.get('ping_replies_late', 0))" "$out/rxsoak.peer.json" \
    2>/dev/null || echo 0 0 0 0 0 0)
echo "rxsoak-test: $1 frames flooded ($2 on VLAN 21); $4 of $3 pings answered, $6 of the" \
     "$5 sent $LATE s or more after the first"
[ "$1" -ge $FLOOD_MIN ] || { echo "rxsoak-test: too few frames flooded"; ok=0; }
if [ "$5" -lt 50 ] || [ $(($6 * 10)) -lt $(($5 * 9)) ]; then
    echo "rxsoak-test: the late pings were not answered"
    ok=0
fi
grep -E "rx so far|netstack: rx" "$out/rxsoak.log" | tail -4 || true
if [ $ok = 1 ]; then
    echo "rxsoak-test: PASS"
else
    echo "rxsoak-test: FAIL"
    exit 1
fi
