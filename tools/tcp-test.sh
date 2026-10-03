#!/bin/sh
# TCP end to end in QEMU: bin/tcptest in the guest against the network
# peer's TCP side (tools/netpeer.py --tcp-serve and --tcp-connect, which
# tools/tcppeer.py does), through e1000e on VLAN 21
# (tools/shell-tests/tcp.txt): 10 MiB each way on a connection the guest
# opens; 8 MiB each way on one with 2 MiB rings to a server whose ACKs
# come 20 ms late (a round trip a scaled window must cover); then 20
# connections at once to a guest listener served from one wait set, 256
# KiB each way each. Every byte is checked on both sides; the run must
# pass the peer's and the pcap's VLAN checks (tools/qemu-test.sh), and the
# peer must count all 22 connections right and none wrong. Window scaling:
# on the 2 MiB rings' connection the peer must see the scaled window, a
# window over 64 KiB, and more than 64 KiB in flight both ways; on the
# default rings' one, no window and no flight over 64 KiB.
# Usage: tools/tcp-test.sh <outdir>
set -eu
out=$1
mkdir -p "$out"
img="$out/tcp.base.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\n' > "$out/tcp.settings"
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/tcp.settings" ::/etc/settings
ok=1
serve="5030:10485760,5032:8388608:20"   # the second: ACKs 20 ms late
QEMU_IMAGE="$img" QEMU_NET=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-900} \
    QEMU_NET_PEER="--tcp-serve $serve --tcp-connect 10.2.21.5:5031:20:262144" \
    QEMU_INPUT=tools/shell-tests/tcp.txt tools/qemu-test.sh "$out" tcp shell || ok=0
count() {
    python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get(sys.argv[2], 0))" \
        "$out/tcp.peer.json" "$1" 2>/dev/null || echo 0
}
good=$(count tcp_ok)
bad=$(count tcp_bad)
echo "tcp-test: the peer counted $good connections right, $bad wrong;" \
     "$(count tcp_bytes_in) bytes in, $(count tcp_bytes_out) out, $(count tcp_retransmits) resent"
grep "tcptest: PASS" "$out/tcp.log" | sed 's/^.*tcptest/tcp-test: tcptest/' || true
for p in 5030 5032; do
    echo "tcp-test: port $p: $(count tcp_${p}_scaled) scaled; the guest's window up to" \
         "$(count tcp_${p}_window), in flight up to $(count tcp_${p}_in_flight) from it and" \
         "$(count tcp_${p}_out_flight) to it"
done
[ "$good" = 22 ] && [ "$bad" = 0 ] || ok=0
# the bulk rings: scaled, and used past an unscaled window both ways
[ "$(count tcp_5032_scaled)" = 1 ] && [ "$(count tcp_5032_window)" -gt 65535 ] &&
    [ "$(count tcp_5032_in_flight)" -gt 65535 ] && [ "$(count tcp_5032_out_flight)" -gt 65535 ] ||
    { echo "tcp-test: the 2 MiB rings' connection didn't use a scaled window"; ok=0; }
# the default rings: scaled too, but never past 64 KiB
[ "$(count tcp_5030_window)" -le 65536 ] && [ "$(count tcp_5030_in_flight)" -le 65536 ] ||
    { echo "tcp-test: the default rings' connection went past 64 KiB"; ok=0; }
if [ $ok = 1 ]; then
    echo "tcp-test: PASS"
else
    echo "tcp-test: FAIL"
    exit 1
fi
