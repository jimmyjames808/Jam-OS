#!/bin/sh
# The network rule over every path that transmits (docs/M9-PLAN.md stage 8,
# "the join"): every frame Jam OS sends is tagged 802.1Q VLAN 21, and with
# `vlan=off` none is sent at all; or, given `none`, every frame is sent
# untagged and none tagged (the untagged mode, a public build's default).
# Plain boots with QEMU's e1000e and tools/netpeer.py (its DHCP and DNS
# servers, netlog's receiver (--netlog), the update server serving this
# build (--update), an SNTP server with the Mac's time (--ntp), an echo
# request to the guest every half second (--ping 10.2.21.100) and the
# frames the driver must drop (--noise: tagged ones in the untagged
# mode)); only net.host = 10.2.21.174 in the stick's settings, so the
# address comes by DHCP:
#   net-vlan      tools/shell-tests/net-vlan.txt: the DHCP lease, netlog
#   (net-untagged sending this boot's log, `host` and `ping <name>`, `ping`
#    with none)   by address through the gateway, `update -n` fetching the
#                 build (checked by init, nothing loaded), sntp setting
#                 the clock from the gateway, netstack answering ARP and
#                 the peer's pings. The peer and the pcap each check every
#                 frame from the guest keeps the mode's rule
#                 (tools/qemu-test.sh); then the pcap is read again here
#                 and its frames counted per path (ARP, DHCP, DNS, echo
#                 requests, echo replies, netlog, update, SNTP): every path must
#                 have sent at least one frame, none may be unknown, and
#                 the peer must have seen exactly the pcap's frames. The
#                 boot gets the mode as its vlan= word unless it is this
#                 build's default (build/build.txt's net line): then it
#                 boots with no word, and the kernel must say the mode
#                 came from the build's default;
#   net-vlan-off  (VLAN 21 only) tools/shell-tests/net-vlan-off.txt: the
#                 same commands on a `vlan=off` boot (QEMU_NET_NONE: the
#                 peer's --expect-none and the pcap's): not one frame; the
#                 driver says "no VLAN: the network stays off" and never
#                 touches the chip, netstack finds no card, and each
#                 command says why it can't (no address, no answer).
# QEMU_SMP passes through. Usage: tools/net-vlan-test.sh <outdir> [21|none]
# (after `make -s image`); exit 0 on PASS.
set -u
out=$1
mode=${2:-21}
mkdir -p "$out"
ok=1

fail() {
    echo "net-vlan-test: FAILED: $1"
    ok=0
}

want() {    # want <run> <text>: a line of the run's serial log has <text>
    grep -aqF -- "$2" "$out/$1.log" || fail "$1: no line with \"$2\""
}

case $mode in
21) main=net-vlan runs="net-vlan net-vlan-off" built=vlan21 says="VLAN 21" ;;
none) main=net-untagged runs=net-untagged built=untagged says="untagged" ;;
*) echo "net-vlan-test: the mode is 21 or none, not $mode"; exit 2 ;;
esac
default=$(sed -n 's/^net //p' build/build.txt)
img="$out/net-vlan.base.img"
# The stick's build has a throwaway test key (tools/update-test-key.sh),
# and the update server serves that build, signed with it.
tools/update-test-key.sh "$out" "${QEMU_IMAGE:-build/jamos.img}" "$img" ||
    { echo "net-vlan-test: can't make the test key's stick"; exit 1; }
printf 'net.host = 10.2.21.174\n' > "$out/net-vlan.settings"
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/net-vlan.settings" ::/etc/settings ||
    { echo "net-vlan-test: can't write the stick's settings"; exit 1; }
cat > "$out/net-vlan.spec.json" <<EOF
{"kernel": "build/jamos.elf", "bootfs": "$out/testkey/bootfs-key.img",
 "key": "$out/testkey/key1/update.key", "plan": []}
EOF
# The untagged run's script: net-vlan.txt, with `net` saying untagged.
sed 's/^wait VLAN 21$/wait , untagged, MAC/' tools/shell-tests/net-vlan.txt \
    > "$out/net-untagged.txt"

