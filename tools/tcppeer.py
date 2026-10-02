"""The network peer's TCP side (tools/netpeer.py --tcp-serve and
--tcp-connect; docs/TESTING.md "The network peer"): a small TCP of its
own over the peer's frames on the VLAN, enough to test Jam OS's TCP from
outside (tools/tcp-test.sh, bin/tcptest in the guest).

A stream is bytes of a known pattern: byte i of stream `seed` is
(i * 131 + (i >> 8) * 7 + seed) & 0xff, as bin/tcptest makes and checks
them.
  --tcp-serve PORT:BYTES      a server on PORT of any address (10.2.21.174,
                              the Mac's, is where the guest connects): each
                              connection's BYTES of stream 0xa1 read to the
                              guest's FIN and checked byte by byte, then
                              BYTES of stream 0xb2 sent and a FIN.
  --tcp-connect ADDR:PORT:CONNS:BYTES
                              CONNS connections to the guest's listener at
                              ADDR:PORT, all at once from ports 40000 and up
                              (a SYN again each second until one is
                              answered): from port P, BYTES of stream P & 0xff
                              and a FIN sent, BYTES of stream (P + 1) & 0xff
                              read to the guest's FIN and checked.
Counted in the peer's summary: tcp_ok (connections whose bytes both ways
were right and whose FINs were both acked), tcp_bad (wrong bytes, a reset,
a bad checksum), tcp_bytes_in, tcp_bytes_out, tcp_retransmits.

The TCP is a test peer's, not a general one: in-order receiving (an
out-of-order segment is dropped and answered with a duplicate ACK), every
segment acked at once, a window of 65535 bytes; sending up to the guest's
window in segments of its MSS, go-back-N when nothing was acked for RTO
seconds, and a one-byte probe of a zero window at that pace; FIN both
ways."""
import random
import struct
import time

MSS = 1460
WINDOW = 65535
RTO = 0.25           # seconds without an ACK before sending again
SYN_EVERY = 1.0      # a client's SYN, again, until it is answered
F_FIN, F_SYN, F_RST, F_PSH, F_ACK = 0x01, 0x02, 0x04, 0x08, 0x10
SERVE_RX_SEED, SERVE_TX_SEED = 0xA1, 0xB2
CLIENT_PORT0 = 40000
M32 = 0xFFFFFFFF

_blocks = {}


def block(seed):
    """The first 65536 bytes of stream `seed` (it repeats after them)."""
    if seed not in _blocks:
        _blocks[seed] = bytes(((i * 131 + (i >> 8) * 7 + seed) & 0xFF) for i in range(65536))
    return _blocks[seed]


def stream(seed, off, n):
    """Bytes off..off+n of stream `seed`."""
    b, out = block(seed), bytearray()
    while n:
        k = off % 65536
        m = min(n, 65536 - k)
        out += b[k:k + m]
        off, n = off + m, n - m
    return bytes(out)


def diff(a, b):
    """a - b in sequence space, as a signed number."""
    d = (a - b) & M32
    return d - (1 << 32) if d >> 31 else d


