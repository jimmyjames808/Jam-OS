#!/bin/sh
# DHCP, DNS and the slow-peer rule in QEMU (docs/M9-PLAN.md stage 5): two
# plain boots with QEMU's e1000e and tools/netpeer.py (its DHCP and DNS
# servers), each passing the peer's and the pcap's VLAN checks
# (tools/qemu-test.sh):
#   dns         no net.address (tools/shell-tests/dns.txt): the lease (20 s,
#               so it is renewed during the run), `net`, `host`, `ping
#               <name>`, Ctrl+C on the slow name, `run dnstest` (while
#               slow.jam waits 10 s, 20 other names and 3 pings are each
#               answered in under 200 ms), then netstack killed: the same
#               address again by INIT-REBOOT, the restarted resolver
#               answers. Checked afterwards: one lease line per lease
#               (2: the boot's and after the kill), renewals ACKed (the
#               peer's counts), the slow name asked 4 times, never answered;
#   dns-static  net.address set (tools/shell-tests/dns-static.txt): no
#               DHCP client is started (no `dhcp:` line, not one DHCP
#               message at the peer), and `host` works with the settings'
#               DNS server.
# QEMU_SMP passes through. Usage: tools/dns-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
ok=1

peer_count() {
    python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get(sys.argv[2], -1))" \
        "$1" "$2" 2>/dev/null || echo -1
}

# --- DHCP and DNS ---
QEMU_TIMEOUT=${QEMU_TIMEOUT:-240} QEMU_NET=1 QEMU_NET_PEER="--dhcp-lease 20" \
    QEMU_INPUT=tools/shell-tests/dns.txt tools/qemu-test.sh "$out" dns shell || ok=0
leases=$(grep -c "dhcp: lease 10.2.21.100/24" "$out/dns.log" || true)
acks=$(peer_count "$out/dns.peer.json" dhcp_acks)
reqs=$(peer_count "$out/dns.peer.json" dhcp_requests)
slow=$(peer_count "$out/dns.peer.json" dns_slow)
answered=$(peer_count "$out/dns.peer.json" dns_answered)
echo "dns: $leases lease lines; the peer ACKed $acks of $reqs REQUESTs; slow.jam asked" \
     "$slow times; $answered answers"
[ "$leases" = 2 ] || { echo "dns: want 2 lease lines (the boot's and after the kill)"; ok=0; }
[ "$acks" -ge 3 ] || { echo "dns: want at least 3 ACKs (2 leases and a renewal)"; ok=0; }
[ "$slow" -ge 4 ] || { echo "dns: want slow.jam asked at least 4 times (dnstest's tries)"; ok=0; }
grep -q "dhcp: .*didn't" "$out/dns.log" && { echo "dns: netstack refused the lease"; ok=0; }

# --- a static address: no DHCP ---
img="$out/dns-static.base.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\n' > "$out/dns-static.settings"
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/dns-static.settings" ::/etc/settings
QEMU_IMAGE="$img" QEMU_NET=1 QEMU_INPUT=tools/shell-tests/dns-static.txt \
    tools/qemu-test.sh "$out" dns-static shell || ok=0
rm -f "$img"
dhcp=$(( $(peer_count "$out/dns-static.peer.json" dhcp_discovers) +
         $(peer_count "$out/dns-static.peer.json" dhcp_requests) ))
echo "dns-static: the peer saw $dhcp DHCP messages"
[ "$dhcp" = 0 ] || { echo "dns-static: DHCP messages with a static address"; ok=0; }
if grep -a "dhcp: " "$out/dns-static.log"; then
    echo "dns-static: the DHCP client ran with a static address"
    ok=0
fi

if [ $ok = 1 ]; then
    echo "dns-test: PASS"
    exit 0
fi
echo "dns-test: FAIL (logs in $out)"
exit 1
