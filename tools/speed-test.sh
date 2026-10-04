#!/bin/sh
# The throughput tester end to end in QEMU (tools/shell-tests/speed.txt):
# bin/speed in the guest against tools/speed.py on the Mac, joined by the
# network peer's relay (TCP and UDP 10.2.21.174:5201 to speed.py's server;
# a Mac port to the guest's 5202 for speed.py's client), through e1000e on
# VLAN 21: TCP out, TCP in, UDP out, then `speed -l` with the Mac's client
# sending and receiving, and sending again through a second Mac port whose
# relay drops SPEED_LOSS percent (default 2) of the segments with bytes it
# sends the guest, as lossy Wi-Fi does: the guest keeps what comes past
# each hole and says so with SACK blocks, so the relay resends little more
# than it dropped (checked: at most 3 times the bytes dropped). Both sides'
# lines are printed (QEMU's numbers, through a relay written in Python: not
# the PC's), and the relay's counts for the lossy run. The run must pass
# the peer's and the pcap's VLAN checks (tools/qemu-test.sh).
# Usage: tools/speed-test.sh <outdir>
set -eu
out=$1
mkdir -p "$out"
img="$out/speed.base.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\n' > "$out/speed.settings"
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/speed.settings" ::/etc/settings
loss=${SPEED_LOSS:-2}
set -- $(python3 tools/netpeer.py --free-ports 3)
hport=$1 fport=$2 lport=$3
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
    python3 tools/speed.py client 127.0.0.1 --port "$lport" -t 3 > "$out/speed.lossy.log" 2>&1 ||
        echo "speed.py (lossy): FAILED"
) > "$out/speed.client.log" 2>&1 &
cpid=$!
trap 'kill $spid $cpid 2>/dev/null || true' EXIT
ok=1
QEMU_IMAGE="$img" QEMU_NET=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} \
    QEMU_NET_PEER="--tcp-relay 5201:$hport --udp-relay 5201:$hport --tcp-forward $fport:10.2.21.5:5202,$lport:10.2.21.5:5202:$loss" \
    QEMU_INPUT=tools/shell-tests/speed.txt tools/qemu-test.sh "$out" speed shell || ok=0
wait $cpid 2>/dev/null || true
grep -a "^speed: \(sent\|received\|the \|[0-9]* datagrams\)" "$log" | tr -d '\r' |
    sed 's/^speed:/speed-test: guest: speed:/' || true
sed 's/^/speed-test: the Mac (server): /' "$out/speed.server.log" | grep -v waiting || true
sed 's/^/speed-test: the Mac (client): /' "$out/speed.client.log" || true
sed "s/^/speed-test: the Mac (client, $loss% lost): /" "$out/speed.lossy.log" 2>/dev/null || true
count() {
    python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get(sys.argv[2], 0))" \
        "$out/speed.peer.json" "$1" 2>/dev/null || echo 0
}
lost=$(count relay_lost_bytes)
resent=$(count relay_resent_bytes)
echo "speed-test: lossy run: the relay dropped $(count relay_lost) segments ($lost bytes)," \
     "resent $resent bytes; $(count relay_sack) connections with SACK, $(count relay_sack_acks)" \
     "ACKs with SACK blocks; $(count relay_fast_retransmits) fast recoveries," \
     "$(count relay_go_back) go-backs, $(count relay_retransmits) timeouts"
grep -q "FAILED" "$out/speed.client.log" && ok=0
[ "$(grep -c "the other side got" "$out/speed.client.log")" -ge 1 ] || ok=0
grep -q "the other side got" "$out/speed.lossy.log" 2>/dev/null || ok=0
if [ "$loss" != 0 ]; then
    [ "$lost" -gt 0 ] && [ "$(count relay_sack_acks)" -gt 0 ] && [ "$resent" -le $((3 * lost)) ] ||
        { echo "speed-test: the lossy run resent more than it lost (or saw no SACK)"; ok=0; }
fi
if [ $ok = 1 ]; then
    echo "speed-test: PASS"
else
    echo "speed-test: FAIL"
    exit 1
fi
