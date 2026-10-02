#!/usr/bin/env python3
"""The network test peer: a small network on VLAN 21 for Jam OS in QEMU
(docs/M9-PLAN.md "Testing without the real NIC"; docs/TESTING.md "The
network peer").

QEMU's `-netdev dgram` carries each Ethernet frame the guest's NIC sends as
one UDP datagram (the frame's bytes, no FCS) to this peer on 127.0.0.1,
and each datagram the peer sends back is one frame the NIC receives.
tools/qemu-test.sh starts the peer itself when QEMU_NET=1.

What it does with each frame from the guest:
  - checks the rule: every frame Jam OS sends is tagged 802.1Q with the
    VLAN (21; --vlan). Anything else (untagged, priority-tagged VLAN 0,
    another VLAN, an outer QinQ tag, a runt, one over 1518 bytes) is
    counted as bad, logged (the first few in hex) and fails the run;
  - strips the tag and answers as a small network: ARP for any IPv4
    address (the peer's MAC; not ARP probes from 0.0.0.0 nor gratuitous
    ones), ICMP echo for any address (so `ping 1.1.1.1` works in QEMU);
    UDP to a port with a handler (add_udp: later DHCP, DNS, netlog and the
    update server hook in here). Every reply is tagged with the VLAN.
  - pings (--ping ADDR): every half second, once it has seen the guest's
    MAC, an ICMP echo request from the Mac's address (10.2.21.174) to
    ADDR; the guest's echo replies to them are checked (checksum, id,
    sequence) and counted (ping_replies in the summary).
  - noise (--noise S, or `noise` on stdin): frames the guest's driver must
    drop (untagged, VLAN 10, a priority tag, QinQ) and one it must pass (a
    broadcast ARP request on the VLAN), counted as sent.

Run (one of):
    netpeer.py --listen P --qemu Q [--vlan N] [--expect-none] [--noise S]
               [--duration S] [--stdin] [--summary FILE] [--ready FILE] [--ping ADDR]
               [--log FILE]
    netpeer.py --free-ports N     print N free UDP ports on 127.0.0.1
    netpeer.py --selftest         the peer against a fake guest, host only

--listen is the peer's UDP port, --qemu QEMU's own (its dgram local port).
--expect-none: any frame at all from the guest fails (the vlan=off run).
--stdin reads commands, one per line: `send <hex>` (an untagged frame,
sent tagged with the VLAN), `raw <hex>` (sent as it is), `noise`, `stats`
(the summary as one JSON line on stdout), `quit`. Otherwise the peer runs
until --duration passes, SIGTERM or SIGINT. At the end it writes the
summary (JSON) to --summary, prints one line `netpeer: ... PASS|FAIL`, and
exits 0 on PASS, 1 on FAIL.

As a module (import netpeer, with tools/ on sys.path): Peer(listen, qemu,
vlan) and its poll, send, send_raw, noise, add_udp and summary; the frame
builders (eth, arp, ipv4, udp, icmp) and classify."""
import argparse
import json
import os
import select
import signal
import socket
import struct
import sys
import time

VLAN = 21
PEER_MAC = bytes.fromhex("024a414d0001")    # locally administered: "JAM" 0001
BROADCAST = b"\xff" * 6
TPID_8021Q, TPID_8021AD, TPID_9100 = 0x8100, 0x88A8, 0x9100
ETH_ARP, ETH_IPV4 = 0x0806, 0x0800
FRAME_MAX = 1518            # a tagged frame without FCS
BAD_LOGGED = 8              # bad frames logged in hex, at most
PING_FROM = "10.2.21.174"   # the Mac's address: where --ping's requests come from
PING_ID = 0x4a4d            # their ICMP id ("JM")


# ---- frames ------------------------------------------------------------------

def classify(frame):
    """(kind, vid, ethertype) of a frame: kind is 'runt', 'untagged',
    'priority' (VLAN 0), 'vlan' or 'outer' (QinQ), as <jam/netframe.h>."""
    if len(frame) < 14:
        return "runt", 0, 0
    t = struct.unpack_from("!H", frame, 12)[0]
    if t not in (TPID_8021Q, TPID_8021AD, TPID_9100):
        return "untagged", 0, t
    if len(frame) < 18:
        return "runt", 0, 0
    tci, inner = struct.unpack_from("!HH", frame, 14)
    vid = tci & 0x0FFF
    if t != TPID_8021Q:
        return "outer", vid, inner
    return ("vlan" if vid else "priority"), vid, inner


