#!/bin/sh
# The network driver in QEMU (drivers/e1000e on QEMU's 82574L) against
# user/tests/nettest, a hostile netstack (docs/M9-PLAN.md stage 2). Each
# scenario is one `init` boot whose init.cfg runs `nettest <mode>` (a
# copy of the stick with its bootfs changed: init's regression mode runs
# no netstack, which in shell mode holds the card's one session), with
# QEMU_NET (tools/qemu-test.sh: the card on -netdev dgram, every frame
# the guest sends dumped to a pcap and checked by tools/pcap-vlan-check.py):
#   vlan      `nettest vlan`: the session rules, then every bad slot (bad
#             lengths, flags, frames already tagged 0x8100/0x88a8/0x9100,
#             produced counts out of range, a thread rewriting EtherTypes
#             while the driver copies). Whatever it writes, every frame
#             that leaves is tagged VLAN 21 exactly once: the scenario's
#             peer (below) and the pcap both check, and both count exactly
#             as many frames as the driver says it queued and the chip
#             says it sent;
#   vlan-off  a `vlan=off` boot: the driver says "no VLAN: the network
#             stays off" and finishes, `nettest off` finds no service, and
#             no frame at all leaves (tools/netpeer.py --expect-none and
#             the pcap);
#   rx        `nettest rx`: the peer answers its "go" frames with untagged,
#             priority-tagged, other-VLAN, QinQ (0x88a8, 0x9100), nested
#             (a tag inside 21), too long and VLAN 21 frames, then a flood
#             of 300 with the ring left unread: only VLAN 21 reaches
#             nettest, untagged and whole, the driver's drop counts match
#             the peer's list exactly, and a full ring drops and counts.
# The scenario peer is tools/netpeer.py's Peer (imported) plus what these
# tests need: it answers "nettest-go <n>" frames, and it also fails on a
# frame with a tag inside VLAN 21's (the stock peer and the pcap check
# look at the outer tag only). The pcap is read again here for that too.
# QEMU_SMP passes through. Usage: tools/net-test.sh <outdir> [scenario...]
# (default: all three); exit 0 on PASS.
set -eu
out=$1
shift
[ $# -gt 0 ] || set -- vlan vlan-off rx
mkdir -p "$out"
ok=1

peer_py=$out/nettest_peer.py
cat > "$peer_py" <<'PY'
"""The net-test.sh scenarios' peer: tools/netpeer.py's Peer plus the
nettest frames (user/tests/nettest/nettest.h: the census RX_* and the
flood RX_FLOOD; rx.c's good_len)."""
import argparse, json, os, select, signal, struct, sys
sys.path.insert(0, "tools")
import netpeer as np

NT = 0x88B5
TPIDS = (0x8100, 0x88A8, 0x9100)
GOOD_LEN = [60, 64, 100, 200, 512, 1000, 1400, 1500, 1513, 1514]
FLOOD = 300


def ntframe(length, text):
    f = np.BROADCAST + np.PEER_MAC + struct.pack("!H", NT) + text.encode() + b"\0"
    return (f + b"\0" * length)[:length]


def census(vlan):
    """What the guest's driver must drop (5 untagged, 3 VLAN 0, 12 other
    VLANs or tags, 1 too long), then the RX_GOOD frames it must pass."""
    base = ntframe(60, "nettest-bad 0")
    on = np.tag(base, vlan)
    fs = [base] * 5
    fs += [np.tag(base, 0, pcp=p) for p in (1, 3, 7)]
    fs += [np.tag(base, v) for v in (1, 10, 11, 20, 22, 4094)]
    fs += [on[:12] + struct.pack("!HH", 0x88A8, vlan) + on[12:]] * 2
    fs += [on[:12] + struct.pack("!HH", 0x9100, vlan) + on[12:]] * 2
    fs += [np.tag(np.tag(base, 5), vlan)] * 2
    fs += [np.tag(ntframe(1518, "nettest-long 0"), vlan)]
    fs += [np.tag(ntframe(n, "nettest-rx %d" % i), vlan, pcp=i % 8)
           for i, n in enumerate(GOOD_LEN)]
    return fs


class NetTestPeer(np.Peer):
    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.nt = {"nested": 0, "go": 0, "marked": 0, "census_sent": 0, "flood_sent": 0}

    def handle(self, frame):
        super().handle(frame)
        kind, vid, et = np.classify(frame)
        if kind != "vlan" or vid != self.vlan:
            return
        if et in TPIDS:
            self.nt["nested"] += 1
            self.log("BAD frame from the guest: a tag (%#06x) inside VLAN %d's: %s"
                     % (et, vid, frame[:32].hex()))
            return
        if et != NT:
            return
        text = np.untag(frame)[14:].split(b"\0")[0]
        if not text.startswith(b"nettest-go "):
            self.nt["marked"] += 1
            return
        self.nt["go"] += 1
        n = text.split()[1]
        if n == b"1":
            for f in census(self.vlan):
                self.send_raw(f)
                self.nt["census_sent"] += 1
        elif n == b"2":
            for i in range(FLOOD):
                self.send(ntframe(60, "nettest-flood %d" % i))
                self.nt["flood_sent"] += 1

    def summary(self, expect_none=False):
        s = super().summary(expect_none)
        s.update(self.nt)
        if self.nt["nested"]:
            s["result"] = "FAIL"
        return s


def main():
    ap = argparse.ArgumentParser()
    for name in ("--listen", "--qemu", "--vlan"):
        ap.add_argument(name, type=int, default=21 if name == "--vlan" else 0)
    for name in ("--summary", "--ready", "--log"):
        ap.add_argument(name)
    a = ap.parse_args()
    peer = NetTestPeer(a.listen, a.qemu, a.vlan, open(a.log, "a"))
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    with open(a.ready, "w") as f:
        f.write("%d\n" % peer.listen)
    while not stop:
        peer.poll(0.2)
    s = peer.summary()
    with open(a.summary, "w") as f:
        json.dump(s, f, indent=1)
    print(np.summary_line(s) + " (nested %d, go %d, marked %d)" % (s["nested"], s["go"],
                                                                   s["marked"]))
    return 0 if s["result"] == "PASS" else 1


sys.exit(main())
PY

# The frames in a pcap whose tag (21) holds another tag inside it, and
# how many frames there are: "<frames> <nested>".
pcap_inner() {
    python3 - "$1" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read()
le = struct.unpack_from("<I", data)[0] in (0xA1B2C3D4, 0xA1B23C4D)
e = "<" if le else ">"
off, n, nested = 24, 0, 0
while off + 16 <= len(data):
    incl = struct.unpack_from(e + "I", data, off + 8)[0]
    f = data[off + 16:off + 16 + incl]
    off += 16 + incl
    n += 1
    if len(f) >= 18 and struct.unpack_from("!H", f, 12)[0] == 0x8100 and \
            struct.unpack_from("!H", f, 16)[0] in (0x8100, 0x88A8, 0x9100):
        nested += 1
print(n, nested)
PY
}

# A copy of build/jamos.img whose bootfs has init.cfg = "bin/nettest
# <mode>": the `init` boot runs it as a test suite, with devmgr's control
# channel and no netstack (init's regression mode starts none; in shell
# mode netstack holds the card's one session), so nettest can open it.
# $1: the image, $2: the mode.
nettest_image() {
    python3 - build/bootfs.img "$1.bootfs" "$2" "$1.d" <<'PY'
import os, struct, subprocess, sys
src, dst, mode, tmp = sys.argv[1:5]
data = open(src, "rb").read()
magic, _, count, _ = struct.unpack_from("<8sIIQ", data, 0)
assert magic == b"JAMBOOTF", "not a bootfs image"
args = []
for i in range(count):
    name, off, size = struct.unpack_from("<56sQQ", data, 24 + 72 * i)
    name = name.rstrip(b"\0").decode()
    path = os.path.join(tmp, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    body = data[off:off + size] if name != "init.cfg" else b"bin/nettest %s\n" % mode.encode()
    open(path, "wb").write(body)
    args.append("%s=%s" % (name, path))
subprocess.run([sys.executable, "tools/mkbootfs.py", dst] + args, check=True,
               stdout=subprocess.DEVNULL)
PY
    cp build/jamos.img "$1"
    mcopy -o -i "$1@@1M" "$1.bootfs" ::/boot/bootfs.img
    rm -rf "$1.bootfs" "$1.d"
}

# The `init` boot of nettest_image's stick, with the words $5...: $1 the
# run's name, $2 the mode, $3 QEMU_NET, $4 QEMU_NET_NONE. The run must end
# with no problems: the driver stopped cleanly when devmgr did, and every
# job is empty.
run_nettest() {
    name=$1 mode=$2 net=$3 none=$4
    shift 4
    nettest_image "$out/$name.stick" "$mode"
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_IMAGE=$out/$name.stick QEMU_NET=$net \
        QEMU_NET_NONE=$none tools/qemu-test.sh "$out" "$name" init "$@" > "$out/$name.out" 2>&1 || {
        echo "$name: the QEMU run failed:"
        tail -5 "$out/$name.out"
        ok=0
    }
    rm -f "$out/$name.stick"
    want_line "$name" "run complete: no problems"
}

# run_nettest with the scenario peer; its summary in $out/<name>.nt.json.
run_peer() {
    name=$1 mode=$2
    set -- $(python3 tools/netpeer.py --free-ports 2)
    pport=$1 qport=$2
    rm -f "$out/$name.nt.ready" "$out/$name.nt.json" "$out/$name.nt.log"
    python3 "$peer_py" --listen "$pport" --qemu "$qport" --summary "$out/$name.nt.json" \
        --ready "$out/$name.nt.ready" --log "$out/$name.nt.log" > "$out/$name.nt.out" 2>&1 &
    ppid=$!
    j=0
    while [ $j -lt 50 ] && [ ! -s "$out/$name.nt.ready" ]; do sleep 0.1; j=$((j + 1)); done
    run_nettest "$name" "$mode" "$pport:$qport" 0
    kill $ppid 2>/dev/null || true
    wait $ppid 2>/dev/null || { echo "$name: the peer failed: $(tail -1 "$out/$name.nt.out")"; ok=0; }
}

# A number from the peer's summary.
peer_count() {
    python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])" "$1" "$2"
}

