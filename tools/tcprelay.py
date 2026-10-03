"""The network peer's TCP relay (tools/netpeer.py --tcp-relay and
--tcp-forward; docs/TESTING.md "The network peer"): real programs on the
Mac talk to Jam OS in QEMU over TCP, as if the Mac were on the VLAN.

QEMU's network here is the peer's frames (no host stack in between), so
the relay ends each TCP connection on the VLAN in a small TCP of its own
and joins it to an ordinary socket on 127.0.0.1:
  --tcp-relay PORT:HOSTPORT       the guest connects to PORT of any address
                                  (10.2.21.174, the Mac's, for the tests):
                                  the relay connects to 127.0.0.1:HOSTPORT
                                  (python3 -m http.server, tools/speed.py,
                                  a test's own server) and passes the bytes
                                  both ways, FIN for FIN.
  --tcp-forward LPORT:ADDR:PORT   the relay listens on 127.0.0.1:LPORT; each
                                  connection a program there makes (curl,
                                  tools/speed.py) becomes one from the
                                  Mac's address to the guest's ADDR:PORT.
  --udp-relay PORT:HOSTPORT       datagrams the guest sends to PORT of any
                                  address go to 127.0.0.1:HOSTPORT from a
                                  socket of the relay's (one for each of the
                                  guest's ports), and what comes back on it
                                  goes to the guest from that address and
                                  PORT (tools/speed.py's UDP side).
Each may be given more than once (comma-separated). A SYN from the guest
to a port nothing is relayed to is answered with a reset, as a closed port
on the Mac would be (`speed` to such a port says it was refused). Counted
in the peer's summary: relay_conns, relay_bytes_to_guest,
relay_bytes_from_guest, relay_resets, relay_refused, relay_dgrams_in,
relay_dgrams_out; relay_scaled (connections whose windows scaled), and the
most seen: relay_window (the guest's window, scaled), relay_out_flight (our
bytes the guest had not acked), relay_in_flight (the guest's bytes past
our last ACK).

The TCP is a test peer's, like tools/tcppeer.py's: in-order receiving (a
segment out of order is dropped and answered with a duplicate ACK), every
segment acked at once, our window the room left in the bytes waiting for
the Mac's socket (so a slow program on the Mac slows the guest's sender),
window scaling (RFC 7323: offered on our SYNs, used when both SYNs carry
it; then the window is up to HOST_READ_MAX); sending up to the guest's
window (at most FLIGHT_MAX in flight) in segments of its MSS, go-back-N
after RTO seconds without an ACK, a one-byte probe of a zero window at
that pace. A reset from either side resets the other."""
import errno
import random
import select
import socket
import struct
import threading
import time
import types

MSS = 1460
WINDOW = 65535          # our window at most unscaled (and in every SYN)
WSHIFT = 7              # our window scale, offered on every SYN we send
FLIGHT_MAX = 262144     # our bytes in flight at most: few enough frames for the guest's rings
RTO = 0.25              # seconds without an ACK before sending again
SYN_EVERY = 1.0         # an active open's SYN, again, until answered
HOST_READ_MAX = 262144  # bytes from the Mac's socket waiting for the guest, at most
F_FIN, F_SYN, F_RST, F_PSH, F_ACK = 0x01, 0x02, 0x04, 0x08, 0x10
M32 = 0xFFFFFFFF
FORWARD_PORT0 = 41000   # the Mac's ports for --tcp-forward's connections


def diff(a, b):
    """a - b in sequence space, as a signed number."""
    d = (a - b) & M32
    return d - (1 << 32) if d >> 31 else d