def tag(frame, vlan, pcp=0):
    """frame (untagged) with an 802.1Q tag for vlan after the addresses."""
    return frame[:12] + struct.pack("!HH", TPID_8021Q, (pcp << 13) | vlan) + frame[12:]


def untag(frame):
    return frame[:12] + frame[16:]


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def eth(dst, src, ethertype, payload):
    return dst + src + struct.pack("!H", ethertype) + payload


def arp(op, sha, spa, tha, tpa):
    """An ARP packet (Ethernet, IPv4); addresses as bytes."""
    return struct.pack("!HHBBH", 1, ETH_IPV4, 6, 4, op) + sha + spa + tha + tpa


def ipv4(src, dst, proto, payload, ttl=64, ident=0):
    hdr = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), ident, 0, ttl, proto, 0,
                      src, dst)
    hdr = hdr[:10] + struct.pack("!H", checksum(hdr)) + hdr[12:]
    return hdr + payload


def udp(src, dst, sport, dport, payload):
    """A UDP datagram (header and payload) with its checksum, for ipv4()."""
    n = 8 + len(payload)
    pseudo = src + dst + struct.pack("!BBH", 0, 17, n)
    hdr = struct.pack("!HHHH", sport, dport, n, 0)
    c = checksum(pseudo + hdr + payload) or 0xFFFF
    return struct.pack("!HHHH", sport, dport, n, c) + payload


def icmp(typ, code, rest, data):
    msg = struct.pack("!BBH", typ, code, 0) + rest + data
    return msg[:2] + struct.pack("!H", checksum(msg)) + msg[4:]


def ip_str(b):
    return ".".join(str(x) for x in b)


def ip_bytes(s):
    return socket.inet_aton(s)


# ---- the peer ----------------------------------------------------------------

