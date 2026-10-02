#!/usr/bin/env python3
"""The network test peer: a small network on VLAN 21 (or, with --vlan
none, an untagged one) for Jam OS in QEMU (docs/M9-PLAN.md "Testing
without the real NIC"; docs/TESTING.md "The network peer").

QEMU's `-netdev dgram` carries each Ethernet frame the guest's NIC sends as
one UDP datagram (the frame's bytes, no FCS) to this peer on 127.0.0.1,
and each datagram the peer sends back is one frame the NIC receives.
tools/qemu-test.sh starts the peer itself when QEMU_NET=1.

What it does with each frame from the guest:
  - checks the rule: every frame Jam OS sends is tagged 802.1Q with the
    VLAN (21; --vlan). Anything else (untagged, priority-tagged VLAN 0,
    another VLAN, an outer QinQ tag, a runt, one over 1518 bytes) is
    counted as bad, logged (the first few in hex) and fails the run. With
    --vlan none (the untagged mode) it is the other way round: every frame
    must be untagged (14..1514 bytes), and any tagged one (VLAN 0, VLAN 21
    and QinQ too) is bad. Everything below then happens untagged: replies,
    pings and DHCP go out with no tag, and the frames the driver must drop
    are the tagged ones;
  - strips the tag and answers as a small network: ARP for any IPv4
    address (the peer's MAC; not ARP probes from 0.0.0.0 nor gratuitous
    ones), ICMP echo for any address (so `ping 1.1.1.1` works in QEMU);
    UDP to a port with a handler (add_udp: later DHCP, DNS, netlog and the
    update server hook in here; --update SPEC: port 5022 is
    tools/update-server.py's peer_handler, SPEC its JSON file). Every
    reply is tagged with the VLAN.
  - pings (--ping ADDR): every half second, once it has seen the guest's
    MAC, an ICMP echo request from the Mac's address (10.2.21.174) to
    ADDR; the guest's echo replies to them are checked (checksum, id,
    sequence) and counted (ping_replies in the summary).
  - netlog (--netlog FOLDER): datagrams to port 5021 go to
    tools/netlog-recv.py's Receiver, which writes the guest's logs into
    FOLDER and gives the ack sent back; --netlog-late S drops them (as if
    nobody listened) until S seconds after the first one, --netlog-pause
    BYTES:S drops them for S seconds once a stream has BYTES (a receiver
    paused mid-way). Counted as netlog_in / netlog_dropped.
  - noise (--noise S, or `noise` on stdin): frames the guest's driver must
    drop (untagged, VLAN 10, a priority tag, QinQ) and one it must pass (a
    broadcast ARP request on the VLAN), counted as sent.
  - a flood (--flood N, with --ping ADDR): N frames a second, the mix a
    busy trunk port carries (flood()): on the VLAN, ARP requests for other
    hosts and for ADDR, broadcast and multicast datagrams, UDP to ADDR's
    closed ports, IPv4 to the guest's MAC for other addresses, echo
    requests to ADDR from other hosts, frames for other MACs, IPv6; and
    off it, frames the driver must drop (untagged, VLANs 10 and 20, a
    priority tag, QinQ). Counted as flood_sent / flood_vlan. With
    --ping-every S the pings come every S seconds (0.5 by default); with
    --late-after S the summary also counts the pings sent S seconds or
    more after the first one, and their replies (pings_late,
    ping_replies_late): tools/rxsoak-test.sh checks the guest still
    answers after thousands of frames.
  - DHCP and DNS (add_dhcp_dns, always on): a DHCP server on port 67
    (leases from 10.2.21.100, router and DNS server 10.2.21.1, the lease
    --dhcp-lease seconds) and a DNS server on port 53 (one.one.one.one,
    mac.jam, router.jam, the CNAME www.jam, fastN.jam = 10.9.0.N;
    slow.jam is never answered, every other name is NXDOMAIN), counted
    as dhcp_* and dns_* in the summary.
  - TCP (--tcp-serve PORT:BYTES, --tcp-connect ADDR:PORT:CONNS:BYTES):
    tools/tcppeer.py's small TCP: a server the guest connects to and
    clients that connect to the guest's listener, every byte checked;
    counted as tcp_* in the summary (tools/tcp-test.sh).
  - a relay (--tcp-relay PORT:HOSTPORT, --tcp-forward LPORT:ADDR:PORT,
    --udp-relay PORT:HOSTPORT):
    tools/tcprelay.py joins TCP connections on the VLAN to real sockets on
    127.0.0.1, so programs on the Mac (curl, python3 -m http.server,
    tools/speed.py) talk to the guest (tools/fetch-test.sh, serve-test.sh,
    speed-test.sh); counted as relay_* in the summary.
  - SNTP (--ntp UNIX): an SNTP server on port 123 of any address (so the
    gateway 10.2.21.1 answers): a client request is answered with the
    time UNIX seconds (fractions allowed), counted from the peer's start,
    stratum 2, the request's transmit timestamp as the origin. With
    --ntp-forge each answer is preceded by a forged one the guest must
    ignore: the same, but its origin one bit off and its time a year
    later. Counted as ntp_queries, ntp_answered, ntp_forged.

Run (one of):
    netpeer.py --listen P --qemu Q [--vlan N|none] [--expect-none] [--noise S]
               [--duration S] [--stdin] [--summary FILE] [--ready FILE] [--ping ADDR]
               [--log FILE] [--dhcp-lease S] [--netlog FOLDER [--netlog-late S] [--netlog-pause BYTES:S]]
               [--update SPEC] [--flood N] [--ping-every S] [--late-after S]
               [--ntp UNIX [--ntp-forge]] [--tcp-serve PORT:BYTES]
               [--tcp-connect ADDR:PORT:CONNS:BYTES] [--tcp-relay PORT:HOSTPORT,...]
               [--tcp-forward LPORT:ADDR:PORT,...] [--udp-relay PORT:HOSTPORT,...]
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
import collections
import importlib.util
import json
import os
import re
import select
import signal
import socket
import struct
import sys
import time

VLAN = 21                   # the default; None: the untagged mode (--vlan none)
PEER_MAC = bytes.fromhex("024a414d0001")    # locally administered: "JAM" 0001
BROADCAST = b"\xff" * 6
TPID_8021Q, TPID_8021AD, TPID_9100 = 0x8100, 0x88A8, 0x9100
ETH_ARP, ETH_IPV4 = 0x0806, 0x0800
FRAME_MAX = 1518            # a tagged frame without FCS
FRAME_MAX_PLAIN = 1514      # an untagged one
BAD_LOGGED = 8              # bad frames logged in hex, at most
PING_FROM = "10.2.21.174"   # the Mac's address: where --ping's requests come from
PING_ID = 0x4a4d            # their ICMP id ("JM")
FLOOD_HOST = bytes.fromhex("024a414d0063")   # another host on the VLAN (the flood's sender)


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


def parse_vlan(text):
    """--vlan's value: a VLAN id 1..4094, or None for the untagged mode
    ("none" or "untagged")."""
    if text in ("none", "untagged"):
        return None
    v = int(text)
    if not 1 <= v <= 4094:
        raise ValueError("a VLAN is 1..4094, or none")
    return v


def vlan_name(vlan):
    return "untagged" if vlan is None else "VLAN %d" % vlan


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
        self.ping_sent_at = {}       # seq -> time.monotonic() it went (--ping)
        self.late_after = None       # pings this long after the first count as late (--late-after)
        self.first_ping = None       # time.monotonic() of the first ping
        self.flood_n = 0             # frames flood() has sent, for its rotation
        self.tcp = None              # tools/tcppeer.py's Tcp (--tcp-serve, --tcp-connect)
        self.relay = None            # tools/tcprelay.py's Relay (--tcp-relay, --tcp-forward)

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
        """An untagged frame, sent tagged with the VLAN (as it is in the
        untagged mode)."""
        self.send_raw(frame if self.vlan is None else tag(frame, self.vlan))

    def add_udp(self, port, fn):
        """fn(peer, src_ip, sport, dst_ip, payload) answers a datagram to
        `port` on any address: its result (bytes) goes back as a datagram
        from that address and port; a UdpReply, from and to the addresses
        it names; None: no answer."""
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
        t = time.monotonic()
        self.ping_sent_at[seq] = t
        self.first_ping = self.first_ping or t
        if self.is_late(t):
            self.counts["pings_late"] = self.counts.get("pings_late", 0) + 1

    def is_late(self, t):
        return self.late_after is not None and t - self.first_ping >= self.late_after

    def flood_frames(self, addr):
        """The flood's frames, one of each kind: (frame, on the VLAN?).
        Untagged frames get the VLAN's tag when they go (send)."""
        g, me, other = self.guest_mac, ip_bytes(addr), ip_bytes("10.2.21.77")
        n = self.flood_n
        host = FLOOD_HOST
        bcast_ip = ip_bytes("10.2.21.255")
        arp_other = eth(BROADCAST, host, ETH_ARP,
                        arp(1, host, other, b"\0" * 6, ip_bytes("10.2.21.%d" % (100 + n % 50))))
        arp_guest = eth(BROADCAST, host, ETH_ARP, arp(1, host, other, b"\0" * 6, me))
        ping_other = icmp(8, 0, struct.pack("!HH", 0x7777, n & 0xFFFF), b"x" * 32)
        frames = [
            (arp_other, True),
            (arp_guest, True),
            (eth(BROADCAST, host, ETH_IPV4, ipv4(other, bcast_ip, 17,
                                                  udp(other, bcast_ip, 137, 137, b"n" * 50))), True),
            (eth(bytes.fromhex("01005e000001"), host, ETH_IPV4,
                 ipv4(other, ip_bytes("224.0.0.1"), 17,
                      udp(other, ip_bytes("224.0.0.1"), 5353, 5353, b"m" * 40))), True),
            (eth(g, host, ETH_IPV4, ipv4(other, me, 17,
                                         udp(other, me, 40000, 9000 + n % 100, b"u" * 20))), True),
            (eth(g, host, ETH_IPV4, ipv4(other, ip_bytes("10.2.21.88"), 17,
                                         udp(other, ip_bytes("10.2.21.88"), 1, 2, b"o"))), True),
            (eth(g, host, ETH_IPV4, ipv4(other, me, 1, ping_other)), True),
            (eth(bytes.fromhex("024a414d0088"), host, ETH_IPV4,
                 ipv4(other, ip_bytes("10.2.21.88"), 17,
                      udp(other, ip_bytes("10.2.21.88"), 1, 2, b"p" * 300))), True),
            (eth(bytes.fromhex("333300000001"), host, 0x86DD, b"\x60" + b"\0" * 59), True),
            (eth(g, host, 0x86DD, b"\x60" + b"\0" * 59), True),
            (tag(arp_other, VLAN) if self.vlan is None else arp_other, False),
            (tag(arp_other, 10), False),              # untagged above: the native VLAN
            (tag(arp_guest, 20), False),
            (tag(arp_other, 0, pcp=3), False),        # a priority tag
            (arp_other[:12] + struct.pack("!HHHH", TPID_8021AD, self.vlan or VLAN, TPID_8021Q,
                                          self.vlan or VLAN) + arp_other[12:], False),   # QinQ
        ]
        return frames

    def flood(self, addr, count):
        """count frames of the flood's mix (once the guest's MAC is known)."""
        if self.guest_mac is None:
            return
        for k in ("flood_sent", "flood_vlan"):
            self.counts.setdefault(k, 0)
        frames = self.flood_frames(addr)
        for _ in range(count):
            f, ours = frames[self.flood_n % len(frames)]
            self.flood_n += 1
            if ours:
                self.send(f)
                self.counts["flood_vlan"] += 1
            else:
                self.send_raw(f)
            self.counts["flood_sent"] += 1

    def noise(self):
        """Frames the guest's driver must drop, and one it must pass."""
        dst = BROADCAST
        req = eth(dst, PEER_MAC, ETH_ARP,
                  arp(1, PEER_MAC, ip_bytes("10.2.21.1"), b"\0" * 6, ip_bytes("10.2.21.99")))
        v = self.vlan or VLAN
        qinq = req[:12] + struct.pack("!HHHH", TPID_8021AD, v, TPID_8021Q, v) + req[12:]
        for f in (tag(req, VLAN) if self.vlan is None else req,   # tagged 21, or untagged
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

    def rule_broken(self, frame, kind, vid):
        """Why a frame from the guest breaks the rule, or None."""
        if self.vlan is None:
            if kind != "untagged" or len(frame) > FRAME_MAX_PLAIN:
                return "too long" if kind == "untagged" else (
                    "TAGGED " + (kind if kind != "vlan" else "vlan %d" % vid))
            return None
        if kind != "vlan" or vid != self.vlan or len(frame) > FRAME_MAX:
            if len(frame) > FRAME_MAX:
                return "too long"
            return kind if kind != "vlan" else "vlan %d" % vid
        return None

    def handle(self, frame):
        self.counts["frames"] += 1
        kind, vid, ethertype = classify(frame)
        what = self.rule_broken(frame, kind, vid)
        if what:
            self.counts["bad"] += 1
            self.bad_kinds[what] = self.bad_kinds.get(what, 0) + 1
            if self.counts["bad"] <= BAD_LOGGED:
                self.log("BAD frame from the guest (%s, %d bytes): %s" %
                         (what, len(frame), frame[:64].hex()))
            return
        self.counts["good"] += 1
        f = frame if self.vlan is None else untag(frame)
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
            seq = struct.unpack_from("!H", body, 6)[0]
            sent = self.ping_sent_at.pop(seq, None)
            if sent is not None and self.is_late(sent):
                self.counts["ping_replies_late"] = self.counts.get("ping_replies_late", 0) + 1
            return
        if proto == 1 and len(body) >= 8 and body[0] == 8 and not checksum(body):
            reply = ipv4(dst, src, 1, icmp(0, 0, body[4:8], body[8:]))
            self.counts["echo_replies"] += 1
        elif proto == 6 and self.relay is not None and self.relay.input(f[6:12], src, dst, body):
            return
        elif proto == 6 and self.tcp is not None:
            self.tcp.input(f[6:12], src, dst, body)
            return
        elif proto == 17 and len(body) >= 8:
            self.counts["udp_in"] += 1
            sport, dport = struct.unpack_from("!HH", body, 0)
            fn = self.udp_handlers.get(dport)
            out = fn(self, src, sport, dst, body[8:]) if fn else None
            if isinstance(out, UdpReply):   # addressed by the handler (DHCP's broadcasts)
                self.send(eth(out.mac, PEER_MAC, ETH_IPV4, ipv4(
                    out.src, out.dst, 17, udp(out.src, out.dst, dport, sport, out.data))))
                return
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


def add_netlog(peer, folder, late=0.0, pause=None):
    """Port 5021 answered by tools/netlog-recv.py's Receiver (files into
    folder). late: datagrams dropped until late seconds after the first;
    pause (bytes, seconds): once a stream holds bytes, dropped for seconds."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "netlog-recv.py")
    spec = importlib.util.spec_from_file_location("netlog_recv", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    rx = mod.Receiver(folder, None, say=peer.log)
    st = {"first": None, "until": 0.0, "paused": pause is None}
    for k in ("netlog_in", "netlog_dropped"):
        peer.counts[k] = 0

    def handle(p, src, sport, dst, data):
        t = time.monotonic()
        p.counts["netlog_in"] += 1
        st["first"] = st["first"] or t
        if not st["paused"] and max([s.have for s in rx.streams.values()] or [0]) >= pause[0]:
            st["paused"], st["until"] = True, t + pause[1]
            p.log("netlog: the receiver pauses for %g s" % pause[1])
        if t < st["first"] + late or t < st["until"]:
            p.counts["netlog_dropped"] += 1
            return None
        return rx.handle(data)
    peer.add_udp(mod.PORT, handle)
    return rx


def summary_line(s):
    good = "untagged" if s["vlan"] is None else "tagged %d" % s["vlan"]
    return ("netpeer: %d frames from the guest, %d %s, %d bad%s; answered %d ARP, %d echo; "
            "%d of %d pings answered; sent %d -> %s" %
            (s["frames"], s["good"], good, s["bad"],
             " " + json.dumps(s["bad_kinds"]) if s["bad_kinds"] else "",
             s["arp_replies"], s["echo_replies"], s["ping_replies"], s["pings"], s["sent"],
             s["result"]))


# ---- DHCP and DNS ------------------------------------------------------------------

UdpReply = collections.namedtuple("UdpReply", "data src dst mac")   # addresses as bytes

DHCP_SERVER = "10.2.21.1"      # the peer's DHCP server, router and DNS server
DHCP_FIRST = 100               # leases from 10.2.21.100 up, one per MAC
DHCP_COOKIE = 0x63825363
DNS_TTL = 300
DNS_SLOW = "slow.jam"          # asked, never answered (the slow-peer test)
DNS_NONE = "nothing.jam"       # NXDOMAIN (as every name not below)
DNS_NAMES = {"one.one.one.one": ["1.1.1.1", "1.0.0.1"], "mac.jam": ["10.2.21.174"],
             "router.jam": ["10.2.21.1"]}
DNS_CNAMES = {"www.jam": "mac.jam"}
DNS_FAST = re.compile(r"fast(\d{1,3})\.jam$")   # fastN.jam: 10.9.0.N (N 1..254)


def dhcp_options(data):
    """{code: bytes} of a DHCP message's options (after the cookie), or None."""
    if len(data) < 240 or struct.unpack_from("!I", data, 236)[0] != DHCP_COOKIE:
        return None
    opts, i = {}, 240
    while i < len(data):
        code = data[i]
        if code == 255:
            break
        if code == 0:
            i += 1
            continue
        if i + 2 > len(data) or i + 2 + data[i + 1] > len(data):
            return None
        opts[code] = opts.get(code, b"") + data[i + 2:i + 2 + data[i + 1]]
        i += 2 + data[i + 1]
    return opts


class DhcpServer:
    """A DHCP server for the guest (RFC 2131): an address per MAC from
    10.2.21.<DHCP_FIRST> up, with the mask /24, the router and DNS server
    10.2.21.1 and a lease of `lease` seconds; OFFER for a DISCOVER, ACK for
    a REQUEST of the address it gave that MAC (NAK for any other: an
    INIT-REBOOT for an address it never gave), nothing for a REQUEST that
    names another server. Answers go to ciaddr when the client has one,
    else to the broadcast address. Counts in the peer's: dhcp_<type>."""

    NAMES = {1: "discovers", 2: "offers", 3: "requests", 4: "declines", 5: "acks", 6: "naks",
             7: "releases"}

    def __init__(self, peer, lease=3600):
        self.peer, self.lease, self.leases = peer, lease, {}
        self.server = ip_bytes(DHCP_SERVER)
        for n in list(self.NAMES.values()) + ["bad"]:
            peer.counts["dhcp_" + n] = 0

    def count(self, what):
        self.peer.counts["dhcp_" + what] += 1

    def address(self, mac):
        if mac not in self.leases:
            self.leases[mac] = ip_bytes("10.2.21.%d" % (DHCP_FIRST + len(self.leases)))
        return self.leases[mac]

    def reply(self, req, mtype, yiaddr):
        opts = struct.pack("!BBB", 53, 1, mtype) + struct.pack("!BB4s", 54, 4, self.server)
        if mtype != 6:
            opts += struct.pack("!BBI", 51, 4, self.lease)
            opts += struct.pack("!BB4s", 1, 4, ip_bytes("255.255.255.0"))
            opts += struct.pack("!BB4s", 3, 4, self.server)   # the router
            opts += struct.pack("!BB4s", 6, 4, self.server)   # the DNS server
        msg = (struct.pack("!BBBB4sHH4s4s4s4s", 2, 1, 6, 0, req[4:8], 0, struct.unpack_from(
               "!H", req, 10)[0], req[12:16], yiaddr, b"\0" * 4, b"\0" * 4) + req[28:44] +
               b"\0" * 192 + struct.pack("!I", DHCP_COOKIE) + opts + b"\xff")
        self.count(self.NAMES[mtype])
        unicast = req[12:16] != b"\0" * 4 and mtype != 6
        return UdpReply(msg, self.server, req[12:16] if unicast else b"\xff" * 4,
                        req[28:34] if unicast else BROADCAST)

    def handle(self, peer, src, sport, dst, data):
        opts = dhcp_options(data)
        if opts is None or data[0] != 1 or data[1:3] != b"\x01\x06" or len(opts.get(53, b"")) != 1:
            self.count("bad")
            return None
        mtype, mac = opts[53][0], bytes(data[28:34])
        if mtype not in self.NAMES:
            self.count("bad")
            return None
        self.count(self.NAMES[mtype])
        if mtype == 1:
            return self.reply(data, 2, self.address(mac))
        if mtype == 7:
            self.leases.pop(mac, None)
        if mtype != 3 or opts.get(54, self.server) != self.server:
            return None   # a DECLINE, a RELEASE, or a REQUEST to another server
        want = opts.get(50) or bytes(data[12:16])
        ok = self.leases.get(mac) == want
        return self.reply(data, 5 if ok else 6, want if ok else b"\0" * 4)


def dns_name(data, i):
    """The (uncompressed) name at data[i:] and the offset after it, or None."""
    labels = []
    while i < len(data) and data[i]:
        n = data[i]
        if n > 63 or i + 1 + n > len(data):
            return None
        labels.append(data[i + 1:i + 1 + n].decode("ascii", "replace"))
        i += 1 + n
    return (".".join(labels), i + 1) if i < len(data) else None


def dns_encode(name):
    return b"".join(bytes([len(l)]) + l.encode() for l in name.split(".")) + b"\0"


class DnsServer:
    """A DNS server for the guest (RFC 1035, A records): DNS_NAMES, the
    CNAME www.jam -> mac.jam (with its A record in the same reply),
    fastN.jam = 10.9.0.N; slow.jam is never answered (counted); every other
    name is NXDOMAIN; another type, no answer (NOERROR). Counts in the
    peer's: dns_queries, dns_answered, dns_nxdomain, dns_slow, dns_bad."""

    def __init__(self, peer):
        self.peer = peer
        for n in ("queries", "answered", "nxdomain", "slow", "bad"):
            peer.counts["dns_" + n] = 0

    def count(self, what):
        self.peer.counts["dns_" + what] += 1

    def records(self, name):
        """[(owner, type, rdata)] for name, or None (NXDOMAIN)."""
        name = name.lower()
        m = DNS_FAST.match(name)
        if m and 1 <= int(m.group(1)) <= 254:
            return [(name, 1, ip_bytes("10.9.0.%s" % m.group(1)))]
        if name in DNS_CNAMES:
            target = DNS_CNAMES[name]
            return [(name, 5, dns_encode(target))] + [(target, 1, ip_bytes(a))
                                                      for a in DNS_NAMES[target]]
        if name in DNS_NAMES:
            return [(name, 1, ip_bytes(a)) for a in DNS_NAMES[name]]
        return None

    def handle(self, peer, src, sport, dst, data):
        q = dns_name(data, 12) if len(data) >= 12 else None
        ident, flags, qd = struct.unpack_from("!HHH", data, 0) if len(data) >= 6 else (0, 0, 0)
        if q is None or flags & 0x8000 or qd != 1 or q[1] + 4 > len(data):
            self.count("bad")
            return None
        self.count("queries")
        name, end = q
        qtype = struct.unpack_from("!H", data, end)[0]
        if name.lower() == DNS_SLOW:
            self.count("slow")
            return None
        recs = self.records(name)
        if recs is not None and qtype != 1:
            recs = [r for r in recs if r[1] == qtype]
        rcode = 3 if recs is None else 0
        self.count("nxdomain" if recs is None else "answered")
        out = struct.pack("!HHHHHH", ident, 0x8080 | (flags & 0x0100) | rcode, 1,
                          len(recs or []), 0, 0) + data[12:end + 4]
        for owner, rtype, rdata in recs or []:
            out += dns_encode(owner) + struct.pack("!HHIH", rtype, 1, DNS_TTL, len(rdata)) + rdata
        return out


# ---- SNTP -------------------------------------------------------------------------

NTP_UNIX = 2208988800   # seconds from 1900 to 1970
NTP_YEAR = 365 * 86400


def ntp_stamp(unix):
    """An NTP timestamp (era 0 or 1: seconds mod 2**32) of Unix seconds."""
    secs = int(unix)
    frac = int((unix - secs) * (1 << 32)) & 0xffffffff
    return ((secs + NTP_UNIX) & 0xffffffff) << 32 | frac


class NtpServer:
    """Port 123: the time `unix` at the peer's start, running from there
    (RFC 4330 server mode, stratum 2). forge: a forged reply first."""

    def __init__(self, peer, unix, forge=False):
        self.unix, self.start, self.forge = unix, time.monotonic(), forge
        for k in ("ntp_queries", "ntp_answered", "ntp_forged"):
            peer.counts[k] = 0

    def now(self):
        return self.unix + (time.monotonic() - self.start)

    def reply(self, req, t, origin_flip=0):
        version = (req[0] >> 3) & 7
        return (struct.pack("!BBbbII4s", version << 3 | 4, 2, 6, -20, 1 << 8, 1 << 8, b"JAMT") +
                struct.pack("!QQQQ", ntp_stamp(t - 16), struct.unpack_from("!Q", req, 40)[0] ^
                            origin_flip, ntp_stamp(t), ntp_stamp(t)))

    def handle(self, peer, src, sport, dst, data):
        if len(data) < 48 or data[0] & 7 != 3:
            return None
        peer.counts["ntp_queries"] += 1
        if self.forge and peer.guest_mac is not None:
            bad = self.reply(data, self.now() + NTP_YEAR, origin_flip=1)
            peer.send(eth(peer.guest_mac, PEER_MAC, ETH_IPV4,
                          ipv4(dst, src, 17, udp(dst, src, 123, sport, bad))))
            peer.counts["ntp_forged"] += 1
        peer.counts["ntp_answered"] += 1
        return self.reply(data, self.now())


def add_dhcp_dns(peer, lease=3600):
    """The peer's DHCP server on port 67 and DNS server on port 53."""
    peer.add_udp(67, DhcpServer(peer, lease).handle)
    peer.add_udp(53, DnsServer(peer).handle)


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
    selftest_dhcp_dns(g, devnull, expect)
    selftest_ntp(g, devnull, expect)
    selftest_untagged(g, devnull, expect)
    problem = tcp_selftest()
    expect(problem is None, problem or "")
    problem = relay_selftest()
    expect(problem is None, problem or "")
    for f in fails:
        print("netpeer selftest: FAILED: " + f)
    print("netpeer selftest: %s" % ("PASS" if not fails else "FAIL"))
    return 0 if not fails else 1


def selftest_untagged(g, devnull, expect):
    """The untagged mode (--vlan none) against the fake guest g: answers go
    out untagged, untagged frames are good, every tagged one is bad (VLAN
    21's too), and the noise to drop is tagged."""
    peer = Peer(0, g.getsockname()[1], None, devnull)
    g.connect(("127.0.0.1", peer.listen))
    gmac, gip = bytes.fromhex("525400123457"), ip_bytes("10.2.21.51")
    req = eth(BROADCAST, gmac, ETH_ARP, arp(1, gmac, gip, b"\0" * 6, ip_bytes("10.2.21.1")))
    g.send(req)
    peer.poll(1)
    try:
        r = g.recv(65536)
        expect(classify(r)[0] == "untagged" and r[:6] == gmac and r[22:28] == PEER_MAC,
               "untagged: the ARP reply goes untagged")
    except socket.timeout:
        expect(False, "untagged: no ARP reply")
    expect(peer.counts["good"] == 1 and peer.counts["bad"] == 0, "untagged: the request is good")
    for f in (tag(req, VLAN), tag(req, 10), tag(req, 0), req[:12] +
              struct.pack("!HH", TPID_8021AD, 21) + tag(req, 21)[12:], req[:10],
              req + b"\0" * 1500):
        peer.handle(f)
    expect(peer.counts["bad"] == 6 and peer.counts["good"] == 1, "untagged: tagged frames bad")
    expect(peer.counts["arp_replies"] == 1, "untagged: a tagged request is never answered")
    expect(peer.summary()["result"] == "FAIL", "untagged: a tagged frame fails the run")
    peer.noise()
    kinds = []
    for _ in range(5):
        try:
            k = classify(g.recv(65536))
            kinds.append(k[0] if k[0] != "vlan" else "vlan %d" % k[1])
        except socket.timeout:
            break
    expect(kinds == ["vlan 21", "vlan 10", "priority", "outer", "untagged"],
           "untagged noise: %s" % kinds)
    expect(parse_vlan("none") is None and parse_vlan("21") == 21, "--vlan's words")


def selftest_ntp(g, devnull, expect):
    """The SNTP server (--ntp, --ntp-forge) against the fake guest g."""
    peer = Peer(0, g.getsockname()[1], VLAN, devnull)
    peer.add_udp(123, NtpServer(peer, 1930000000.25, forge=True).handle)
    g.connect(("127.0.0.1", peer.listen))
    gmac, gip, gw = bytes.fromhex("525400abcd01"), ip_bytes("10.2.21.100"), ip_bytes("10.2.21.1")
    req = bytes([0x23]) + bytes(39) + struct.pack("!Q", 0x0123456789abcdef)
    g.send(tag(eth(PEER_MAC, gmac, ETH_IPV4, ipv4(gip, gw, 17, udp(gip, gw, 50000, 123, req))),
               VLAN))
    peer.poll(1)
    got = []
    for _ in range(2):
        try:
            u = untag(g.recv(65536))
            got.append(u[42:] if struct.unpack_from("!HH", u, 34) == (123, 50000) else b"")
        except socket.timeout:
            break
    expect(len(got) == 2 and all(len(r) == 48 for r in got), "NTP: a forged reply and a good one")
    if len(got) == 2 and all(len(r) == 48 for r in got):
        forged, good = got
        origin = struct.unpack_from("!Q", good, 24)[0]
        t3 = struct.unpack_from("!Q", good, 40)[0]
        expect(good[0] == 0x24 and good[1] == 2 and origin == 0x0123456789abcdef,
               "NTP: the good reply's header and origin")
        expect(abs((t3 >> 32) - (1930000000 + NTP_UNIX) % (1 << 32)) <= 2, "NTP: its time")
        expect(struct.unpack_from("!Q", forged, 24)[0] == origin ^ 1, "NTP: the forged origin")
    expect(peer.counts["ntp_queries"] == 1 and peer.counts["ntp_forged"] == 1 and
           peer.counts["bad"] == 0, "NTP's counts")


def selftest_dhcp_dns(g, devnull, expect):
    """The DHCP and DNS servers against the fake guest g (a socket)."""
    peer = Peer(0, g.getsockname()[1], VLAN, devnull)
    add_dhcp_dns(peer, lease=60)
    g.connect(("127.0.0.1", peer.listen))
    gmac, zero, bcast = bytes.fromhex("525400abcdef"), b"\0" * 4, b"\xff" * 4

    def dhcp(mtype, ciaddr=zero, src=zero, dst=bcast, opts=b""):
        msg = (struct.pack("!BBBB4sHH4s4s4s4s", 1, 1, 6, 0, b"jamx", 0, 0x8000, ciaddr, zero,
                           zero, zero) + gmac + b"\0" * 202 + struct.pack("!I", DHCP_COOKIE) +
               struct.pack("!BBB", 53, 1, mtype) + opts + b"\xff")
        dmac = PEER_MAC if dst != bcast else BROADCAST
        g.send(tag(eth(dmac, gmac, ETH_IPV4, ipv4(src, dst, 17, udp(src, dst, 68, 67, msg))), VLAN))
        peer.poll(0.5)
        try:
            u = untag(g.recv(65536))
        except socket.timeout:
            return None
        return u, dhcp_options(u[42:])

    def dns(name, port=5353):
        q = struct.pack("!HHHHHH", 0x1234, 0x0100, 1, 0, 0, 0) + dns_encode(name) + b"\0\1\0\1"
        src = ip_bytes("10.2.21.100")
        g.send(tag(eth(PEER_MAC, gmac, ETH_IPV4, ipv4(src, ip_bytes(DHCP_SERVER), 17,
                                                      udp(src, ip_bytes(DHCP_SERVER), port, 53,
                                                          q))), VLAN))
        peer.poll(0.5)
        try:
            u = untag(g.recv(65536))
        except socket.timeout:
            return None
        return u[42:] if struct.unpack_from("!HH", u, 34) == (53, port) else b""

    g.settimeout(0.5)
    r = dhcp(1)   # DISCOVER: an OFFER of 10.2.21.100, broadcast, with every option
    expect(r and r[0][:6] == BROADCAST and r[0][30:34] == bcast and r[1].get(53) == b"\2" and
           r[0][58:62] == ip_bytes("10.2.21.100") and r[1].get(1) == ip_bytes("255.255.255.0") and
           r[1].get(3) == r[1].get(6) == r[1].get(54) == ip_bytes(DHCP_SERVER) and
           r[1].get(51) == struct.pack("!I", 60), "DHCP: the OFFER")
    sid = struct.pack("!BB4s", 54, 4, ip_bytes(DHCP_SERVER))
    r = dhcp(3, opts=struct.pack("!BB4s", 50, 4, ip_bytes("10.2.21.100")) + sid)
    expect(r and r[1].get(53) == b"\5" and r[0][58:62] == ip_bytes("10.2.21.100"), "DHCP: the ACK")
    r = dhcp(3, opts=struct.pack("!BB4s", 50, 4, ip_bytes("10.2.21.99")))   # INIT-REBOOT, not ours
    expect(r and r[1].get(53) == b"\6" and r[0][:6] == BROADCAST, "DHCP: a NAK")
    other = struct.pack("!BB4s", 54, 4, ip_bytes("10.2.21.2"))
    expect(dhcp(3, opts=struct.pack("!BB4s", 50, 4, ip_bytes("10.2.21.100")) + other) is None,
           "DHCP: a REQUEST to another server: no answer")
    me = ip_bytes("10.2.21.100")   # a renewal: unicast both ways
    r = dhcp(3, ciaddr=me, src=me, dst=ip_bytes(DHCP_SERVER))
    expect(r and r[1].get(53) == b"\5" and r[0][:6] == gmac and r[0][30:34] == me,
           "DHCP: a renewal's ACK, unicast")
    expect(peer.counts["dhcp_offers"] == 1 and peer.counts["dhcp_acks"] == 2 and
           peer.counts["dhcp_naks"] == 1 and peer.counts["dhcp_requests"] == 4, "DHCP's counts")
    a = dns("one.one.one.one")
    expect(a and struct.unpack_from("!HHHH", a, 0) == (0x1234, 0x8180, 1, 2) and
           ip_bytes("1.1.1.1") in a and ip_bytes("1.0.0.1") in a, "DNS: one.one.one.one")
    a = dns("www.jam")
    expect(a and struct.unpack_from("!H", a, 6)[0] == 2 and dns_encode("mac.jam") in a and
           a.endswith(ip_bytes("10.2.21.174")), "DNS: a CNAME and its A record")
    a = dns("fast7.jam")
    expect(a and a.endswith(ip_bytes("10.9.0.7")), "DNS: fast7.jam")
    a = dns(DNS_NONE)
    expect(a and struct.unpack_from("!H", a, 2)[0] & 15 == 3, "DNS: NXDOMAIN")
    expect(dns(DNS_SLOW) is None and peer.counts["dns_slow"] == 1, "DNS: the slow name unanswered")
    expect(peer.counts["dns_queries"] == 5 and peer.counts["bad"] == 0, "DNS's counts")
    g.settimeout(2)


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


def update_handler(spec, log):
    """--update SPEC: port 5022 answered by tools/update-server.py's
    peer_handler (a build, and a plan of damaged ones for the tests)."""
    import importlib.util
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "update-server.py")
    s = importlib.util.spec_from_file_location("update_server", path)
    mod = importlib.util.module_from_spec(s)
    s.loader.exec_module(mod)
    return mod.peer_handler(spec, log)


