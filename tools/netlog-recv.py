#!/usr/bin/env python3
"""Receive the PC's log over UDP (docs/M9-PLAN.md, "netlog: the log over
UDP to the Mac"): bin/netlog sends every boot's log from its first line.

    netlog-recv.py [--port 5021] [--bind ADDR] [--from ADDR] [--quiet] FOLDER
        write one file per boot into FOLDER, print the lines as they come
        (not with --quiet), say where a gap is; until ^C
    netlog-recv.py --self-test
        the receiver against a Python sender on 127.0.0.1 (netlog.c's
        go-back-N): started late, paused, datagrams lost both ways, the
        PC's ring dropping bytes, a crash log, the receiver restarted with
        and without its files, hostile datagrams; exit 0 on PASS

The datagrams (user/include/netlog.h has the byte layout; little-endian):
data from the PC carries a boot id (the kernel's start, UTC ns), a stream
(0 this boot's log, 1 after a panic the panicked boot's), an offset, the
offset the PC was last acked, a sequence number and up to 1400 bytes of
text; the receiver answers each with an ack: the offset below which it
has every byte of that stream.

Files: boot-<the boot's start, local time>.txt for the live stream and
the same name with -lastcrash for the log of the boot that panicked
before it (boot-unknown-<when first seen>.txt if the PC's clock wasn't
set). Beside each, <file>.pos keeps how many bytes of the stream the file
holds, so a restarted receiver goes on where it stopped. Text is written
only in order; a datagram past a gap waits for the PC to send again (it
does, from the acked offset). Notes in the file, in square brackets:
bytes the PC's ring dropped before they were sent (NETLOG_F_LOST), and
bytes an earlier receiver acked that this one doesn't have (it started
without the files)."""
import argparse
import os
import random
import socket
import struct
import sys
import tempfile
import threading
import time

PORT = 5021
MAGIC = 0x474C4E4A          # "JNLG"
VERSION = 1
DATA, ACK = 1, 2
LIVE, CRASH = 0, 1
F_LOST, F_END, F_ALL = 1, 2, 3
TEXT_MAX = 1400
HDR = struct.Struct("<IBBBBQQQIHH")     # 40 bytes, then the text
ACKS = struct.Struct("<IBBBBQQ")        # 24 bytes


def decode_data(d):
    """(stream, flags, boot, offset, acked, seq, text) or None: netlog.c's
    netlog_data_decode, rule for rule."""
    if not HDR.size <= len(d) <= HDR.size + TEXT_MAX:
        return None
    magic, ver, typ, stream, flags, boot, off, acked, seq, n, res = HDR.unpack_from(d)
    if magic != MAGIC or ver != VERSION or typ != DATA or res or stream > CRASH or \
            flags & ~F_ALL or len(d) != HDR.size + n or n > TEXT_MAX or \
            (not n and not flags & F_END) or acked > off or off + n >= 1 << 64:
        return None
    return stream, flags, boot, off, acked, seq, d[HDR.size:]


def encode_ack(stream, boot, acked):
    return ACKS.pack(MAGIC, VERSION, ACK, stream, 0, boot, acked)


class Stream:
    """One boot's stream: its file and how much of it is there."""

    def __init__(self, folder, name):
        self.path = os.path.join(folder, name)
        self.pos = self.path + ".pos"
        self.have, self.seq, self.missed, self.gap_said, self.ended = 0, None, 0, None, False
        if os.path.exists(self.pos):
            with open(self.pos) as f:
                self.have = int(f.read().strip() or 0)
        self.f = open(self.path, "ab")

    def write(self, data, have):
        self.f.write(data)
        self.f.flush()
        self.have = have
        with open(self.pos + ".new", "w") as f:
            f.write("%d\n" % have)
        os.replace(self.pos + ".new", self.pos)

    def note(self, text):
        self.write(("[netlog-recv: %s]\n" % text).encode(), self.have)