class Peer:
    """One end of QEMU's dgram netdev: a UDP socket on 127.0.0.1:listen,
    sending to 127.0.0.1:qemu."""

    def __init__(self, listen, qemu, vlan=VLAN, log=None):
        self.vlan, self.log_file = vlan, log
        self.qemu = ("127.0.0.1", qemu)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", listen))
        self.listen = self.sock.getsockname()[1]
        self.counts = {"frames": 0, "good": 0, "bad": 0, "arp_replies": 0, "echo_replies": 0,
                       "udp_in": 0, "unhandled": 0, "sent": 0, "noise_drop": 0,
                       "noise_pass": 0, "pings": 0, "ping_replies": 0}
        self.bad_kinds = {}          # kind (or "vlan <id>") -> count
        self.guest_mac = None        # learned from its first good frame
        self.udp_handlers = {}       # port -> fn(peer, ip_src, sport, ip_dst, payload) -> bytes|None

    def log(self, msg):
        line = "netpeer: " + msg
        if self.log_file:
            self.log_file.write(line + "\n")
            self.log_file.flush()
        else:
            print(line, file=sys.stderr)

    # sending
    def send_raw(self, frame):
        self.sock.sendto(frame, self.qemu)
        self.counts["sent"] += 1

    def send(self, frame):
        """An untagged frame, sent tagged with the VLAN."""
        self.send_raw(tag(frame, self.vlan))

    def add_udp(self, port, fn):
        """fn(peer, src_ip, sport, dst_ip, payload) answers a datagram to
        `port` on any address: its result (bytes) goes back as a datagram
        from that address and port; None: no answer."""
        self.udp_handlers[port] = fn

    def ping(self, addr):
        """An echo request to the guest at addr (once its MAC is known)."""
        if self.guest_mac is None:
            return
        seq = self.counts["pings"] + 1
        body = icmp(8, 0, struct.pack("!HH", PING_ID, seq), b"jamos-ping" * 5)
        self.send(eth(self.guest_mac, PEER_MAC, ETH_IPV4,
                      ipv4(ip_bytes(PING_FROM), ip_bytes(addr), 1, body)))
        self.counts["pings"] = seq

    def noise(self):
        """Frames the guest's driver must drop, and one it must pass."""
        dst = BROADCAST
        req = eth(dst, PEER_MAC, ETH_ARP,
                  arp(1, PEER_MAC, ip_bytes("10.2.21.1"), b"\0" * 6, ip_bytes("10.2.21.99")))
        qinq = req[:12] + struct.pack("!HHHH", TPID_8021AD, self.vlan, TPID_8021Q,
                                      self.vlan) + req[12:]
        for f in (req,                     # untagged
                  tag(req, 10),            # another VLAN
                  tag(req, 0, pcp=5),      # a priority tag (VLAN 0)
                  qinq):                   # an outer tag over the VLAN's
            self.send_raw(f)
            self.counts["noise_drop"] += 1
        self.send(req)
        self.counts["noise_pass"] += 1

    # receiving
    def poll(self, timeout):
        """Handle what arrives within timeout seconds (at least one wait)."""
        r, _, _ = select.select([self.sock], [], [], timeout)
        while r:
            frame, _ = self.sock.recvfrom(65536)
            self.handle(frame)
            r, _, _ = select.select([self.sock], [], [], 0)

    def handle(self, frame):
        self.counts["frames"] += 1
        kind, vid, ethertype = classify(frame)
        if kind != "vlan" or vid != self.vlan or len(frame) > FRAME_MAX:
            what = kind if kind != "vlan" else "vlan %d" % vid
            if len(frame) > FRAME_MAX:
                what = "too long"
            self.counts["bad"] += 1
            self.bad_kinds[what] = self.bad_kinds.get(what, 0) + 1
            if self.counts["bad"] <= BAD_LOGGED:
                self.log("BAD frame from the guest (%s, %d bytes): %s" %
                         (what, len(frame), frame[:64].hex()))
            return
        self.counts["good"] += 1
        f = untag(frame)
        if self.guest_mac is None:
            self.guest_mac = f[6:12]
        if ethertype == ETH_ARP:
            self.on_arp(f)
        elif ethertype == ETH_IPV4:
            self.on_ipv4(f)
        else:
            self.counts["unhandled"] += 1

    def on_arp(self, f):
        if len(f) < 42:
            return
        htype, ptype, hlen, plen, op = struct.unpack_from("!HHBBH", f, 14)
        sha, spa, tpa = f[22:28], f[28:32], f[38:42]
        if (htype, ptype, hlen, plen, op) != (1, ETH_IPV4, 6, 4, 1):
            return
        if spa == b"\0\0\0\0" or spa == tpa:   # a probe or a gratuitous ARP: nobody answers
            return
        self.send(eth(sha, PEER_MAC, ETH_ARP, arp(2, PEER_MAC, tpa, sha, spa)))
        self.counts["arp_replies"] += 1

    def on_ipv4(self, f):
        p = f[14:]
        if len(p) < 20 or p[0] >> 4 != 4:
            return
        ihl = (p[0] & 15) * 4
        total = struct.unpack_from("!H", p, 2)[0]
        if ihl < 20 or total < ihl or total > len(p) or checksum(p[:ihl]):
            return
        proto, src, dst, body = p[9], p[12:16], p[16:20], p[ihl:total]
        reply = None
        if (proto == 1 and len(body) >= 8 and body[0] == 0 and not checksum(body) and
                struct.unpack_from("!H", body, 4)[0] == PING_ID and dst == ip_bytes(PING_FROM)):
            self.counts["ping_replies"] += 1   # an answer to --ping
            return
        if proto == 1 and len(body) >= 8 and body[0] == 8 and not checksum(body):
            reply = ipv4(dst, src, 1, icmp(0, 0, body[4:8], body[8:]))
            self.counts["echo_replies"] += 1
        elif proto == 17 and len(body) >= 8:
            self.counts["udp_in"] += 1
            sport, dport = struct.unpack_from("!HH", body, 0)
            fn = self.udp_handlers.get(dport)
            out = fn(self, src, sport, dst, body[8:]) if fn else None
            if out is not None:
                reply = ipv4(dst, src, 17, udp(dst, src, dport, sport, out))
        if reply is None:
            self.counts["unhandled"] += proto not in (1, 17)
            return
        self.send(eth(f[6:12], PEER_MAC, ETH_IPV4, reply))

    def summary(self, expect_none=False):
        ok = self.counts["bad"] == 0 and not (expect_none and self.counts["frames"])
        return dict(self.counts, vlan=self.vlan, bad_kinds=self.bad_kinds,
                    expect_none=expect_none, result="PASS" if ok else "FAIL")


def summary_line(s):
    return ("netpeer: %d frames from the guest, %d tagged %d, %d bad%s; answered %d ARP, %d echo; "
            "%d of %d pings answered; sent %d -> %s" %
            (s["frames"], s["good"], s["vlan"], s["bad"],
             " " + json.dumps(s["bad_kinds"]) if s["bad_kinds"] else "",
             s["arp_replies"], s["echo_replies"], s["ping_replies"], s["pings"], s["sent"],
             s["result"]))


def free_ports(n):
    socks = []
    for _ in range(n):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        socks.append(s)
    ports = [s.getsockname()[1] for s in socks]
    for s in socks:
        s.close()
    return ports


