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
  --tcp-forward LPORT:ADDR:PORT[:LOSS]
                                  the relay listens on 127.0.0.1:LPORT; each
                                  connection a program there makes (curl,
                                  tools/speed.py) becomes one from the
                                  Mac's address to the guest's ADDR:PORT.
                                  LOSS (a percentage, 0 if left out): of the
                                  segments with bytes the relay sends the
                                  guest on them, that many are dropped at
                                  random (a fixed seed), as a lossy Wi-Fi
                                  link drops frames.
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
relay_dgrams_out; relay_scaled (connections whose windows scaled),
relay_sack (connections where both SYNs offered SACK), relay_sack_acks (the
guest's ACKs that carried SACK blocks), relay_lost and relay_lost_bytes
(segments LOSS dropped), relay_resent_bytes (bytes sent to the guest a
second time or more), relay_fast_retransmits (recoveries begun on three
duplicate ACKs), relay_retransmits (timeouts); and the most seen:
relay_window (the guest's window, scaled), relay_out_flight (our bytes
the guest had not acked), relay_in_flight (the guest's bytes past our last
ACK).

The TCP is a test peer's, like tools/tcppeer.py's: in-order receiving (a
segment out of order is dropped and answered with a duplicate ACK), every
segment acked at once, our window the room left in the bytes waiting for
the Mac's socket (so a slow program on the Mac slows the guest's sender),
window scaling (RFC 7323: offered on our SYNs, used when both SYNs carry
it; then the window is up to HOST_READ_MAX); sending up to the guest's
window (at most FLIGHT_MAX in flight) in segments of its MSS. Lost bytes
are sent again as a sender like the Mac's would: on the third duplicate
ACK, fast retransmit (RFC 5681). With SACK (RFC 2018: offered on our SYNs,
used when both SYNs carry it) only the guest's holes below its highest
SACK block are resent, each once a recovery, and again as later ACKs
show more (RFC 6675's idea, without its pipe count), and all of them again
once the guest SACKs bytes sent after them (a resend lost too, as RACK,
RFC 8985, finds it); without SACK the
first segment, and a partial ACK then means the guest dropped the rest:
everything from it on goes again (go-back-N). After RTO seconds without
an ACK: with SACK, the holes below the guest's latest blocks again (as
Linux and macOS do, not RFC 2018's go-back: a lost resend would otherwise
cost the whole flight), and go-back-N if a second timeout comes before
any progress (the guest may have dropped what it said it kept: lwIP does
past its limits); without SACK, go-back-N. A one-byte probe of a zero
window at that pace. A reset from either side resets the other."""
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
LINGER = 1.0            # seconds a connection closed both ways is kept, to ack a FIN again
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

    def __init__(self, relay, mine, theirs, mac, sock, active, connecting=False, loss=0.0):
        self.relay, self.mine, self.theirs, self.mac = relay, mine, theirs, mac
        self.sock, self.connecting = sock, connecting
        self.iss = random.getrandbits(32)
        self.snd_una = self.snd_nxt = self.iss
        self.rcv_nxt, self.peer_wnd, self.peer_mss = 0, 0, 536
        self.state = "SYN_SENT" if active else "SYN_RCVD"
        self.offer_ws = active       # we offer scaling as a client; as a server, if offered
        self.scaled, self.snd_shift = False, 0   # both SYNs had it; the guest's shift
        self.offer_sack = active     # SACK-permitted, the same way
        self.sack = False            # both SYNs had it: the guest's SACK blocks are used
        self.sacked = []             # the guest's SACK blocks above snd_una, [left, right), sorted
        self.dupacks = 0             # duplicate ACKs in a row
        self.recover = None          # in fast recovery until this is acked (snd_nxt then)
        self.resent_to = 0           # this recovery resent holes up to here
        self.resent_mark = None      # self.high at the last pass of resends (lost again if
                                     # the guest SACKs bytes past it with the holes still open)
        self.high = self.iss         # past the highest byte ever sent
        self.loss = loss             # the share of data segments to drop (--tcp-forward's LOSS)
        self.last_blocks = []        # the SACK blocks of the guest's latest ACK
        self.rtos = 0                # timeouts since the last ACK that moved snd_una
        self.acked_out = 0           # the last ACK number we sent
        self.out = bytearray()       # bytes from the Mac, from snd_una on
        self.to_host = bytearray()   # bytes from the guest, not yet written to the Mac
        self.host_eof = False        # the Mac's program shut down its side
        self.guest_fin = False       # the guest sent its FIN (all its bytes are in to_host)
        self.host_shut = False       # we shut down the Mac's socket's write side
        self.fin_sent = False
        self.done = False
        self.linger_until = None     # closed both ways: forgotten at this time
        self.last = 0.0 if active else time.monotonic()

    # the wire
    def window(self):
        return max(0, min(HOST_READ_MAX if self.scaled else WINDOW,
                          HOST_READ_MAX - len(self.to_host)))

    def seg(self, flags, seq, data=b"", syn_opts=False):
        opts = struct.pack("!BBH", 2, 4, MSS) if syn_opts else b""
        if syn_opts and self.offer_ws:   # NOP, then the window scale (RFC 7323 2.2)
            opts += struct.pack("!BBBB", 1, 3, 3, WSHIFT)
        if syn_opts and self.offer_sack:   # two NOPs, then SACK-permitted (RFC 2018 2)
            opts += struct.pack("!BBBB", 1, 1, 4, 2)
        wnd = self.window()
        wnd = min(wnd, WINDOW) if flags & F_SYN or not self.scaled else wnd >> WSHIFT
        if flags & F_ACK:
            self.acked_out = self.rcv_nxt
        hdr = struct.pack("!HHIIBBHHH", self.mine[1], self.theirs[1], seq & M32,
                          self.rcv_nxt & M32 if flags & F_ACK else 0, (20 + len(opts)) // 4 << 4,
                          flags, min(wnd, 0xFFFF), 0, 0) + opts
        self.relay.send_segment(self, hdr, data)

    def scale(self, opts):
        """The guest's SYN (or SYN-ACK) said whether it scales and does SACK:
        both SYNs must."""
        self.scaled = self.offer_ws and 3 in opts
        self.snd_shift = min(opts.get(3, 0), 14)
        if self.scaled:
            self.relay.count("relay_scaled")
        self.sack = self.offer_sack and 4 in opts
        if self.sack:
            self.relay.count("relay_sack")

    def ack(self):
        self.seg(F_ACK, self.snd_nxt)

    def reset(self, why):
        if not self.done:
            self.seg(F_RST | F_ACK, self.snd_nxt)
            self.relay.count("relay_resets")
            self.relay.peer.log("relay %s: reset: %s" % (self.name(), why))
        self.finish()

    def linger(self):
        """Closed both ways: the Mac's socket goes now, the connection
        LINGER seconds later (TIME_WAIT's job): if our ACK of the guest's
        FIN was lost, its FIN comes again and is acked again, where a
        connection already forgotten would leave the guest resending it."""
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
        if self.linger_until is None:
            self.linger_until = time.monotonic() + LINGER

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
            self.send_data(self.snd_nxt, bytes(self.out[off:off + n]))
            self.snd_nxt = (self.snd_nxt + n) & M32
        self.relay.most("relay_out_flight", diff(self.snd_nxt, self.snd_una))
        if self.host_eof and self.snd_nxt == end and not self.fin_sent:
            self.seg(F_ACK | F_FIN, self.snd_nxt)
            self.snd_nxt = (self.snd_nxt + 1) & M32
            self.fin_sent = True

    def send_data(self, seq, data):
        """A segment of our bytes to the guest: counted when sent before,
        and dropped on purpose at the rate LOSS asks."""
        if diff(seq, self.high) < 0:
            self.relay.count("relay_resent_bytes", len(data))
        if diff(seq + len(data), self.high) > 0:
            self.high = (seq + len(data)) & M32
        if self.loss and self.relay.rng.random() < self.loss:
            self.relay.count("relay_lost")
            self.relay.count("relay_lost_bytes", len(data))
            return
        self.seg(F_ACK | F_PSH, seq, data)

    def note_sacks(self, blocks):
        """The guest's SACK blocks, merged into ours: those above snd_una."""
        rel = lambda x: diff(x, self.snd_una)
        spans = [(l, r) for l, r in self.sacked + list(blocks)
                 if 0 < rel(r) <= diff(self.snd_nxt, self.snd_una) and rel(l) < rel(r)]
        spans.sort(key=lambda b: rel(b[0]))
        merged = []
        for l, r in spans:
            if merged and rel(l) <= rel(merged[-1][1]):
                if rel(r) > rel(merged[-1][1]):
                    merged[-1] = (merged[-1][0], r)
            else:
                merged.append((l, r))
        self.sacked = merged

    def resend_holes(self):
        """Fast recovery: what the guest lacks below its highest SACK block
        (without SACK: the first segment), each byte once a recovery. With
        SACK and no block nothing is known lost: the rest may be in flight."""
        if self.sack and not self.sacked:
            return
        top = self.sacked[-1][1] if self.sacked else (self.snd_una + self.peer_mss) & M32
        if diff(top, self.snd_nxt) > 0:
            top = self.snd_nxt
        if self.resent_mark is not None and diff(top, self.resent_mark) > 0:
            seq = self.snd_una   # bytes sent after the last pass arrived, its resends didn't
        else:
            seq = self.resent_to if diff(self.resent_to, self.snd_una) > 0 else self.snd_una
        if diff(top, seq) > 0:
            self.resent_mark = self.high
        for l, r in self.sacked + [(top, top)]:
            while diff(l, seq) > 0 and diff(top, seq) > 0:   # the hole before this block
                off = diff(seq, self.snd_una)
                n = min(self.peer_mss, diff(l, seq), diff(top, seq), len(self.out) - off)
                if n <= 0:
                    break
                self.send_data(seq, bytes(self.out[off:off + n]))
                seq = (seq + n) & M32
            if diff(r, seq) > 0:
                seq = r
        self.resent_to = seq

    def acked(self, ack, dup):
        """An ACK from the guest: bytes acked, or a duplicate (fast recovery
        on the third)."""
        if 0 < diff(ack, self.snd_una) <= diff(self.snd_nxt, self.snd_una):
            n = diff(ack, self.snd_una)
            del self.out[:min(n, len(self.out))]   # the FIN's sequence number has no byte
            self.snd_una, self.last, self.dupacks, self.rtos = ack, time.monotonic(), 0, 0
            self.note_sacks([])
            if self.recover is not None and diff(ack, self.recover) >= 0:
                self.recover = self.resent_mark = None
            elif self.recover is not None and self.sack:
                self.resend_holes()   # a partial ACK: the next holes
            elif self.recover is not None:   # no SACK: the guest dropped the rest
                self.relay.count("relay_go_back")
                self.snd_nxt, self.fin_sent, self.recover = self.snd_una, False, None
            return
        if not dup:
            return
        self.dupacks += 1
        if self.dupacks == 3 and self.recover is None:
            self.relay.count("relay_fast_retransmits")
            self.recover, self.resent_to, self.resent_mark = self.snd_nxt, self.snd_una, None
            self.resend_holes()
        elif self.recover is not None and self.sack:
            self.resend_holes()   # more of the guest's blocks: more holes known

    def tick(self, now):
        if self.linger_until is not None and now >= self.linger_until:
            self.done = True
        if self.done or self.linger_until is not None:
            return
        if now - self.last < (SYN_EVERY if self.state == "SYN_SENT" else RTO):
            return
        self.last = now
        if self.state == "SYN_SENT":
            self.seg(F_SYN, self.iss, syn_opts=True)
            self.snd_nxt = (self.iss + 1) & M32
        elif self.state == "SYN_RCVD":
            self.seg(F_SYN | F_ACK, self.iss, syn_opts=True)
        elif self.snd_nxt != self.snd_una and self.sack and self.last_blocks and not self.rtos:
            # the guest's latest blocks only (it may have dropped what older
            # ones said: lwIP does past its limits), its holes again
            self.relay.count("relay_retransmits")
            self.rtos += 1
            self.sacked, self.dupacks = [], 0
            self.note_sacks(self.last_blocks)
            self.recover, self.resent_to = self.snd_nxt, self.snd_una
            self.resend_holes()
        elif self.snd_nxt != self.snd_una:   # go back to what was acked
            self.relay.count("relay_retransmits")
            self.rtos += 1
            self.snd_nxt, self.fin_sent = self.snd_una, False
            self.sacked, self.recover, self.dupacks = [], None, 0
            self.pump()
        elif self.peer_wnd == 0 and self.out:   # a zero window: one byte, to hear it open
            self.send_data(self.snd_nxt, bytes(self.out[:1]))
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
        if flags & F_ACK:
            self.last_blocks = opts.get(5, [])
            if opts.get(5):
                self.relay.count("relay_sack_acks")
                if self.sack:
                    self.note_sacks(opts[5])
            # a duplicate (RFC 5681): no bytes, no SYN or FIN, bytes out, and the
            # same window (or SACK blocks: the guest's reads may move the window)
            dup = (ack == self.snd_una and not data and not flags & (F_SYN | F_FIN) and
                   (wnd == self.peer_wnd or bool(opts.get(5))) and self.snd_nxt != self.snd_una)
            self.acked(ack, dup)
            self.peer_wnd = wnd
            self.relay.most("relay_window", wnd)
        self.take(flags, seq, data)
        self.pump()

    def established(self, ack, wnd, opts):
        self.snd_una = self.snd_nxt = self.high = self.resent_to = ack
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
            self.linger()

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
        self.rng = random.Random(0x10557)   # LOSS's: the same drops every run
        for k in ("relay_conns", "relay_bytes_to_guest", "relay_bytes_from_guest",
                  "relay_resets", "relay_refused", "relay_retransmits", "relay_dgrams_in",
                  "relay_dgrams_out", "relay_sack", "relay_sack_acks", "relay_lost",
                  "relay_lost_bytes", "relay_resent_bytes", "relay_fast_retransmits",
                  "relay_go_back"):
            peer.counts[k] = 0
        for port, hostport in (udp or {}).items():
            peer.add_udp(port, self.udp_handler(port, hostport))
        for lport, gaddr, gport, *loss in forwards:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("127.0.0.1", lport))
            s.listen(16)
            s.setblocking(False)
            self.listeners.append((s, gaddr, gport, loss[0] if loss else 0.0))

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
            if body[k] == 4 and body[k + 1] == 2:
                opts[4] = True
            if body[k] == 5 and body[k + 1] % 8 == 2 and k + body[k + 1] <= hl:
                opts[5] = [struct.unpack_from("!II", body, k + 2 + 8 * i)
                           for i in range(body[k + 1] // 8)]
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
        c.offer_ws, c.offer_sack = 3 in opts, 4 in opts   # answered in kind
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
        r = [s for s, _, _, _ in self.listeners] + list(self.udp_socks)
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
        for s, gaddr, gport, loss in self.listeners:
            if s in readable:
                self.forward(s, gaddr, gport, loss)
        for c in list(self.streams.values()):
            if c.sock is not None:
                c.host_io(c.sock in readable, c.sock in writable)

    def forward(self, lsock, gaddr, gport, loss):
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
        c = Stream(self, mine, (gaddr, gport), self.peer.guest_mac, s, True, loss=loss)
        self.streams[(gaddr, gport, p)] = c

    def tick(self):
        now = time.monotonic()
        for k, c in list(self.streams.items()):
            c.tick(now)
            if c.done:
                del self.streams[k]

    def close(self):
        for s, _, _, _ in self.listeners:
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
    """"18080:10.2.21.5:8080,18081:10.2.21.5:8080:2" -> [(18080, address
    bytes, 8080, 0.0), (18081, ..., 0.02)]: the last field LOSS in percent."""
    out = []
    for part in filter(None, (text or "").split(",")):
        lport, addr, gport, *loss = part.split(":")
        out.append((int(lport), np.ip_bytes(addr), int(gport),
                    float(loss[0]) / 100 if loss else 0.0))
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


def _resent(wire):
    """The sequence numbers of the data segments on wire (and forget them)."""
    seqs = [struct.unpack_from("!I", f, 34 + 4)[0] for f in wire.out if len(f) > 34 + 20]
    wire.out = []
    return seqs


def _sack_selftest(np):
    """Fast recovery with SACK, by hand: 20 segments out, the guest lacks
    the 1st and the 6th and says so in three duplicate ACKs: exactly those
    two go again; partial ACKs then resend nothing more. Without SACK the
    third duplicate resends the first, and a partial ACK all the rest."""
    a = _Wire()
    relay = Relay(a, np, {}, [])
    for sack in (True, False):
        c = Stream(relay, (b"\x0a\x02\x15\xae", 41000), (b"\x0a\x02\x15\x05", 5202), a.guest_mac,
                   None, True)
        c.state = "ESTABLISHED"
        c.established((c.iss + 1) & M32, 1 << 20, {2: MSS})
        c.sack, c.out, una = sack, bytearray(20 * MSS), (c.iss + 1) & M32
        c.pump()
        a.out = []
        seg = lambda i: (una + i * MSS) & M32
        blocks = [(seg(1), seg(5)), (seg(6), seg(10))] if sack else []
        for _ in range(3):
            c.input(F_ACK, c.rcv_nxt, una, 1 << 20, {5: blocks} if sack else {}, b"")
        got = _resent(a)
        if got != ([seg(0), seg(5)] if sack else [seg(0)]):
            return "relay selftest: sack %s: resent %s" % (sack, [(s - una) // MSS for s in got])
        c.input(F_ACK, c.rcv_nxt, seg(5), 1 << 20, {5: blocks[1:]} if sack else {}, b"")
        got = _resent(a)
        if got != ([] if sack else [seg(i) for i in range(5, 20)]):
            return "relay selftest: sack %s: after a partial ACK, resent %s" % (
                sack, [(s - una) // MSS for s in got])
    return None


def selftest(np, tcp, nbytes=300000, drop=0.02, limit=30.0):
    """The relay both ways against tools/tcppeer.py's TCP as the guest, over
    a lossy wire, with real sockets on 127.0.0.1 as the Mac's programs:
    every byte right both ways; and fast recovery by hand (_sack_selftest).
    Returns a problem or None."""
    problem = _sack_selftest(np)
    if problem:
        return problem
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