class Receiver:
    def __init__(self, folder, sock, peer=None, out=None, say=None):
        self.folder, self.sock, self.peer = folder, sock, peer
        self.out = out                       # where text is printed (None: nowhere)
        self.say = say or (lambda s: print(s, file=sys.stderr, flush=True))
        self.streams = {}
        self.unknown = {}                    # boot id 0: a name per first sight
        self.ignored = 0
        os.makedirs(folder, exist_ok=True)

    def name(self, boot, stream):
        if boot:
            t = time.localtime(boot // 1_000_000_000)
            base = "boot-%s.%03d" % (time.strftime("%Y-%m-%d_%H-%M-%S", t),
                                     boot // 1_000_000 % 1000)
        else:
            base = self.unknown.setdefault(stream, "boot-unknown-" +
                                           time.strftime("%Y-%m-%d_%H-%M-%S"))
        return base + ("-lastcrash" if stream == CRASH else "") + ".txt"

    def stream(self, boot, stream):
        key = (boot, stream)
        if key not in self.streams:
            s = Stream(self.folder, self.name(boot, stream))
            self.streams[key] = s
            self.say("netlog-recv: %s%s" % (s.path, " (going on at byte %d)" % s.have
                                             if s.have else ""))
        return self.streams[key]

    def take(self, s, stream, flags, off, acked, seq, text):
        if s.seq is not None and seq > s.seq + 1:
            s.missed += seq - s.seq - 1
        s.seq = seq if s.seq is None else max(s.seq, seq)
        if acked > s.have:
            s.note("bytes %d to %d: received by an earlier receiver, not here" % (s.have, acked))
            s.have = acked
        if flags & F_LOST and off > s.have:
            s.note("%d bytes of the log were lost on the PC (its ring dropped them)"
                   % (off - s.have))
            s.have = off
        end = off + len(text)
        if off <= s.have < end:
            new = text[s.have - off:]
            s.write(new, end)
            s.gap_said = None
            if self.out:
                self.out.write(new.decode("utf-8", "replace") if stream == LIVE else
                               "".join("[crash] " + l + "\n" for l in
                                       new.decode("utf-8", "replace").splitlines()))
                self.out.flush()
        elif off > s.have and s.gap_said != s.have:
            s.gap_said = s.have
            self.say("netlog-recv: %s: gap at byte %d (a datagram at %d came first): waiting "
                     "for the PC to send it again" % (os.path.basename(s.path), s.have, off))
        if flags & F_END and s.have == end and not s.ended:
            s.ended = True
            self.say("netlog-recv: %s: complete, %d bytes" % (os.path.basename(s.path), end))

    def handle(self, dgram):
        """One datagram: the ack to send back, or None (not one of ours)."""
        d = decode_data(dgram)
        if d is None:
            self.ignored += 1
            return None
        stream, flags, boot, off, acked, seq, text = d
        s = self.stream(boot, stream)
        self.take(s, stream, flags, off, acked, seq, text)
        return encode_ack(stream, boot, s.have)

    def serve_one(self):
        dgram, peer = self.sock.recvfrom(2048)
        if self.peer and peer[0] != self.peer:
            return
        ack = self.handle(dgram)
        if ack:
            self.sock.sendto(ack, peer)

    def close(self):
        for s in self.streams.values():
            s.f.close()


# ---- the self-test's sender: netlog.c's go-back-N, in Python -----------------------

class Sender:
    """streams: {stream: (text, first, ended)}: the text, where the PC's ring
    starts (bytes below it are gone), whether the stream has an end."""

    def __init__(self, addr, boot, streams, drop=0.0, seed=1, resend=0.05):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setblocking(False)
        self.addr, self.boot, self.drop, self.resend = addr, boot, drop, resend
        self.rng = random.Random(seed)
        self.s = {k: dict(text=t, first=f, end=e, acked=0, next=0, high=0, seq=0, lost=False)
                  for k, (t, f, e) in streams.items()}
        self.retry_at, self.wait = None, resend

    def send_one(self, k, s, window=64 << 10):
        if s["next"] - s["acked"] >= window or s["next"] >= len(s["text"]):
            return False
        first = max(s["next"], s["first"])
        if first > s["next"]:
            s["lost"] = True
        n = min(TEXT_MAX, window - (s["next"] - s["acked"]), len(s["text"]) - first)
        if n <= 0:
            return False
        chunk = s["text"][first:first + n]
        if n == TEXT_MAX and first + n < len(s["text"]) and b"\n" in chunk:
            chunk = chunk[:chunk.rindex(b"\n") + 1]
        flags = (F_LOST if s["lost"] else 0) | \
                (F_END if s["end"] and first + len(chunk) == len(s["text"]) else 0)
        d = HDR.pack(MAGIC, VERSION, DATA, k, flags, self.boot, first, s["acked"], s["seq"],
                     len(chunk), 0) + chunk
        s["seq"] += 1
        s["lost"] = False
        s["next"] = first + len(chunk)
        s["high"] = max(s["high"], s["next"])
        if self.rng.random() >= self.drop:
            try:
                self.sock.sendto(d, self.addr)
            except OSError:
                pass        # nobody listening yet: a lost datagram
        return True

    def poll(self, now):
        if self.retry_at and now >= self.retry_at:
            for s in self.s.values():
                s["next"] = s["acked"]
            self.wait = min(self.wait * 2, 0.4)
            self.retry_at = None
        for k, s in self.s.items():
            for _ in range(16):
                if not self.send_one(k, s):
                    break
        if not self.retry_at and any(s["high"] > s["acked"] for s in self.s.values()):
            self.retry_at = now + self.wait
        while True:
            try:
                a = self.sock.recv(64)
            except (BlockingIOError, OSError):
                return
            if len(a) != ACKS.size or self.rng.random() < self.drop:
                continue
            magic, ver, typ, k, res, boot, acked = ACKS.unpack(a)
            s = self.s.get(k)
            if magic != MAGIC or typ != ACK or boot != self.boot or not s or acked > s["high"]:
                continue
            self.wait = self.resend
            if acked > s["acked"]:
                s["acked"] = acked
                s["next"] = max(s["next"], acked)
                self.retry_at = now + self.wait

    def done(self):
        return all(s["acked"] == len(s["text"]) for s in self.s.values())


def make_log(n_lines, seed):
    rng = random.Random(seed)
    lines = []
    for i in range(n_lines):
        w = "x" * rng.randint(0, 3000) if i % 97 == 5 else "word " * rng.randint(1, 30)
        lines.append(("[%8.3f] line %d: %s" % (i / 100, i, w)).encode())
    return b"\n".join(lines) + b"\n"


def run_receiver(folder, sock, stop, pause=None):
    """Serve until stop is set; pause: (event, seconds): stop reading for a while."""
    r = Receiver(folder, sock, say=lambda s: None)
    sock.settimeout(0.02)
    while not stop.is_set():
        if pause and pause[0].is_set():
            time.sleep(pause[1])
            pause[0].clear()
        try:
            r.serve_one()
        except socket.timeout:
            pass
    r.close()
    return r


def self_test():
    fails = []
    tmp = tempfile.mkdtemp(prefix="netlog-recv-test.")

    def check(what, cond):
        print("netlog-recv self-test: %s: %s" % (what, "ok" if cond else "FAILED"))
        if not cond:
            fails.append(what)

    def free_port():
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        p = s.getsockname()[1]
        s.close()
        return p

    def start(folder, port, pause=None):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", port))
        stop = threading.Event()
        box = {}
        t = threading.Thread(target=lambda: box.update(r=run_receiver(folder, sock, stop, pause)),
                             daemon=True)
        t.start()
        return stop, t, sock, box

    def drive(sender, until, also=None):
        t0 = time.monotonic()
        while time.monotonic() - t0 < until and not sender.done():
            if also:
                also(time.monotonic() - t0)
            sender.poll(time.monotonic())
            time.sleep(0.002)
        return sender.done()

    boot = 1_790_000_000_123_456_789
    live, crash = make_log(3000, 1), make_log(400, 2)
    name = Receiver(tmp, None, say=lambda s: None).name(boot, LIVE)
    # 1. Started late (0.5 s after the sender), paused for 0.5 s half way, 10%
    # lost both ways, the ring had dropped the first 5000 bytes, plus the crash log.
    folder = os.path.join(tmp, "a")
    port = free_port()
    s = Sender(("127.0.0.1", port), boot, {LIVE: (live, 5000, False), CRASH: (crash, 0, True)},
               drop=0.1)
    pause = threading.Event()
    box = {}

    def events(t):
        if t > 0.5 and "rx" not in box:
            box["rx"] = start(folder, port, (pause, 0.5))
        if t > 1.5 and "paused" not in box:
            box["paused"] = pause.set() or True
    ok = drive(s, 60, events)
    check("the whole log arrived (late start, a pause, 10% lost)", ok)
    box["rx"][0].set()
    box["rx"][1].join()
    got = open(os.path.join(folder, name), "rb").read()
    note = b"[netlog-recv: 5000 bytes of the log were lost on the PC (its ring dropped them)]\n"
    check("the file is the log, with the ring's loss noted", got == note + live[5000:])
    got = open(os.path.join(folder, name.replace(".txt", "-lastcrash.txt")), "rb").read()
    check("the crash log arrived whole", got == crash)
    check("no gap notes for lost datagrams (they were sent again)", got.count(b"[netlog-recv") == 0)
    # 2. The receiver stopped half way and started again on the same folder:
    # it goes on where it stopped (its .pos), nothing doubled.
    folder = os.path.join(tmp, "b")
    port = free_port()
    s = Sender(("127.0.0.1", port), boot, {LIVE: (live, 0, False)})
    rx = start(folder, port)
    t0 = time.monotonic()
    while s.s[LIVE]["acked"] < len(live) // 2 and time.monotonic() - t0 < 60:
        s.poll(time.monotonic())
        time.sleep(0.002)
    rx[0].set()
    rx[1].join()
    rx[2].close()
    acked = s.s[LIVE]["acked"]     # the PC goes on from what it was acked
    s = Sender(("127.0.0.1", port), boot, {LIVE: (live, 0, False)})
    s.s[LIVE]["acked"] = s.s[LIVE]["next"] = s.s[LIVE]["high"] = acked
    rx = start(folder, port)
    check("a restarted receiver gets the rest", drive(s, 60))
    rx[0].set()
    rx[1].join()
    rx[2].close()
    check("... with nothing doubled or missing", open(os.path.join(folder, name), "rb").read() == live)
    # 3. A receiver without the files (another folder): the PC was acked up to
    # some offset by an earlier one: the file starts there, with a note.
    folder = os.path.join(tmp, "c")
    port = free_port()
    s = Sender(("127.0.0.1", port), boot, {LIVE: (live, 0, False)})
    s.s[LIVE]["acked"] = s.s[LIVE]["next"] = s.s[LIVE]["high"] = 10000
    rx = start(folder, port)
    check("a fresh receiver takes the rest", drive(s, 60))
    rx[0].set()
    rx[1].join()
    rx[2].close()
    got = open(os.path.join(folder, name), "rb").read()
    check("... noting the bytes an earlier receiver has",
          got == b"[netlog-recv: bytes 0 to 10000: received by an earlier receiver, not here]\n" +
          live[10000:])
    # 4. Hostile datagrams: no ack, nothing written.
    r = Receiver(os.path.join(tmp, "d"), None, say=lambda s: None)
    good = HDR.pack(MAGIC, VERSION, DATA, LIVE, 0, boot, 0, 0, 0, 3, 0) + b"ab\n"
    bad = [b"", b"\0" * 40, good[:-1], good + b"x",
           HDR.pack(MAGIC ^ 1, VERSION, DATA, LIVE, 0, boot, 0, 0, 0, 3, 0) + b"ab\n",
           HDR.pack(MAGIC, 2, DATA, LIVE, 0, boot, 0, 0, 0, 3, 0) + b"ab\n",
           HDR.pack(MAGIC, VERSION, ACK, LIVE, 0, boot, 0, 0, 0, 3, 0) + b"ab\n",
           HDR.pack(MAGIC, VERSION, DATA, 2, 0, boot, 0, 0, 0, 3, 0) + b"ab\n",
           HDR.pack(MAGIC, VERSION, DATA, LIVE, 4, boot, 0, 0, 0, 3, 0) + b"ab\n",
           HDR.pack(MAGIC, VERSION, DATA, LIVE, 0, boot, 0, 0, 0, 3, 1) + b"ab\n",
           HDR.pack(MAGIC, VERSION, DATA, LIVE, 0, boot, 0, 0, 0, 0, 0),
           HDR.pack(MAGIC, VERSION, DATA, LIVE, 0, boot, 5, 6, 0, 3, 0) + b"ab\n",
           HDR.pack(MAGIC, VERSION, DATA, LIVE, 0, boot, 0, 0, 0, 1401, 0) + b"a" * 1401]
    check("hostile datagrams get no ack", all(r.handle(d) is None for d in bad))
    check("... and nothing is written", not os.listdir(os.path.join(tmp, "d")))
    a = r.handle(good)
    check("a good one is acked", a == encode_ack(LIVE, boot, 3))
    # A gap: the next datagram past what is there: acked at 3 still.
    later = HDR.pack(MAGIC, VERSION, DATA, LIVE, 0, boot, 10, 0, 2, 3, 0) + b"cd\n"
    check("past a gap: acked where the file ends", r.handle(later) == encode_ack(LIVE, boot, 3))
    r.close()
    if fails:
        print("netlog-recv self-test: FAIL (%d)" % len(fails))
        return 1
    print("netlog-recv self-test: PASS")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("folder", nargs="?")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--from", dest="peer", help="take datagrams only from this address (the PC's)")
    ap.add_argument("--quiet", action="store_true", help="don't print the lines")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not a.folder:
        ap.error("a folder for the logs")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((a.bind, a.port))
    r = Receiver(a.folder, sock, peer=a.peer, out=None if a.quiet else sys.stdout)
    print("netlog-recv: listening on %s:%d, logs into %s" % (a.bind, a.port, a.folder),
          file=sys.stderr)
    try:
        while True:
            r.serve_one()
    except KeyboardInterrupt:
        r.close()
        return 0


if __name__ == "__main__":
    sys.exit(main())