def tcp_module():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tcppeer.py")
    s = importlib.util.spec_from_file_location("tcppeer", path)
    mod = importlib.util.module_from_spec(s)
    s.loader.exec_module(mod)
    return mod


def tcp_selftest():
    """tools/tcppeer.py's own test: its server and client over a lossy wire."""
    return tcp_module().selftest(sys.modules[__name__])


def relay_module():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tcprelay.py")
    s = importlib.util.spec_from_file_location("tcprelay", path)
    mod = importlib.util.module_from_spec(s)
    s.loader.exec_module(mod)
    return mod


def relay_selftest():
    """tools/tcprelay.py's own test: both ways against tcppeer.py's TCP."""
    return relay_module().selftest(sys.modules[__name__], tcp_module())


def relay_side(peer, a):
    """--tcp-relay, --tcp-forward and --udp-relay: tools/tcprelay.py's Relay on peer."""
    mod = relay_module()
    np = sys.modules[__name__]
    return mod.Relay(peer, np, mod.parse_relays(a.tcp_relay),
                     mod.parse_forwards(a.tcp_forward, np), mod.parse_relays(a.udp_relay))


def tcp_side(peer, a):
    """--tcp-serve and --tcp-connect: tools/tcppeer.py's Tcp on peer."""
    mod = tcp_module()
    serve = connect = None
    if a.tcp_serve:
        port, nbytes = a.tcp_serve.split(":")
        serve = (int(port), int(nbytes))
    if a.tcp_connect:
        addr, port, conns, nbytes = a.tcp_connect.split(":")
        connect = (ip_bytes(addr), int(port), int(conns), int(nbytes))
    return mod.Tcp(peer, sys.modules[__name__], serve, connect)


