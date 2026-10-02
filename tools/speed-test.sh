#!/bin/sh
# The throughput tester end to end in QEMU (tools/shell-tests/speed.txt):
# bin/speed in the guest against tools/speed.py on the Mac, joined by the
# network peer's relay (TCP and UDP 10.2.21.174:5201 to speed.py's server;
# a Mac port to the guest's 5202 for speed.py's client), through e1000e on
# VLAN 21: TCP out, TCP in, UDP out, then `speed -l` with the Mac's client
# sending and receiving. Both sides' lines are printed (QEMU's numbers,
# through a relay written in Python: not the PC's). The run must pass the
# peer's and the pcap's VLAN checks (tools/qemu-test.sh).
# Usage: tools/speed-test.sh <outdir>
set -eu
out=$1
mkdir -p "$out"
img="$out/speed.base.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\n' > "$out/speed.settings"
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/speed.settings" ::/etc/settings
set -- $(python3 tools/netpeer.py --free-ports 2)
hport=$1 fport=$2
python3 tools/speed.py server --port "$hport" > "$out/speed.server.log" 2>&1 &
spid=$!
log="$out/speed.log"
rm -f "$log" "$out/speed.client.log"
# The Mac's client, once the guest listens.
(
    j=0
    while [ $j -lt 1500 ] && ! grep -q "speed: listening on" "$log" 2>/dev/null; do
        sleep 0.2
        j=$((j + 1))
    done
    python3 tools/speed.py client 127.0.0.1 --port "$fport" -t 3 || echo "speed.py: FAILED"
    python3 tools/speed.py client 127.0.0.1 --port "$fport" -r -t 3 || echo "speed.py: FAILED"
) > "$out/speed.client.log" 2>&1 &
cpid=$!
trap 'kill $spid $cpid 2>/dev/null || true' EXIT
ok=1
QEMU_IMAGE="$img" QEMU_NET=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} \
    QEMU_NET_PEER="--tcp-relay 5201:$hport --udp-relay 5201:$hport --tcp-forward $fport:10.2.21.5:5202" \
    QEMU_INPUT=tools/shell-tests/speed.txt tools/qemu-test.sh "$out" speed shell || ok=0
wait $cpid 2>/dev/null || true
grep -a "^speed: \(sent\|received\|the \|[0-9]* datagrams\)" "$log" | tr -d '\r' |
    sed 's/^speed:/speed-test: guest: speed:/' || true
sed 's/^/speed-test: the Mac (server): /' "$out/speed.server.log" | grep -v waiting || true
sed 's/^/speed-test: the Mac (client): /' "$out/speed.client.log" || true
grep -q "FAILED" "$out/speed.client.log" && ok=0
[ "$(grep -c "the other side got" "$out/speed.client.log")" -ge 1 ] || ok=0
if [ $ok = 1 ]; then
    echo "speed-test: PASS"
else
    echo "speed-test: FAIL"
    exit 1
fi
