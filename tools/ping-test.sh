#!/bin/sh
# The shell's `net` and `ping` in QEMU (tools/shell-tests/net.txt): the
# e1000e driver, netstack with net.address from the stick's settings, a
# program's pings through /svc/net, answered by tools/netpeer.py (ICMP echo
# for any address, 1.1.1.1 through the gateway). The run must pass the
# script, the peer's and the pcap's VLAN checks (tools/qemu-test.sh), and
# the peer must have answered at least ECHO_MIN of the guest's echoes.
# Usage: tools/ping-test.sh <outdir>
set -eu
out=$1
mkdir -p "$out"
ECHO_MIN=5   # ping 1.1.1.1 -c 3, then at least 2 to the Mac before Ctrl+C
img="$out/ping.base.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\n' > "$out/ping.settings"
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/ping.settings" ::/etc/settings
ok=1
QEMU_IMAGE="$img" QEMU_NET=1 QEMU_INPUT=tools/shell-tests/net.txt \
    tools/qemu-test.sh "$out" ping shell || ok=0
answered=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['echo_replies'])" \
               "$out/ping.peer.json" 2>/dev/null || echo 0)
echo "ping-test: the peer answered $answered echo requests"
[ "$answered" -ge $ECHO_MIN ] || ok=0
if [ $ok = 1 ]; then
    echo "ping-test: PASS"
else
    echo "ping-test: FAIL"
    exit 1
fi
