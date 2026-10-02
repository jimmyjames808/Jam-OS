#!/bin/sh
# netstack end to end in QEMU: the e1000e driver, netstack on its netdev
# rings, init's net.address, and tools/netpeer.py pinging the guest on
# VLAN 21 (tools/shell-tests/netstack.txt). A copy of the image gets
# /data/etc/settings with net.address; the run must pass the peer's and
# the pcap's VLAN checks (tools/qemu-test.sh) and at least PINGS_MIN of the
# peer's pings must have been answered, across a netstack restart.
# Usage: tools/netstack-test.sh <outdir>
set -eu
out=$1
mkdir -p "$out"
PINGS_MIN=8
img="$out/netstack.base.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\n' > "$out/netstack.settings"
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/netstack.settings" ::/etc/settings
ok=1
QEMU_IMAGE="$img" QEMU_NET=1 QEMU_NET_PEER="--ping 10.2.21.5" \
    QEMU_INPUT=tools/shell-tests/netstack.txt tools/qemu-test.sh "$out" netstack shell || ok=0
answered=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['ping_replies'])" \
               "$out/netstack.peer.json" 2>/dev/null || echo 0)
echo "netstack-test: $answered of the peer's pings answered"
[ "$answered" -ge $PINGS_MIN ] || ok=0
grep -q "netstack: on " "$out/netstack.log" || { echo "netstack-test: no session"; ok=0; }
if [ $ok = 1 ]; then
    echo "netstack-test: PASS"
else
    echo "netstack-test: FAIL"
    exit 1
fi