class Stream:
    """One relayed connection: our TCP end toward the guest, and the socket
    toward the program on the Mac."""

    def __init__(self, relay, mine, theirs, mac, sock, active, connecting=False):
        self.relay, self.mine, self.theirs, self.mac = relay, mine, theirs, mac
        self.sock, self.connecting = sock, connecting
        self.iss = random.getrandbits(32)
        self.snd_una = self.snd_nxt = self.iss
        self.rcv_nxt, self.peer_wnd, self.peer_mss = 0, 0, 536
        self.state = "SYN_SENT" if active else "SYN_RCVD"
        self.offer_ws = active       # we offer scaling as a client; as a server, if offered
        self.scaled, self.snd_shift = False, 0   # both SYNs had it; the guest's shift
        self.acked_out = 0           # the last ACK number we sent
        self.out = bytearray()       # bytes from the Mac, from snd_una on
        self.to_host = bytearray()   # bytes from the guest, not yet written to the Mac
        self.host_eof = False        # the Mac's program shut down its side
        self.guest_fin = False       # the guest sent its FIN (all its bytes are in to_host)
        self.host_shut = False       # we shut down the Mac's socket's write side
        self.fin_sent = False
        self.done = False
        self.last = 0.0 if active else time.monotonic()

    # the wire
    def window(self):
        return max(0, min(HOST_READ_MAX if self.scaled else WINDOW,
                          HOST_READ_MAX - len(self.to_host)))

    def seg(self, flags, seq, data=b"", syn_opts=False):
        opts = struct.pack("!BBH", 2, 4, MSS) if syn_opts else b""
        if syn_opts and self.offer_ws:   # NOP, then the window scale (RFC 7323 2.2)
            opts += struct.pack("!BBBB", 1, 3, 3, WSHIFT)
        wnd = self.window()
        wnd = min(wnd, WINDOW) if flags & F_SYN or not self.scaled else wnd >> WSHIFT
        if flags & F_ACK:
            self.acked_out = self.rcv_nxt
        hdr = struct.pack("!HHIIBBHHH", self.mine[1], self.theirs[1], seq & M32,
                          self.rcv_nxt & M32 if flags & F_ACK else 0, (20 + len(opts)) // 4 << 4,
                          flags, min(wnd, 0xFFFF), 0, 0) + opts
        self.relay.send_segment(self, hdr, data)

    def scale(self, opts):
        """The guest's SYN (or SYN-ACK) said whether it scales: both must."""
        self.scaled = self.offer_ws and 3 in opts
        self.snd_shift = min(opts.get(3, 0), 14)
        if self.scaled:
            self.relay.count("relay_scaled")

    def ack(self):
        self.seg(F_ACK, self.snd_nxt)

    def reset(self, why):
        if not self.done:
            self.seg(F_RST | F_ACK, self.snd_nxt)
            self.relay.count("relay_resets")
            self.relay.peer.log("relay %s: reset: %s" % (self.name(), why))
        self.finish()

    def finish(self):
        self.done = True
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def name(self):
        return "%s:%d<->%s:%d" % (self.relay.np.ip_str(self.mine[0]), self.mine[1],
                                  self.relay.np.ip_str(self.theirs[0]), self.theirs[1])

    # sending to the guest
    def pump(self):
        if self.state != "ESTABLISHED":
            return
        end = (self.snd_una + len(self.out)) & M32
        while diff(end, self.snd_nxt) > 0:
            room = min(self.peer_wnd, FLIGHT_MAX) - diff(self.snd_nxt, self.snd_una)
            if room <= 0:
                break
            off = diff(self.snd_nxt, self.snd_una)
            n = min(self.peer_mss, room, len(self.out) - off)
            self.seg(F_ACK | F_PSH, self.snd_nxt, bytes(self.out[off:off + n]))
            self.snd_nxt = (self.snd_nxt + n) & M32
        self.relay.most("relay_out_flight", diff(self.snd_nxt, self.snd_una))
        if self.host_eof and self.snd_nxt == end and not self.fin_sent:
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
            self.relay.count("relay_retransmits")
            self.snd_nxt, self.fin_sent = self.snd_una, False
            self.pump()
        elif self.peer_wnd == 0 and self.out:   # a zero window: one byte, to hear it open
            self.seg(F_ACK, self.snd_nxt, bytes(self.out[:1]))
            self.snd_nxt = (self.snd_nxt + 1) & M32

    # from the guest
    def input(self, flags, seq, ack, wnd, opts, data):
        if flags & F_RST:
            if self.state != "SYN_SENT" or ack == (self.iss + 1) & M32:
                self.relay.peer.log("relay %s: the guest reset it" % self.name())
                self.relay.count("relay_resets")
                self.finish()
            return
        if self.state == "SYN_SENT":
            if flags & F_SYN and flags & F_ACK and ack == (self.iss + 1) & M32:
                self.rcv_nxt, self.state = (seq + 1) & M32, "ESTABLISHED"
                self.scale(opts)
                self.established(ack, wnd, opts)
            return
        if self.scaled and not flags & F_SYN:   # a SYN's window is never scaled
            wnd <<= self.snd_shift
        if self.state == "SYN_RCVD":
            if flags & F_ACK and ack == (self.iss + 1) & M32:
                self.state = "ESTABLISHED"
                self.established(ack, wnd, {})
            elif flags & F_SYN:
                self.seg(F_SYN | F_ACK, self.iss, syn_opts=True)
                return
        if flags & F_ACK and 0 < diff(ack, self.snd_una) <= diff(self.snd_nxt, self.snd_una):
            n = diff(ack, self.snd_una)
            del self.out[:min(n, len(self.out))]   # the FIN's sequence number has no byte
            self.snd_una, self.last = ack, time.monotonic()
        if flags & F_ACK:
            self.peer_wnd = wnd
            self.relay.most("relay_window", wnd)
        self.take(flags, seq, data)
        self.pump()

    def established(self, ack, wnd, opts):
        self.snd_una = self.snd_nxt = ack
        self.peer_wnd = wnd
        if 2 in opts:
            self.peer_mss = opts[2]
        self.last = time.monotonic()
        self.ack()
        self.relay.count("relay_conns")

    def take(self, flags, seq, data):
        if not data and not flags & F_FIN:
            return
        if seq != self.rcv_nxt or self.guest_fin:
            self.ack()   # out of order, or a repeat: the ACK says where we are
            return
        kept = data[:self.window()]   # past our window: dropped, sent again later
        if kept:
            self.to_host += kept
            self.relay.count("relay_bytes_from_guest", len(kept))
            self.rcv_nxt = (self.rcv_nxt + len(kept)) & M32
            self.relay.most("relay_in_flight", diff(self.rcv_nxt, self.acked_out))
        if flags & F_FIN and len(kept) == len(data):   # the FIN comes after every byte
            self.rcv_nxt = (self.rcv_nxt + 1) & M32
            self.guest_fin = True
        self.ack()

    # the Mac's side
    def host_io(self, readable, writable):
        if self.sock is None or self.done:
            return
        if self.connecting and writable:
            err = self.sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
            if err:
                self.reset("connecting to the Mac's port: %s" % errno.errorcode.get(err, err))
                return
            self.connecting = False
        if self.connecting:
            return
        if readable and not self.host_eof and len(self.out) < HOST_READ_MAX:
            try:
                b = self.sock.recv(HOST_READ_MAX - len(self.out))
            except (BlockingIOError, InterruptedError):
                b = None
            except OSError as e:
                self.reset("reading the Mac's socket: %s" % e)
                return
            if b == b"":
                self.host_eof = True
            elif b:
                self.out += b
                self.relay.count("relay_bytes_to_guest", len(b))
        if writable and self.to_host:
            try:
                n = self.sock.send(bytes(self.to_host[:65536]))
                was_full = self.window() < MSS
                del self.to_host[:n]
                if was_full and self.window() >= MSS:
                    self.ack()   # our window opened: tell the guest
            except (BlockingIOError, InterruptedError):
                pass
            except OSError as e:
                self.reset("writing the Mac's socket: %s" % e)
                return
        if self.guest_fin and not self.to_host and not self.host_shut:
            try:
                self.sock.shutdown(socket.SHUT_WR)
            except OSError:
                pass
            self.host_shut = True
        self.pump()
        acked = self.fin_sent and self.snd_una == self.snd_nxt
        if acked and self.host_shut:
            self.relay.peer.log("relay %s: closed, both ways" % self.name())
            self.finish()

    def wants_read(self):
        return (self.sock is not None and not self.connecting and not self.host_eof and
                len(self.out) < HOST_READ_MAX)

    def wants_write(self):
        return self.sock is not None and (self.connecting or bool(self.to_host))