class Conn:
    """One connection: the peer's half."""

    def __init__(self, tcp, mine, theirs, mac, seeds, nbytes, active):
        self.tcp, self.mine, self.theirs, self.mac = tcp, mine, theirs, mac   # (addr, port) pairs
        self.rx_seed, self.tx_seed = seeds
        self.nbytes, self.active = nbytes, active
        self.iss = random.getrandbits(32)
        self.snd_una = self.snd_nxt = self.iss
        self.rcv_nxt, self.peer_wnd, self.peer_mss = 0, 0, 536
        self.state = "SYN_SENT" if active else "SYN_RCVD"
        self.got, self.peer_fin, self.fin_sent, self.done, self.bad = 0, False, False, False, ""
        self.sending = active      # a client sends at once; the server after the guest's FIN
        self.last = time.monotonic()

    # the wire
    def seg(self, flags, seq, data=b"", syn_opts=False):
        opts = struct.pack("!BBH", 2, 4, MSS) if syn_opts else b""
        hdr = struct.pack("!HHIIBBHHH", self.mine[1], self.theirs[1], seq & M32,
                          self.rcv_nxt & M32 if flags & F_ACK else 0, (20 + len(opts)) // 4 << 4,
                          flags, WINDOW, 0, 0) + opts
        self.tcp.send_segment(self, hdr, data)

    def ack(self):
        self.seg(F_ACK, self.snd_nxt)

    def data_end(self):
        return (self.iss + 1 + self.nbytes) & M32

    # sending
    def pump(self):
        if self.state != "ESTABLISHED" or not self.sending:
            return
        while diff(self.data_end(), self.snd_nxt) > 0:
            room = self.peer_wnd - diff(self.snd_nxt, self.snd_una)
            if room <= 0:
                break
            off = diff(self.snd_nxt, self.iss + 1)
            n = min(self.peer_mss, room, self.nbytes - off)
            self.seg(F_ACK | F_PSH, self.snd_nxt, stream(self.tx_seed, off, n))
            self.tcp.count("tcp_bytes_out", n)
            self.snd_nxt = (self.snd_nxt + n) & M32
        if self.snd_nxt == self.data_end() and not self.fin_sent:
            self.seg(F_ACK | F_FIN, self.snd_nxt)
            self.snd_nxt = (self.snd_nxt + 1) & M32
            self.fin_sent = True

    def tick(self, now):
        if self.done or now - self.last < (SYN_EVERY if self.state == "SYN_SENT" else RTO):
            return
        self.last = now
        if self.state == "SYN_SENT":
            self.seg(F_SYN, self.iss, syn_opts=True)
            self.snd_nxt = (self.iss + 1) & M32
        elif self.state == "SYN_RCVD":
            self.seg(F_SYN | F_ACK, self.iss, syn_opts=True)
        elif self.snd_nxt != self.snd_una:   # go back to what was acked
            self.tcp.count("tcp_retransmits")
            self.snd_nxt, self.fin_sent = self.snd_una, False
            self.pump()
        elif self.sending and self.peer_wnd == 0 and diff(self.data_end(), self.snd_nxt) > 0:
            off = diff(self.snd_nxt, self.iss + 1)   # a zero window: one byte, to hear it open
            self.seg(F_ACK, self.snd_nxt, stream(self.tx_seed, off, 1))
            self.snd_nxt = (self.snd_nxt + 1) & M32

    # receiving
    def input(self, flags, seq, ack, wnd, opts, data):
        if flags & F_RST:
            if self.state != "SYN_SENT":
                self.fail("a reset")
            return
        if self.state == "SYN_SENT":
            if flags & F_SYN and flags & F_ACK and ack == (self.iss + 1) & M32:
                self.rcv_nxt, self.state = (seq + 1) & M32, "ESTABLISHED"
                self.syn_done(ack, wnd, opts)
                self.pump()
            return
        if self.state == "SYN_RCVD":
            if flags & F_ACK and ack == (self.iss + 1) & M32:
                self.state = "ESTABLISHED"
                self.syn_done(ack, wnd, opts)
            elif flags & F_SYN:
                self.seg(F_SYN | F_ACK, self.iss, syn_opts=True)
                return
        if flags & F_ACK and 0 < diff(ack, self.snd_una) <= diff(self.snd_nxt, self.snd_una):
            self.snd_una, self.last = ack, time.monotonic()
        if flags & F_ACK:
            self.peer_wnd = wnd
        self.take(flags, seq, data)
        self.pump()
        self.check_done()

    def syn_done(self, ack, wnd, opts):
        self.snd_una = self.snd_nxt = ack
        self.peer_wnd = wnd
        self.peer_mss = opts.get(2, 536)
        self.last = time.monotonic()
        self.ack()

    def take(self, flags, seq, data):
        if not data and not flags & F_FIN:
            return
        if seq != self.rcv_nxt or self.peer_fin:
            self.ack()   # out of order, or a repeat: the ACK says where we are
            return
        if data:
            if data != stream(self.rx_seed, self.got, len(data)):
                self.fail("wrong bytes at %d" % self.got)
            self.got += len(data)
            self.tcp.count("tcp_bytes_in", len(data))
            self.rcv_nxt = (self.rcv_nxt + len(data)) & M32
        if flags & F_FIN:
            self.rcv_nxt = (self.rcv_nxt + 1) & M32
            self.peer_fin = True
            if self.got != self.nbytes:
                self.fail("a FIN after %d of %d bytes" % (self.got, self.nbytes))
            self.sending = True
        self.ack()

    def check_done(self):
        acked = self.fin_sent and self.snd_una == self.snd_nxt
        if not self.done and self.peer_fin and acked and not self.bad:
            self.done = True
            self.tcp.count("tcp_ok")

    def fail(self, why):
        if not self.bad:
            self.bad = why
            self.tcp.count("tcp_bad")
            self.tcp.peer.log("tcp %s:%d: %s" % (self.tcp.ip_str(self.theirs[0]), self.theirs[1],
                                                 why))