# ---- the self-test -------------------------------------------------------------

def selftest():
    """The peer against a fake guest (a second socket), host only: what it
    answers, what it counts as bad, its noise. 0 on PASS."""
    g = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    g.bind(("127.0.0.1", 0))
    g.settimeout(2)
    devnull = open(os.devnull, "w")
    peer = Peer(0, g.getsockname()[1], VLAN, devnull)
    g.connect(("127.0.0.1", peer.listen))
    gmac, gip = bytes.fromhex("525400123456"), ip_bytes("10.2.21.50")
    fails = []

    def expect(cond, what):
        if not cond:
            fails.append(what)

    def exchange(frame, want_reply):
        g.send(frame)
        peer.poll(1)
        if not want_reply:
            return None
        try:
            return g.recv(65536)
        except socket.timeout:
            fails.append("no reply to %s" % frame[:20].hex())
            return None

    # ARP for the router, tagged 21: answered, tagged 21, with the peer's MAC.
    req = eth(BROADCAST, gmac, ETH_ARP, arp(1, gmac, gip, b"\0" * 6, ip_bytes("10.2.21.1")))
    r = exchange(tag(req, VLAN), True)
    if r:
        kind, vid, et = classify(r)
        u = untag(r)
        expect(kind == "vlan" and vid == VLAN and et == ETH_ARP, "ARP reply tagged 21")
        expect(u[:6] == gmac and u[22:28] == PEER_MAC and u[28:32] == ip_bytes("10.2.21.1"),
               "ARP reply's addresses")
    # An ARP probe (sender 0.0.0.0) and a gratuitous ARP: no answer.
    probe = eth(BROADCAST, gmac, ETH_ARP, arp(1, gmac, b"\0" * 4, b"\0" * 6, gip))
    exchange(tag(probe, VLAN), False)
    grat = eth(BROADCAST, gmac, ETH_ARP, arp(1, gmac, gip, b"\0" * 6, gip))
    exchange(tag(grat, VLAN), False)
    # ICMP echo to 1.1.1.1: answered from 1.1.1.1, checksums good.
    ping = icmp(8, 0, struct.pack("!HH", 0x1234, 7), b"jamos-ping" * 5)
    r = exchange(tag(eth(PEER_MAC, gmac, ETH_IPV4, ipv4(gip, ip_bytes("1.1.1.1"), 1, ping)),
                     VLAN), True)
    if r:
        u = untag(r)
        ip = u[14:34]
        expect(classify(r)[:2] == ("vlan", VLAN), "echo reply tagged 21")
        expect(ip[12:16] == ip_bytes("1.1.1.1") and ip[16:20] == gip and not checksum(ip),
               "echo reply's IP header")
        expect(u[34] == 0 and not checksum(u[34:]) and u[38:] == ping[4:], "echo reply's ICMP")
    # A UDP handler (here an echo on port 7).
    peer.add_udp(7, lambda p, src, sport, dst, data: data[::-1])
    d = udp(gip, ip_bytes("10.2.21.1"), 4000, 7, b"hello")
    r = exchange(tag(eth(PEER_MAC, gmac, ETH_IPV4, ipv4(gip, ip_bytes("10.2.21.1"), 17, d)),
                     VLAN), True)
    if r:
        u = untag(r)
        expect(u[42:] == b"olleh" and struct.unpack_from("!HH", u, 34) == (7, 4000),
               "UDP handler's answer")
    good = peer.counts["good"]
    expect(peer.counts["bad"] == 0 and good == 5, "5 good frames so far, none bad")
    # The bad ones: untagged, another VLAN, VLAN 0, QinQ, a runt, too long.
    bad = [req, tag(req, 10), tag(req, 0), req[:12] + struct.pack("!HH", TPID_8021AD, 21) +
           tag(req, 21)[12:], req[:10], tag(req + b"\0" * 1500, VLAN)]
    for f in bad:
        exchange(f, False)
    expect(peer.counts["bad"] == len(bad) and peer.counts["good"] == good, "bad frames counted")
    expect(peer.summary()["result"] == "FAIL", "a bad frame fails the run")
    expect(peer.counts["arp_replies"] == 1 and peer.counts["echo_replies"] == 1,
           "only the right frames were answered")
    # Noise: four frames to drop, one to pass, each what it says.
    peer.noise()
    kinds = []
    for _ in range(5):
        try:
            k = classify(g.recv(65536))
            kinds.append(k[0] if k[0] != "vlan" else "vlan %d" % k[1])
        except socket.timeout:
            break
    expect(kinds == ["untagged", "vlan 10", "priority", "outer", "vlan 21"], "noise: %s" % kinds)
    # --ping: a request to the guest's MAC (learned), tagged; its reply counted.
    peer.ping("10.2.21.50")
    try:
        r = untag(g.recv(65536))
        expect(r[:6] == gmac and r[30:34] == gip and r[34] == 8 and not checksum(r[34:]),
               "--ping's request")
        reply = icmp(0, 0, r[38:42], r[42:])
        g.send(tag(eth(PEER_MAC, gmac, ETH_IPV4, ipv4(gip, r[26:30], 1, reply)), VLAN))
        peer.poll(1)
        expect(peer.counts["ping_replies"] == 1 and peer.counts["echo_replies"] == 1,
               "--ping's reply counted (and not answered)")
    except socket.timeout:
        fails.append("no --ping request")
    fresh = Peer(0, g.getsockname()[1], VLAN, devnull)
    expect(fresh.summary(expect_none=True)["result"] == "PASS", "no frames, expect-none: PASS")
    fresh.handle(tag(req, VLAN))
    expect(fresh.summary(expect_none=True)["result"] == "FAIL", "a frame, expect-none: FAIL")
    for f in fails:
        print("netpeer selftest: FAILED: " + f)
    print("netpeer selftest: %s" % ("PASS" if not fails else "FAIL"))
    return 0 if not fails else 1