class Relay:
    """The relay over a netpeer.Peer: relays = {guest port: Mac port},
    forwards = [(Mac listening port, guest address bytes, guest port)]."""

    def __init__(self, peer, np, relays, forwards, udp=None):
        self.peer, self.np = peer, np
        self.relays = relays
        self.streams = {}            # (their addr, their port, our port) -> Stream
        self.listeners = []          # (socket, guest addr, guest port)
        self.udp_socks = {}          # socket -> (guest addr, guest port, our addr, our port)
        self.udp_by_guest = {}       # (guest addr, guest port, our addr, our port) -> socket
        self.next_port = FORWARD_PORT0
        for k in ("relay_conns", "relay_bytes_to_guest", "relay_bytes_from_guest",
                  "relay_resets", "relay_refused", "relay_retransmits", "relay_dgrams_in",
                  "relay_dgrams_out"):
            peer.counts[k] = 0
        for port, hostport in (udp or {}).items():
            peer.add_udp(port, self.udp_handler(port, hostport))
        for lport, gaddr, gport in forwards:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("127.0.0.1", lport))
            s.listen(16)
            s.setblocking(False)
            self.listeners.append((s, gaddr, gport))

    def count(self, what, n=1):
        self.peer.counts[what] = self.peer.counts.get(what, 0) + n

    def most(self, what, n):
        """The summary's `what`: the most n seen."""
        if n > self.peer.counts.get(what, 0):
            self.peer.counts[what] = n

    def send_segment(self, c, hdr, data):
        seg = hdr + data
        pseudo = c.mine[0] + c.theirs[0] + struct.pack("!BBH", 0, 6, len(seg))
        s = self.np.checksum(pseudo + seg)
        seg = seg[:16] + struct.pack("!H", s) + seg[18:]
        self.peer.send(self.np.eth(c.mac, self.np.PEER_MAC, self.np.ETH_IPV4,
                                   self.np.ipv4(c.mine[0], c.theirs[0], 6, seg)))

    def input(self, mac, src, dst, body):
        """A TCP segment from the guest: True if it is the relay's."""
        pseudo = src + dst + struct.pack("!BBH", 0, 6, len(body))
        if len(body) < 20 or self.np.checksum(pseudo + body):
            return False
        sport, dport, seq, ack, off, flags, wnd = struct.unpack_from("!HHIIBBH", body, 0)
        hl = (off >> 4) * 4
        if hl < 20 or hl > len(body):
            return False
        c = self.streams.get((src, sport, dport))
        if c is None and flags & F_SYN and not flags & F_ACK:
            if dport in self.relays:
                self.accept(mac, src, sport, dst, dport, seq, wnd, self.options(body, hl))
            else:
                self.refuse(mac, src, sport, dst, dport, seq)
            return True
        if c is None:
            return False
        c.input(flags, seq, ack, wnd, self.options(body, hl), body[hl:])
        return True

    @staticmethod
    def options(body, hl):
        opts, k = {}, 20
        while k + 1 < hl and body[k] != 0:
            if body[k] == 1:
                k += 1
                continue
            if body[k + 1] < 2:
                break
            if body[k] == 2 and body[k + 1] == 4 and k + 4 <= hl:
                opts[2] = struct.unpack_from("!H", body, k + 2)[0]
            if body[k] == 3 and body[k + 1] == 3 and k + 3 <= hl:
                opts[3] = body[k + 2]
            k += body[k + 1]
        return opts

    def refuse(self, mac, src, sport, dst, dport, seq):
        """The guest's SYN to a port nothing is relayed to: a reset that acks
        it, as a closed port answers (RFC 9293 3.10.7.1)."""
        hdr = struct.pack("!HHIIBBHHH", dport, sport, 0, (seq + 1) & M32, 5 << 4, F_RST | F_ACK,
                          0, 0, 0)
        self.send_segment(types.SimpleNamespace(mine=(dst, dport), theirs=(src, sport), mac=mac),
                          hdr, b"")
        self.count("relay_refused")

    def accept(self, mac, src, sport, dst, dport, seq, wnd, opts):
        """The guest's SYN to a relayed port: our SYN-ACK, and a connect to the Mac."""
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setblocking(False)
        try:
            s.connect(("127.0.0.1", self.relays[dport]))
        except BlockingIOError:
            pass
        except OSError as e:
            self.peer.log("relay: connecting to 127.0.0.1:%d: %s" % (self.relays[dport], e))
        c = Stream(self, (dst, dport), (src, sport), mac, s, False, connecting=True)
        c.offer_ws = 3 in opts   # answered in kind
        c.scale(opts)
        c.rcv_nxt, c.peer_wnd = (seq + 1) & M32, wnd
        c.peer_mss = opts.get(2, 536)
        self.streams[(src, sport, dport)] = c
        c.seg(F_SYN | F_ACK, c.iss, syn_opts=True)
        c.snd_nxt = (c.iss + 1) & M32
        return c

    def udp_handler(self, port, hostport):
        """netpeer's handler for datagrams to `port`: on to the Mac's hostport."""
        def handle(peer, src, sport, dst, data):
            key = (src, sport, dst, port)
            s = self.udp_by_guest.get(key)
            if s is None:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.connect(("127.0.0.1", hostport))
                s.setblocking(False)
                self.udp_by_guest[key] = s
                self.udp_socks[s] = key
            try:
                s.send(data)
                self.count("relay_dgrams_in")
            except OSError:
                pass   # the Mac's program isn't there: as a lost datagram
            return None
        return handle

    def udp_back(self, s):
        """What the Mac's program answered on s: to the guest."""
        src, sport, dst, port = self.udp_socks[s]
        for _ in range(64):
            try:
                data = s.recv(65536)
            except OSError:
                return
            if self.peer.guest_mac is None:
                continue
            self.peer.send(self.np.eth(self.peer.guest_mac, self.np.PEER_MAC, self.np.ETH_IPV4,
                                       self.np.ipv4(dst, src, 17,
                                                    self.np.udp(dst, src, port, sport, data))))
            self.count("relay_dgrams_out")

    def fds(self):
        """The sockets to watch: (readable, writable) lists."""
        r = [s for s, _, _ in self.listeners] + list(self.udp_socks)
        w = []
        for c in self.streams.values():
            if c.wants_read():
                r.append(c.sock)
            if c.wants_write():
                w.append(c.sock)
        return r, w

    def service(self, readable, writable):
        for s in list(self.udp_socks):
            if s in readable:
                self.udp_back(s)
        for s, gaddr, gport in self.listeners:
            if s in readable:
                self.forward(s, gaddr, gport)
        for c in list(self.streams.values()):
            if c.sock is not None:
                c.host_io(c.sock in readable, c.sock in writable)

    def forward(self, lsock, gaddr, gport):
        """A program on the Mac connected: open a connection to the guest for it."""
        if self.peer.guest_mac is None:
            return   # not yet: it waits in the listen queue
        try:
            s, _ = lsock.accept()
        except OSError:
            return
        s.setblocking(False)
        p = self.next_port
        self.next_port = FORWARD_PORT0 + (self.next_port - FORWARD_PORT0 + 1) % 20000
        mine = (self.np.ip_bytes(self.np.PING_FROM), p)
        c = Stream(self, mine, (gaddr, gport), self.peer.guest_mac, s, True)
        self.streams[(gaddr, gport, p)] = c

    def tick(self):
        now = time.monotonic()
        for k, c in list(self.streams.items()):
            c.tick(now)
            if c.done:
                del self.streams[k]

    def close(self):
        for s, _, _ in self.listeners:
            s.close()
        for s in self.udp_socks:
            s.close()
        for c in self.streams.values():
            c.finish()