class Tcp:
    """The peer's TCP: a server (serve = (port, bytes)) and clients
    (connect = (addr, port, conns, bytes)) over a netpeer.Peer."""

    def __init__(self, peer, np, serve=None, connect=None):
        self.peer, self.np = peer, np
        self.serve, self.connect = serve, connect
        self.conns = {}                       # (their addr, their port, our port) -> Conn
        self.connect_started = False
        for k in ("tcp_ok", "tcp_bad", "tcp_bytes_in", "tcp_bytes_out", "tcp_retransmits"):
            peer.counts[k] = 0

    def count(self, what, n=1):
        self.peer.counts[what] += n

    def ip_str(self, b):
        return self.np.ip_str(b)

    def send_segment(self, c, hdr, data):
        seg = hdr + data
        pseudo = c.mine[0] + c.theirs[0] + struct.pack("!BBH", 0, 6, len(seg))
        s = self.np.checksum(pseudo + seg)
        seg = seg[:16] + struct.pack("!H", s) + seg[18:]
        self.peer.send(self.np.eth(c.mac, self.np.PEER_MAC, self.np.ETH_IPV4,
                                   self.np.ipv4(c.mine[0], c.theirs[0], 6, seg)))

    def input(self, mac, src, dst, body):
        """A TCP segment from the guest (body: from the TCP header on)."""
        pseudo = src + dst + struct.pack("!BBH", 0, 6, len(body))
        if len(body) < 20 or self.np.checksum(pseudo + body):
            self.count("tcp_bad")
            return
        sport, dport, seq, ack, off, flags, wnd = struct.unpack_from("!HHIIBBH", body, 0)
        hl = (off >> 4) * 4
        if hl < 20 or hl > len(body):
            self.count("tcp_bad")
            return
        opts, k = {}, 20
        while k + 1 < hl and body[k] != 0:
            if body[k] == 1:
                k += 1
                continue
            if body[k + 1] < 2:
                break
            if body[k] == 2 and body[k + 1] == 4:
                opts[2] = struct.unpack_from("!H", body, k + 2)[0]
            k += body[k + 1]
        c = self.conns.get((src, sport, dport))
        if c is None and self.serve and dport == self.serve[0] and flags & F_SYN \
                and not flags & F_ACK:
            c = Conn(self, (dst, dport), (src, sport), mac, (SERVE_RX_SEED, SERVE_TX_SEED),
                     self.serve[1], False)
            c.rcv_nxt, c.peer_wnd, c.peer_mss = (seq + 1) & M32, wnd, opts.get(2, 536)
            self.conns[(src, sport, dport)] = c
            c.seg(F_SYN | F_ACK, c.iss, syn_opts=True)
            c.snd_nxt = (c.iss + 1) & M32
            return
        if c is not None:
            c.input(flags, seq, ack, wnd, opts, body[hl:])

    def tick(self):
        now = time.monotonic()
        if self.connect and not self.connect_started and self.peer.guest_mac is not None:
            self.connect_started = True
            addr, port, n, nbytes = self.connect
            for i in range(n):
                p = CLIENT_PORT0 + i
                c = Conn(self, (self.np.ip_bytes(self.np.PING_FROM), p), (addr, port),
                         self.peer.guest_mac, ((p + 1) & 0xFF, p & 0xFF), nbytes, True)
                c.last = 0.0   # its first SYN now
                self.conns[(addr, port, p)] = c
        for c in list(self.conns.values()):
            c.tick(now)

    def report(self):
        """A line for each connection not done: where it stands."""
        for c in self.conns.values():
            if not c.done:
                self.peer.log("tcp %s:%d from %d: not done: %s, %d of %d bytes in%s, sent to %d "
                              "(acked %d) of %d%s, their window %d%s" % (
                                  self.ip_str(c.theirs[0]), c.theirs[1], c.mine[1], c.state,
                                  c.got, c.nbytes, ", their FIN" if c.peer_fin else "",
                                  diff(c.snd_nxt, c.iss + 1), diff(c.snd_una, c.iss + 1),
                                  c.nbytes, ", our FIN" if c.fin_sent else "", c.peer_wnd,
                                  ", " + c.bad if c.bad else ""))

    def busy(self):
        return any(not c.done for c in self.conns.values()) or \
            (self.connect and not self.connect_started)


class _Wire:
    """selftest's stand-in for netpeer.Peer: frames queued, nothing logged."""

    def __init__(self):
        self.counts, self.guest_mac, self.out = {}, bytes(6), []

    def send(self, frame):
        self.out.append(frame)

    def log(self, msg):
        pass


def selftest(np, nbytes=60000, drop=0.02, limit=20.0):
    """A server and a client of this TCP against each other, frames dropped
    at random: both ends' bytes right, both FINs acked. Returns a problem
    or None."""
    a, b = _Wire(), _Wire()
    server, client = Tcp(a, np, serve=(5030, nbytes)), Tcp(b, np)
    sa, ca = np.ip_bytes("10.2.21.174"), np.ip_bytes("10.2.21.5")
    c = Conn(client, (ca, CLIENT_PORT0), (sa, 5030), bytes(6), (SERVE_TX_SEED, SERVE_RX_SEED),
             nbytes, True)
    c.last = 0.0
    client.conns[(sa, 5030, CLIENT_PORT0)] = c
    rng, end = random.Random(21), time.monotonic() + limit
    while time.monotonic() < end and not (a.counts["tcp_ok"] and b.counts["tcp_ok"]):
        for wire, to in ((a, client), (b, server)):
            frames, wire.out = wire.out, []
            for f in frames:
                if rng.random() >= drop:
                    to.input(f[6:12], f[26:30], f[30:34], f[34:])
        server.tick()
        client.tick()
        if not a.out and not b.out:
            time.sleep(0.01)
    if a.counts["tcp_bad"] or b.counts["tcp_bad"]:
        return "tcp selftest: bad bytes or a reset"
    if not (a.counts["tcp_ok"] == 1 and b.counts["tcp_ok"] == 1):
        return "tcp selftest: not done in %d s (%s, %s)" % (limit, a.counts, b.counts)
    if a.counts["tcp_bytes_in"] != nbytes or b.counts["tcp_bytes_in"] != nbytes:
        return "tcp selftest: byte counts %s %s" % (a.counts, b.counts)
    return None