def run(a):
    log = open(a.log, "a") if a.log else None
    peer = Peer(a.listen, a.qemu, a.vlan, log)
    if a.tcp_serve or a.tcp_connect:
        peer.tcp = tcp_side(peer, a)
    if a.tcp_relay or a.tcp_forward or a.udp_relay:
        peer.relay = relay_side(peer, a)
    add_dhcp_dns(peer, a.dhcp_lease)
    if a.netlog:
        b, _, secs = (a.netlog_pause or "").partition(":")
        add_netlog(peer, a.netlog, a.netlog_late,
                   (int(b), float(secs)) if a.netlog_pause else None)
    if a.update:
        peer.add_udp(5022, update_handler(a.update, peer.log))
    if a.ntp is not None:
        peer.add_udp(123, NtpServer(peer, a.ntp, a.ntp_forge).handle)
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    signal.signal(signal.SIGINT, lambda *_: stop.append(1))
    if a.ready:
        with open(a.ready, "w") as f:
            f.write("%d\n" % peer.listen)
    peer.log("listening on 127.0.0.1:%d, QEMU at %d, %s" % (peer.listen, a.qemu,
                                                            vlan_name(a.vlan)))
    end = time.monotonic() + a.duration if a.duration else None
    next_noise = time.monotonic() + a.noise if a.noise else None
    next_ping = time.monotonic() + a.ping_every if a.ping else None
    flood_at, flood_step = time.monotonic(), 0.05
    if a.late_after:
        peer.late_after = a.late_after
        peer.counts["pings_late"] = peer.counts["ping_replies_late"] = 0
    while not stop and (end is None or time.monotonic() < end):
        fds = [peer.sock] + ([sys.stdin] if a.stdin else [])
        rr, rw = peer.relay.fds() if peer.relay is not None else ([], [])
        busy = a.flood or peer.tcp or peer.relay
        r, w, _ = select.select(fds + rr, rw, [], 0.02 if busy else 0.2)
        if peer.sock in r:
            peer.poll(0)
        if peer.tcp is not None:
            peer.tcp.tick()
        if peer.relay is not None:
            peer.relay.service(r, w)
            peer.relay.tick()
        if a.stdin and sys.stdin in r:
            line = sys.stdin.readline()
            if not line or not stdin_command(peer, line, a):
                break
        if next_ping and time.monotonic() >= next_ping:
            peer.ping(a.ping)
            next_ping += a.ping_every
        if a.flood and a.ping and time.monotonic() >= flood_at:
            due = int((time.monotonic() - flood_at) / flood_step) + 1
            peer.flood(a.ping, max(1, round(a.flood * flood_step)) * min(due, 20))
            flood_at += flood_step * due
        if next_noise and time.monotonic() >= next_noise:
            peer.noise()
            next_noise += a.noise
    peer.poll(0)
    if peer.tcp is not None:
        peer.tcp.report()
    if peer.relay is not None:
        peer.relay.close()
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
    ap.add_argument("--vlan", type=parse_vlan, default=VLAN, help="1..4094, or none")
    ap.add_argument("--dhcp-lease", type=int, default=3600)
    ap.add_argument("--expect-none", action="store_true")
    ap.add_argument("--noise", type=float, default=0)
    ap.add_argument("--duration", type=float, default=0)
    ap.add_argument("--stdin", action="store_true")
    ap.add_argument("--summary")
    ap.add_argument("--ready")
    ap.add_argument("--log")
    ap.add_argument("--ping")
    ap.add_argument("--ping-every", type=float, default=0.5)
    ap.add_argument("--flood", type=float, default=0, help="frames a second (with --ping)")
    ap.add_argument("--late-after", type=float, default=0,
                    help="count the pings from S seconds after the first apart")
    ap.add_argument("--netlog", help="answer netlog (port 5021) into this folder")
    ap.add_argument("--netlog-late", type=float, default=0)
    ap.add_argument("--netlog-pause", help="BYTES:SECONDS")
    ap.add_argument("--update", metavar="SPEC")
    ap.add_argument("--ntp", type=float, metavar="UNIX", help="answer SNTP with this time")
    ap.add_argument("--ntp-forge", action="store_true", help="a forged SNTP reply first")
    ap.add_argument("--tcp-serve", metavar="PORT:BYTES", help="a TCP server (tools/tcppeer.py)")
    ap.add_argument("--tcp-connect", metavar="ADDR:PORT:CONNS:BYTES",
                    help="TCP clients of the guest's listener (tools/tcppeer.py)")
    ap.add_argument("--tcp-relay", metavar="PORT:HOSTPORT,...",
                    help="the guest's connections to PORT go to 127.0.0.1:HOSTPORT")
    ap.add_argument("--tcp-forward", metavar="LPORT:ADDR:PORT,...",
                    help="connections to 127.0.0.1:LPORT go to the guest's ADDR:PORT")
    ap.add_argument("--udp-relay", metavar="PORT:HOSTPORT,...",
                    help="the guest's datagrams to PORT go to 127.0.0.1:HOSTPORT")
    ap.add_argument("--free-ports", type=int, default=0)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.free_ports:
        print(" ".join(str(p) for p in free_ports(a.free_ports)))
        return 0
    if not a.qemu:
        ap.error("--qemu <port> is needed")
    return run(a)


if __name__ == "__main__":
    sys.exit(main())