def parse_relays(text):
    """"8000:18000,5201:15201" -> {8000: 18000, 5201: 15201}."""
    out = {}
    for part in filter(None, (text or "").split(",")):
        a, b = part.split(":")
        out[int(a)] = int(b)
    return out


def parse_forwards(text, np):
    """"18080:10.2.21.5:8080" -> [(18080, address bytes, 8080)]."""
    out = []
    for part in filter(None, (text or "").split(",")):
        lport, addr, gport = part.split(":")
        out.append((int(lport), np.ip_bytes(addr), int(gport)))
    return out


class _Wire:
    """selftest's stand-in for netpeer.Peer: frames queued, nothing logged."""

    def __init__(self):
        self.counts, self.guest_mac, self.out = {}, bytes.fromhex("020000000005"), []

    def send(self, frame):
        self.out.append(frame)

    def log(self, msg):
        pass

    def add_udp(self, port, fn):
        pass


def _host_side(kind, port, send_seed, recv_seed, nbytes, result, tcp):
    """The Mac's program: a client of --tcp-forward's port, or a server for
    --tcp-relay's: nbytes of stream send_seed out and a FIN, nbytes of
    recv_seed back to the end, checked."""
    try:
        if kind == "client":
            s = socket.create_connection(("127.0.0.1", port), timeout=20)
        else:
            ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            ls.bind(("127.0.0.1", port))
            ls.listen(1)
            ls.settimeout(20)
            s, _ = ls.accept()
            ls.close()
        s.settimeout(20)
        threading.Thread(target=lambda: (s.sendall(tcp.stream(send_seed, 0, nbytes)),
                                         s.shutdown(socket.SHUT_WR)), daemon=True).start()
        got = bytearray()
        while True:
            b = s.recv(65536)
            if not b:
                break
            got += b
        s.close()
        result.append("ok" if bytes(got) == tcp.stream(recv_seed, 0, nbytes) else
                      "%s: %d bytes back, wrong" % (kind, len(got)))
    except OSError as e:
        result.append("%s: %s" % (kind, e))