for run in $runs; do
    words=shell none=0 script=tools/shell-tests/$run.txt word=1
    [ $run = net-vlan-off ] && words="shell vlan=off" none=1
    [ $run = net-untagged ] && script=$out/net-untagged.txt
    [ $run != net-vlan-off ] && [ "$default" = $built ] && word=0   # the build's default
    rm -rf "$out/$run.netlog"
    QEMU_IMAGE="$img" QEMU_NET=1 QEMU_NET_NONE=$none QEMU_NET_VLAN=$mode QEMU_NET_WORD=$word \
        QEMU_NET_PEER="--netlog $out/$run.netlog --update $out/net-vlan.spec.json --ntp $(date +%s) --ping 10.2.21.100 --noise 5" \
        QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=$script \
        tools/qemu-test.sh "$out" $run $words > "$out/$run.out" 2>&1 ||
        fail "$run: the script or the network checks (see $out/$run.out, $out/$run.log)"
    grep -aE "^(netpeer|pcap-vlan-check):" "$out/$run.out" | sed "s/^/$run: /"
done
rm -f "$img"

# --- the main run: the frames of each path ---
python3 - "$out" "$main" "$mode" <<'EOF' || ok=0
import json, os, struct, sys
out, run, mode = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, "tools")
import importlib
pcv = importlib.import_module("pcap-vlan-check")

PATHS = ["arp", "dhcp", "dns", "echo request", "echo reply", "netlog", "update", "sntp"]
UDP = {67: "dhcp", 53: "dns", 5021: "netlog", 5022: "update", 123: "sntp"}


def path(f):
    """Which path sent frame f (tagged 21, or untagged in the untagged
    mode: the checks above), or why it is none of them."""
    if mode == "none":
        if len(f) < 14 or struct.unpack_from("!H", f, 12)[0] in pcv.TPIDS:
            return "TAGGED"
        et, p = struct.unpack_from("!H", f, 12)[0], f[14:]
    else:
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
fs = pcv.frames(os.path.join(out, run + ".pcap"))
for f in fs:
    k = path(f)
    counts[k] = counts.get(k, 0) + 1
peer = json.load(open(os.path.join(out, run + ".peer.json")))
print("%s: frames per path: " % run + ", ".join("%s %d" % kv for kv in counts.items()) +
      " (%d in all; the peer got %d, %d bad)" % (len(fs), peer["frames"], peer["bad"]))
fails = [k for k in PATHS if not counts[k]]
fails += [k for k in counts if k not in PATHS]
if fails:
    print("%s: FAILED: a path sent nothing, or frames from no known path: " % run +
          ", ".join(fails))
if peer["frames"] != len(fs) or peer["bad"]:
    print("%s: FAILED: the peer and the pcap don't agree" % run)
    fails.append("peer")
print("%s: the peer answered %d of its %d pings, %d netlog datagram(s) in" %
      (run, peer["ping_replies"], peer["pings"], peer.get("netlog_in", 0)))
sys.exit(1 if fails else 0)
EOF
log=$out/$main.log
grep -aq "update: .* -> .*: checked by init in .* not loaded (-n)" "$log" ||
    fail "$main: update -n wasn't checked by init"
[ "$(grep -ac "kexec: kexec_load from init" "$log")" -eq 0 ] ||
    fail "$main: update -n loaded a build"
ls "$out/$main.netlog/"*.txt > /dev/null 2>&1 || fail "$main: no log file from netlog"
want $main "off: set from the gateway (10.2.21.1, stratum 2"
if [ "$default" = $built ]; then
    want $main "network:     $says (the build's default)"
else
    want $main "network:     $says (the vlan= word)"
fi
want $main "netstack: on 82574L, $says, MAC"

# --- net-vlan-off: nothing at all ---
if [ $mode = 21 ]; then
    log=$out/net-vlan-off.log
    want net-vlan-off "[e1000e] no VLAN: the network stays off"
    want net-vlan-off "network:     off (the vlan= word)"
    if grep -aqE "e1000e\] (transmitter on|rings:|address)" "$log"; then
        fail "net-vlan-off: the driver touched the chip without a VLAN"
    fi
    [ -z "$(ls "$out/net-vlan-off.netlog" 2>/dev/null)" ] ||
        fail "net-vlan-off: netlog's receiver got a log"
    if grep -aE "dhcp: lease|netstack: link up|netstack: on |update: fetched" "$log"; then
        fail "net-vlan-off: a network came up without a VLAN"
    fi
fi

if [ $ok = 1 ]; then
    echo "net-vlan-test ($says): PASS"
    exit 0
fi
echo "net-vlan-test ($says): FAIL (logs in $out)"
exit 1
