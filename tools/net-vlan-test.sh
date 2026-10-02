#!/bin/sh
# The VLAN rule over every path that transmits (docs/M9-PLAN.md stage 8,
# "the join"): every frame Jam OS sends is tagged 802.1Q VLAN 21, and with
# `vlan=off` none is sent at all. Two plain boots with QEMU's e1000e and
# tools/netpeer.py (its DHCP and DNS servers, netlog's receiver
# (--netlog), the update server serving this build (--update), an SNTP
# server with the Mac's time (--ntp), an echo request to the guest every
# half second (--ping 10.2.21.100) and the frames the driver must drop
# (--noise)); only net.host = 10.2.21.174 in the stick's settings, so the
# address comes by DHCP:
#   net-vlan      tools/shell-tests/net-vlan.txt: the DHCP lease, netlog
#                 sending this boot's log, `host` and `ping <name>`, `ping`
#                 by address through the gateway, `update -n` fetching the
#                 build (checked by init, nothing loaded), sntp setting
#                 the clock from the gateway, netstack answering ARP and
#                 the peer's pings. The peer and the pcap
#                 each check every frame from the guest is tagged 21
#                 (tools/qemu-test.sh); then the pcap is read again here
#                 and its frames counted per path (ARP, DHCP, DNS, echo
#                 requests, echo replies, netlog, update, SNTP): every path must
#                 have sent at least one frame, none may be unknown, and
#                 the peer must have seen exactly the pcap's frames;
#   net-vlan-off  tools/shell-tests/net-vlan-off.txt: the same commands on
#                 a `vlan=off` boot (QEMU_NET_NONE: the peer's
#                 --expect-none and the pcap's): not one frame; the driver
#                 says "no VLAN: the network stays off" and never touches
#                 the chip, netstack finds no card, and each command says
#                 why it can't (no address, no answer).
# QEMU_SMP passes through. Usage: tools/net-vlan-test.sh <outdir> (after
# `make -s image`); exit 0 on PASS.
set -u
out=$1
mkdir -p "$out"
ok=1

fail() {
    echo "net-vlan-test: FAILED: $1"
    ok=0
}

want() {    # want <run> <text>: a line of the run's serial log has <text>
    grep -aqF -- "$2" "$out/$1.log" || fail "$1: no line with \"$2\""
}

img="$out/net-vlan.base.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
printf 'net.host = 10.2.21.174\n' > "$out/net-vlan.settings"
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/net-vlan.settings" ::/etc/settings ||
    { echo "net-vlan-test: can't write the stick's settings"; exit 1; }
cat > "$out/net-vlan.spec.json" <<EOF
{"kernel": "build/jamos.elf", "bootfs": "build/bootfs.img", "plan": []}
EOF

for run in net-vlan net-vlan-off; do
    words=shell none=0
    [ $run = net-vlan-off ] && words="shell vlan=off" none=1
    rm -rf "$out/$run.netlog"
    QEMU_IMAGE="$img" QEMU_NET=1 QEMU_NET_NONE=$none \
        QEMU_NET_PEER="--netlog $out/$run.netlog --update $out/net-vlan.spec.json --ntp $(date +%s) --ping 10.2.21.100 --noise 5" \
        QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/$run.txt \
        tools/qemu-test.sh "$out" $run $words > "$out/$run.out" 2>&1 ||
        fail "$run: the script or the VLAN checks (see $out/$run.out, $out/$run.log)"
    grep -aE "^(netpeer|pcap-vlan-check):" "$out/$run.out" | sed "s/^/$run: /"
done
rm -f "$img"

# --- net-vlan: the frames of each path ---
python3 - "$out" <<'EOF' || ok=0
import json, os, struct, sys
out = sys.argv[1]
sys.path.insert(0, "tools")
import importlib
pcv = importlib.import_module("pcap-vlan-check")

PATHS = ["arp", "dhcp", "dns", "echo request", "echo reply", "netlog", "update", "sntp"]
UDP = {67: "dhcp", 53: "dns", 5021: "netlog", 5022: "update", 123: "sntp"}


def path(f):
    """Which path sent frame f (tagged 21: the checks above), or why it is
    none of them."""
    if len(f) < 18 or struct.unpack_from("!HH", f, 12) != (0x8100, 21):
        return "NOT TAGGED 21"
    et, p = struct.unpack_from("!H", f, 16)[0], f[18:]
    if et == 0x0806:
        return "arp"
    if et != 0x0800 or len(p) < 20:
        return "unknown (EtherType %04x)" % et
    hl, proto = (p[0] & 15) * 4, p[9]
    if proto == 1 and len(p) > hl:
        return {8: "echo request", 0: "echo reply"}.get(p[hl], "icmp type %d" % p[hl])
    if proto == 17 and len(p) >= hl + 8:
        dport = struct.unpack_from("!H", p, hl + 2)[0]
        return UDP.get(dport, "udp to port %d" % dport)
    return "unknown (IP protocol %d)" % proto


counts = {k: 0 for k in PATHS}
fs = pcv.frames(os.path.join(out, "net-vlan.pcap"))
for f in fs:
    k = path(f)
    counts[k] = counts.get(k, 0) + 1
peer = json.load(open(os.path.join(out, "net-vlan.peer.json")))
print("net-vlan: frames per path: " + ", ".join("%s %d" % kv for kv in counts.items()) +
      " (%d in all; the peer got %d, %d bad)" % (len(fs), peer["frames"], peer["bad"]))
fails = [k for k in PATHS if not counts[k]]
fails += [k for k in counts if k not in PATHS]
if fails:
    print("net-vlan: FAILED: a path sent nothing, or frames from no known path: " +
          ", ".join(fails))
if peer["frames"] != len(fs) or peer["bad"]:
    print("net-vlan: FAILED: the peer and the pcap don't agree")
    fails.append("peer")
print("net-vlan: the peer answered %d of its %d pings, %d netlog datagram(s) in" %
      (peer["ping_replies"], peer["pings"], peer.get("netlog_in", 0)))
sys.exit(1 if fails else 0)
EOF
log=$out/net-vlan.log
grep -aq "update: .* -> .*: checked by init in .* not loaded (-n)" "$log" ||
    fail "net-vlan: update -n wasn't checked by init"
[ "$(grep -ac "kexec: kexec_load from init" "$log")" -eq 0 ] ||
    fail "net-vlan: update -n loaded a build"
ls "$out/net-vlan.netlog/"*.txt > /dev/null 2>&1 || fail "net-vlan: no log file from netlog"
want net-vlan "off: set from the gateway (10.2.21.1, stratum 2"

# --- net-vlan-off: nothing at all ---
log=$out/net-vlan-off.log
want net-vlan-off "[e1000e] no VLAN: the network stays off"
if grep -aqE "e1000e\] (transmitter on|rings:|address)" "$log"; then
    fail "net-vlan-off: the driver touched the chip without a VLAN"
fi
[ -z "$(ls "$out/net-vlan-off.netlog" 2>/dev/null)" ] ||
    fail "net-vlan-off: netlog's receiver got a log"
if grep -aE "dhcp: lease|netstack: link up|netstack: on |update: fetched" "$log"; then
    fail "net-vlan-off: a network came up without a VLAN"
fi

if [ $ok = 1 ]; then
    echo "net-vlan-test: PASS"
    exit 0
fi
echo "net-vlan-test: FAIL (logs in $out)"
exit 1