# ---- the command line ----------------------------------------------------------

def stdin_command(peer, line, a):
    """One --stdin command; False to stop."""
    words = line.split()
    if not words:
        return True
    cmd, arg = words[0], "".join(words[1:])
    if cmd == "quit":
        return False
    if cmd == "send":
        peer.send(bytes.fromhex(arg))
    elif cmd == "raw":
        peer.send_raw(bytes.fromhex(arg))
    elif cmd == "noise":
        peer.noise()
    elif cmd == "stats":
        print(json.dumps(peer.summary(a.expect_none)), flush=True)
    else:
        peer.log("unknown command %r" % cmd)
    return True


def run(a):
    log = open(a.log, "a") if a.log else None
    peer = Peer(a.listen, a.qemu, a.vlan, log)
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    signal.signal(signal.SIGINT, lambda *_: stop.append(1))
    if a.ready:
        with open(a.ready, "w") as f:
            f.write("%d\n" % peer.listen)
    peer.log("listening on 127.0.0.1:%d, QEMU at %d, VLAN %d" % (peer.listen, a.qemu, a.vlan))
    end = time.monotonic() + a.duration if a.duration else None
    next_noise = time.monotonic() + a.noise if a.noise else None
    next_ping = time.monotonic() + 0.5 if a.ping else None
    while not stop and (end is None or time.monotonic() < end):
        fds = [peer.sock] + ([sys.stdin] if a.stdin else [])
        r, _, _ = select.select(fds, [], [], 0.2)
        if peer.sock in r:
            peer.poll(0)
        if a.stdin and sys.stdin in r:
            line = sys.stdin.readline()
            if not line or not stdin_command(peer, line, a):
                break
        if next_ping and time.monotonic() >= next_ping:
            peer.ping(a.ping)
            next_ping += 0.5
        if next_noise and time.monotonic() >= next_noise:
            peer.noise()
            next_noise += a.noise
    peer.poll(0)
    s = peer.summary(a.expect_none)
    if a.summary:
        with open(a.summary, "w") as f:
            json.dump(s, f, indent=1)
    peer.log(summary_line(s)[len("netpeer: "):])
    print(summary_line(s))
    return 0 if s["result"] == "PASS" else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--listen", type=int, default=0)
    ap.add_argument("--qemu", type=int, default=0)
    ap.add_argument("--vlan", type=int, default=VLAN)
    ap.add_argument("--expect-none", action="store_true")
    ap.add_argument("--noise", type=float, default=0)
    ap.add_argument("--duration", type=float, default=0)
    ap.add_argument("--stdin", action="store_true")
    ap.add_argument("--summary")
    ap.add_argument("--ready")
    ap.add_argument("--log")
    ap.add_argument("--ping")
    ap.add_argument("--free-ports", type=int, default=0)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.free_ports:
        print(" ".join(str(p) for p in free_ports(a.free_ports)))
        return 0
    if not a.qemu or not 1 <= a.vlan <= 4094:
        ap.error("--qemu <port> is needed, and --vlan must be 1..4094")
    return run(a)


if __name__ == "__main__":
    sys.exit(main())