want_line() {
    grep -qF -- "$2" "$out/$1.log" || { echo "$1: no line with \"$2\""; ok=0; }
}

scenario_vlan() {
    run_peer net-vlan vlan
    log=$out/net-vlan.log
    grep -aE "nettest: (race|tx):" "$log" | sed 's/^/  /' || true
    want_line net-vlan "nettest vlan: 2 passed"
    queued=$(grep -aoE "the driver queued [0-9]+ frames" "$log" | grep -oE "[0-9]+" | tail -1)
    queued=${queued:-missing}
    good=$(peer_count "$out/net-vlan.nt.json" good)
    bad=$(peer_count "$out/net-vlan.nt.json" bad)
    set -- $(pcap_inner "$out/net-vlan.pcap")
    echo "net-vlan: the driver queued $queued; the peer got $good tagged 21 ($bad bad); the" \
         "pcap holds $1 ($2 with a tag inside)"
    [ "$good" = "$queued" ] && [ "$1" = "$queued" ] && [ "$bad" = 0 ] && [ "$2" = 0 ] ||
        { echo "net-vlan: the frames that left don't match what the driver queued"; ok=0; }
}

scenario_vlan_off() {
    run_nettest net-off off 1 1 vlan=off
    want_line net-off "[e1000e] no VLAN: the network stays off"
    want_line net-off "nettest off: 1 passed"
    if grep -aqE "e1000e\] (transmitter on|rings:|address)" "$out/net-off.log"; then
        echo "net-off: the driver touched the chip without a VLAN"
        ok=0
    fi
    grep -aE "pcap-vlan-check|netpeer:" "$out/net-off.out" | sed 's/^/  /' || true
}

scenario_rx() {
    run_peer net-rx rx
    log=$out/net-rx.log
    grep -aE "nettest: (rx|flood):" "$log" | sed 's/^/  /' || true
    want_line net-rx "nettest rx: 2 passed"
    sent=$(peer_count "$out/net-rx.nt.json" census_sent)
    flood=$(peer_count "$out/net-rx.nt.json" flood_sent)
    good=$(peer_count "$out/net-rx.nt.json" good)
    echo "net-rx: the peer sent the census ($sent frames) and the flood ($flood); it got $good" \
         "frames, all tagged 21"
    [ "$sent" = 31 ] && [ "$flood" = 300 ] && [ "$good" = 2 ] ||
        { echo "net-rx: want 31 census frames, 300 flood frames, 2 go frames"; ok=0; }
}

for s in "$@"; do
    case $s in
    vlan) scenario_vlan ;;
    vlan-off) scenario_vlan_off ;;
    rx) scenario_rx ;;
    *) echo "net-test: no scenario $s (vlan, vlan-off, rx)"; exit 2 ;;
    esac
done

if [ "$ok" = 1 ]; then
    echo "net-test: PASS ($*)"
    exit 0
fi
echo "net-test: FAIL (logs in $out)"
exit 1