def _run(np, tcp, relay, guest, a, b, result, limit, drop):
    rng, end = random.Random(21), time.monotonic() + limit
    while time.monotonic() < end and not (result and b.counts["tcp_ok"]):
        for wire, to in ((a, guest), (b, relay)):
            frames, wire.out = wire.out, []
            for f in frames:
                if rng.random() >= drop:
                    to.input(f[6:12], f[26:30], f[30:34], f[34:])
        r, w = relay.fds()
        r, w, _ = select.select(r, w, [], 0.005)
        relay.service(r, w)
        relay.tick()
        guest.tick()
    relay.close()


def selftest(np, tcp, nbytes=300000, drop=0.02, limit=30.0):
    """The relay both ways against tools/tcppeer.py's TCP as the guest, over
    a lossy wire, with real sockets on 127.0.0.1 as the Mac's programs:
    every byte right both ways. Returns a problem or None."""
    lport, hport = np.free_ports(2)
    gip, pip = np.ip_bytes("10.2.21.5"), np.ip_bytes(np.PING_FROM)
    # --tcp-forward: a program connects; the guest serves (tcppeer's server)
    a, b, result = _Wire(), _Wire(), []
    relay = Relay(a, np, {}, [(lport, gip, 5030)])
    guest = tcp.Tcp(b, np, serve=[(5030, nbytes, 0.0)])
    t = threading.Thread(target=_host_side, args=("client", lport, tcp.SERVE_RX_SEED,
                                                  tcp.SERVE_TX_SEED, nbytes, result, tcp))
    t.start()
    _run(np, tcp, relay, guest, a, b, result, limit, drop)
    t.join(1)
    if result != ["ok"] or b.counts["tcp_ok"] != 1:
        return "relay selftest, forward: %s, %s" % (result, b.counts)
    # --tcp-relay: the guest connects (tcppeer's client) to the Mac's server
    a, b, result = _Wire(), _Wire(), []
    relay = Relay(a, np, {8000: hport}, [])
    t = threading.Thread(target=_host_side, args=("server", hport, (tcp.CLIENT_PORT0 + 1) & 0xFF,
                                                  tcp.CLIENT_PORT0 & 0xFF, nbytes, result, tcp))
    t.start()
    time.sleep(0.2)
    guest = tcp.Tcp(b, np, connect=(pip, 8000, 1, nbytes))
    b.guest_mac = bytes.fromhex("024a414d0001")
    _run(np, tcp, relay, guest, a, b, result, limit, drop)
    t.join(1)
    if result != ["ok"] or b.counts["tcp_ok"] != 1:
        return "relay selftest, relay: %s, %s" % (result, b.counts)
    if a.counts.get("relay_scaled") != 1 or a.counts.get("relay_window", 0) <= WINDOW:
        return "relay selftest: windows not scaled (%s)" % a.counts
    return None
