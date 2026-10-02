#!/usr/bin/env python3
"""Serve the Mac's build to the PC's `update` (docs/M9-PLAN.md, "update: a
new build from the Mac"): build/jamos.elf, build/bootfs.img and their
manifest, over UDP.

    update-server.py [--build DIR] [--port 5022] [--bind ADDR] [--client ADDR]
        serve DIR/jamos.elf and DIR/bootfs.img (default build/) until ^C
    update-server.py --manifest KERNEL BOOTFS [--version V] [--git G]
        print the manifest of those two files and exit
    update-server.py --self-test
        the server against a Python client on 127.0.0.1: a whole fetch
        with requests and replies lost, a snapshot kept while the files
        change, a dropped snapshot, malformed requests; exit 0 on PASS

The manifest (user/include/update.h has the C side's rules, and its strict
parser is user/lib/update.c):

    jamos-update 1
    version <the kernel's version string>
    git <short hash, -dirty if the tree has changes>
    kernel <size> <sha256>
    bootfs <size> <sha256>
    signature

The signature line has no value: updates are unsigned in M9 (the owner's
decision); it keeps the place for signing later.

The protocol (user/include/updwire.h has the byte layout; every field is
little-endian): a request names a snapshot, a file (0 the manifest, 1 the
kernel, 2 the boot image), an offset and a length of at most 1400 bytes;
the reply carries the same, the file's size and the bytes. A request for
the manifest with snapshot 0 makes a snapshot: both files read into memory
then (read again if either changed while being read, so a `make` running
meanwhile can't mix two builds), and every later request for that snapshot
is answered from that copy. The server keeps the last SNAPSHOTS snapshots
and nothing per client: a request for another snapshot gets GONE and the
fetcher starts again from the manifest. Malformed datagrams get no answer.

The version comes from the kernel's own `jamos_version` symbol in the ELF
file (so it is the build's, not the source's), else from kernel/main.c;
the git hash is the tree's when the snapshot is made."""
import argparse
import collections
import hashlib
import os
import random
import re
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

PORT = 5022
MAGIC = 0x4450554A          # "JUPD"
VERSION = 1
REQUEST, REPLY = 1, 2
MANIFEST, KERNEL, BOOTFS = 0, 1, 2
OK, GONE, RANGE, BAD = 0, 1, 2, 3
CHUNK_MAX = 1400
REQ = struct.Struct("<IBBBBIIHH")           # 20 bytes
REP = struct.Struct("<IBBBBIIIHH")          # 24 bytes, then the data
FILE_MAX = 32 << 20                         # <update.h> UPDATE_FILE_MAX
MANIFEST_MAX = 1024
SNAPSHOTS = 8
SETTLE = 1.0                                # seconds both files must be unchanged
VERSION_RE = re.compile(rb"[A-Za-z0-9._+-]{1,47}$")
GIT_RE = re.compile(rb"[0-9a-f]{7,40}(-dirty)?$")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ---- the manifest ---------------------------------------------------------------

def manifest(kernel, bootfs, version, git):
    """The manifest's bytes for these two files' contents."""
    if not VERSION_RE.match(version.encode()) or not GIT_RE.match(git.encode()):
        raise ValueError("bad version %r or git %r" % (version, git))
    lines = ["jamos-update 1", "version " + version, "git " + git]
    for name, data in (("kernel", kernel), ("bootfs", bootfs)):
        if not 1 <= len(data) <= FILE_MAX:
            raise ValueError("%s is %d bytes (1..%d)" % (name, len(data), FILE_MAX))
        lines.append("%s %d %s" % (name, len(data), hashlib.sha256(data).hexdigest()))
    lines.append("signature")
    text = ("\n".join(lines) + "\n").encode()
    assert len(text) <= MANIFEST_MAX
    return text


def parse_manifest(text):
    """The C parser's rules, for the self-test: (version, git, [(size, sha)]) or None."""
    lines = text.split(b"\n")
    if len(text) > MANIFEST_MAX or len(lines) != 7 or lines[6] != b"" or lines[0] != b"jamos-update 1":
        return None
    v, g = lines[1].split(b" ", 1), lines[2].split(b" ", 1)
    if v[0] != b"version" or len(v) != 2 or not VERSION_RE.match(v[1]):
        return None
    if g[0] != b"git" or len(g) != 2 or not GIT_RE.match(g[1]):
        return None
    files = []
    for name, line in zip((b"kernel", b"bootfs"), lines[3:5]):
        w = line.split(b" ")
        if len(w) != 3 or w[0] != name or not re.match(rb"[1-9][0-9]{0,9}$", w[1]) or \
                not re.match(rb"[0-9a-f]{64}$", w[2]) or int(w[1]) > FILE_MAX:
            return None
        files.append((int(w[1]), w[2].decode()))
    return (v[1].decode(), g[1].decode(), files) if lines[5] == b"signature" else None


def elf_symbol_string(data, name):
    """The NUL-terminated string a symbol of a 64-bit little-endian ELF
    file points at (from .symtab, by its section's file offset), or None."""
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return None
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    secs = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    for sec in secs:
        if sec[1] != 2:     # SHT_SYMTAB
            continue
        strtab = secs[sec[6]]
        for at in range(sec[4], sec[4] + sec[5], 24):
            st_name, _, _, shndx, value, _ = struct.unpack_from("<IBBHQQ", data, at)
            end = data.index(b"\0", strtab[4] + st_name)
            if data[strtab[4] + st_name:end] != name or not 0 < shndx < shnum:
                continue
            target = secs[shndx]
            off = target[4] + value - target[3]
            return data[off:data.index(b"\0", off)].decode(errors="replace")
    return None


def build_version(kernel):
    v = elf_symbol_string(kernel, b"jamos_version")
    if v:
        return v
    with open(os.path.join(REPO, "kernel/main.c")) as f:
        m = re.search(r'#define JAMOS_VERSION\s+"([^"]+)"', f.read())
    return m.group(1) if m else "unknown"


def git_hash():
    try:
        h = subprocess.run(["git", "-C", REPO, "rev-parse", "--short", "HEAD"], check=True,
                           capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", REPO, "status", "--porcelain",
                                "--untracked-files=no"], check=True,
                               capture_output=True, text=True).stdout.strip()
        return h + ("-dirty" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "0000000"


# ---- the server -------------------------------------------------------------------

def read_stable(paths, settle, tries=10):
    """The files' bytes, once neither has changed for `settle` seconds, and
    read again if either changed during the read. (A snapshot taken while
    `make` is between writing the kernel and the boot image would still
    pair a new kernel with an old image: run `update` once `make` is done.)"""
    for _ in range(tries):
        before = [os.stat(p) for p in paths]
        young = settle - (time.time() - max(s.st_mtime for s in before))
        if young > 0:
            time.sleep(min(young, 2.0))
            continue
        datas = [open(p, "rb").read() for p in paths]
        after = [os.stat(p) for p in paths]
        if all((a.st_size, a.st_mtime_ns) == (b.st_size, b.st_mtime_ns) and a.st_size == len(d)
               for a, b, d in zip(before, after, datas)):
            return datas
    raise OSError("the build kept changing while it was read: is `make` still running?")


class Server:
    def __init__(self, kernel_path, bootfs_path, sock, client=None, log=print,
                 version=None, git=None, settle=SETTLE):
        self.paths = [kernel_path, bootfs_path]
        self.sock, self.client, self.log = sock, client, log
        self.version, self.git, self.settle = version, git, settle
        self.snaps = collections.OrderedDict()   # id -> [manifest, kernel, bootfs]

    def snapshot(self):
        kernel, bootfs = read_stable(self.paths, self.settle)
        text = manifest(kernel, bootfs, self.version or build_version(kernel),
                        self.git or git_hash())
        sid = 0
        while not sid or sid in self.snaps:
            sid = secrets.randbits(32)
        self.snaps[sid] = [text, kernel, bootfs]
        while len(self.snaps) > SNAPSHOTS:
            self.snaps.popitem(last=False)
        self.log("update-server: snapshot %08x: %s" % (sid, text.decode().split("\n")[1:3]))
        return sid

    def answer(self, dgram):
        """The reply to one datagram, or None (not a request of ours)."""
        if len(dgram) != REQ.size:
            return None
        magic, ver, typ, f, res, sid, off, length, res2 = REQ.unpack(dgram)
        if magic != MAGIC or ver != VERSION or typ != REQUEST or res or res2 or \
                f > BOOTFS or not 1 <= length <= CHUNK_MAX or (not sid and f != MANIFEST):
            return None
        if not sid:
            try:
                sid = self.snapshot()
            except (OSError, ValueError) as e:
                self.log("update-server: can't snapshot the build: %s" % e)
                return None
        snap = self.snaps.get(sid)
        if snap is None:
            return REP.pack(MAGIC, VERSION, REPLY, f, GONE, sid, off, 0, 0, 0)
        data = snap[f]
        if off > len(data) or (off == len(data) and len(data)):
            return REP.pack(MAGIC, VERSION, REPLY, f, RANGE, sid, off, len(data), 0, 0)
        piece = data[off:off + length]
        return REP.pack(MAGIC, VERSION, REPLY, f, OK, sid, off, len(data), len(piece), 0) + piece

    def serve_one(self):
        dgram, peer = self.sock.recvfrom(2048)
        if self.client and peer[0] != self.client:
            return
        rep = self.answer(dgram)
        if rep is not None:
            self.sock.sendto(rep, peer)


# ---- the self-test's client: updfetch.c's window, in Python --------------------------

class Client:
    def __init__(self, addr, drop=0.0, seed=1, window=32, retry=0.05):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.01)
        self.addr, self.window, self.retry = addr, window, retry
        self.rng = random.Random(seed)
        self.drop = drop
        self.sent = self.resent = 0

    def request(self, f, sid, off, length):
        self.sent += 1
        if self.rng.random() >= self.drop:      # the request lost on the way
            self.sock.sendto(REQ.pack(MAGIC, VERSION, REQUEST, f, 0, sid, off, length, 0),
                             self.addr)

    def replies(self):
        while True:
            try:
                d = self.sock.recv(2048)
            except socket.timeout:
                return
            if self.rng.random() < self.drop:   # the reply lost on the way
                continue
            if len(d) < REP.size:
                continue
            fields = REP.unpack_from(d)
            yield fields, d[REP.size:]

    def one(self, wait=1.0):
        """The next reply's fields and data within wait seconds, or None."""
        self.sock.settimeout(wait)
        try:
            d = self.sock.recv(2048)
        except socket.timeout:
            return None
        finally:
            self.sock.settimeout(0.01)
        return (REP.unpack_from(d), d[REP.size:]) if len(d) >= REP.size else None

    def fetch_manifest(self, tries=50):
        for _ in range(tries):
            self.request(MANIFEST, 0, 0, CHUNK_MAX)
            for (magic, _, typ, f, st, sid, off, size, n, _), data in self.replies():
                if typ == REPLY and f == MANIFEST and st == OK and n == size:
                    return sid, data
        raise TimeoutError("no manifest")

    def fetch(self, sid, sizes, between=None):
        """Both files of snapshot sid; between() runs once, half way."""
        out = [bytearray(s) for s in sizes]
        todo = collections.deque((f, off, min(CHUNK_MAX, sizes[f - 1] - off))
                                 for f in (KERNEL, BOOTFS) for off in range(0, sizes[f - 1], CHUNK_MAX))
        total, flight, done = len(todo), {}, 0
        while todo or flight:
            now = time.monotonic()
            for key, (t, tries) in list(flight.items()):
                if now - t >= self.retry:
                    if tries >= 40:
                        raise TimeoutError("request %r unanswered" % (key,))
                    self.resent += 1
                    self.request(key[0], sid, key[1], key[2])
                    flight[key] = (now, tries + 1)
            while todo and len(flight) < self.window:
                key = todo.popleft()
                self.request(key[0], sid, key[1], key[2])
                flight[key] = (now, 1)
            for (magic, _, typ, f, st, rsid, off, size, n, _), data in self.replies():
                if st == GONE:
                    raise LookupError("snapshot gone")
                key = (f, off, n)
                if typ != REPLY or st != OK or rsid != sid or key not in flight:
                    continue
                del flight[key]
                out[f - 1][off:off + n] = data
                done += 1
                if between and done == total // 2:
                    between()
                    between = None
        return [bytes(o) for o in out]


def self_test():
    fails = []
    tmp = tempfile.mkdtemp(prefix="update-server-test.")
    kpath, bpath = os.path.join(tmp, "jamos.elf"), os.path.join(tmp, "bootfs.img")
    rng = random.Random(7)
    kernel = bytes(rng.getrandbits(8) for _ in range(300_001))
    bootfs = bytes(rng.getrandbits(8) for _ in range(512 * 1024))
    open(kpath, "wb").write(kernel)
    open(bpath, "wb").write(bootfs)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    server = Server(kpath, bpath, sock, log=lambda s: None, version="0.0.29-test", git="abcdef0",
                    settle=0.0)
    stop = threading.Event()

    def loop():
        sock.settimeout(0.05)
        while not stop.is_set():
            try:
                server.serve_one()
            except socket.timeout:
                pass
    threading.Thread(target=loop, daemon=True).start()
    addr = sock.getsockname()

    def check(what, cond):
        print("update-server self-test: %s: %s" % (what, "ok" if cond else "FAILED"))
        if not cond:
            fails.append(what)

    # 1. A whole fetch, 10% of requests and of replies lost; the files are
    # changed half way (a `make` meanwhile): the fetch still gets the snapshot's.
    c = Client(addr, drop=0.1)
    sid, text = c.fetch_manifest()
    m = parse_manifest(text)
    check("the manifest parses", m is not None and m[0] == "0.0.29-test" and m[1] == "abcdef0")
    sizes = [m[2][0][0], m[2][1][0]] if m else [0, 0]

    def rebuild():
        open(kpath, "wb").write(b"x" * 1000)
        open(bpath, "wb").write(b"y" * 2000)
    k, b = c.fetch(sid, sizes, between=rebuild)
    check("the fetch matches the manifest's SHA-256s",
          m and hashlib.sha256(k).hexdigest() == m[2][0][1] and
          hashlib.sha256(b).hexdigest() == m[2][1][1])
    check("the snapshot is the files as they were", k == kernel and b == bootfs)
    check("some requests were sent again", c.resent > 0)
    # 2. A new manifest request is a new snapshot, of the files as they are now.
    sid2, text2 = Client(addr).fetch_manifest()
    m2 = parse_manifest(text2)
    check("a new snapshot sees the new files", sid2 != sid and m2 and m2[2][0][0] == 1000)
    # 3. Requests the server must refuse or ignore.
    q = Client(addr)
    q.request(KERNEL, 0x12345678 if 0x12345678 not in server.snaps else 1, 0, 100)
    got = q.one()
    check("an unknown snapshot is GONE", got and got[0][4] == GONE)
    q.request(KERNEL, sid2, 1000, 100)
    got = q.one()
    check("past the end is RANGE", got and got[0][4] == RANGE)
    q.request(KERNEL, sid2, 990, 100)
    got = q.one()
    check("the last piece is short", got and got[0][4] == OK and got[0][8] == 10)
    bad = [REQ.pack(MAGIC, VERSION, REQUEST, KERNEL, 0, sid2, 0, 0, 0),        # length 0
           REQ.pack(MAGIC, VERSION, REQUEST, KERNEL, 0, sid2, 0, 1401, 0),     # too long
           REQ.pack(MAGIC, VERSION, REQUEST, 3, 0, sid2, 0, 10, 0),            # no such file
           REQ.pack(MAGIC, VERSION, REQUEST, KERNEL, 0, 0, 0, 10, 0),          # snapshot 0
           REQ.pack(MAGIC, VERSION, REQUEST, KERNEL, 1, sid2, 0, 10, 0),       # reserved
           REQ.pack(MAGIC ^ 1, VERSION, REQUEST, KERNEL, 0, sid2, 0, 10, 0),   # magic
           REQ.pack(MAGIC, VERSION, REPLY, KERNEL, 0, sid2, 0, 10, 0),         # a reply
           REQ.pack(MAGIC, VERSION, REQUEST, KERNEL, 0, sid2, 0, 10, 0) + b"x", b"", b"\0" * 7]
    for d in bad:
        q.sock.sendto(d, addr)
    check("malformed requests get no answer", q.one(0.5) is None)
    # 4. Old snapshots are dropped: the first fetch's is gone after SNAPSHOTS more.
    for _ in range(SNAPSHOTS):
        Client(addr).fetch_manifest()
    q.request(KERNEL, sid, 0, 100)
    got = q.one()
    check("an old snapshot is dropped", got and got[0][4] == GONE)
    # 5. The manifest's own rules (the C parser's, user/lib/update.c).
    good = manifest(b"k", b"b", "1.0", "abcdef0")
    check("a manifest parses", parse_manifest(good) is not None)
    for what, broken in (("no last newline", good[:-1]), ("a blank line after", good + b"\n"),
                         ("signed", good.replace(b"signature", b"signature 00")),
                         ("format 2", good.replace(b"jamos-update 1", b"jamos-update 2")),
                         ("a leading zero", good.replace(b"kernel 1", b"kernel 01"))):
        check("a manifest refused: " + what, parse_manifest(broken) is None)
    # 6. The version comes from the ELF symbol when there is a real build.
    elf = os.path.join(REPO, "build/jamos.elf")
    if os.path.exists(elf):
        v = elf_symbol_string(open(elf, "rb").read(), b"jamos_version")
        check("build/jamos.elf's jamos_version is %r" % v, bool(v) and VERSION_RE.match(v.encode()))
    stop.set()
    if fails:
        print("update-server self-test: FAIL (%d)" % len(fails))
        return 1
    print("update-server self-test: PASS")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build", default=os.path.join(REPO, "build"))
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--client", help="answer only this address (the PC's)")
    ap.add_argument("--manifest", nargs=2, metavar=("KERNEL", "BOOTFS"))
    ap.add_argument("--version")
    ap.add_argument("--git")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if a.manifest:
        k, b = (open(p, "rb").read() for p in a.manifest)
        sys.stdout.write(manifest(k, b, a.version or build_version(k), a.git or git_hash()).decode())
        return 0
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((a.bind, a.port))
    server = Server(os.path.join(a.build, "jamos.elf"), os.path.join(a.build, "bootfs.img"),
                    sock, client=a.client, version=a.version, git=a.git)
    print("update-server: serving %s on %s:%d" % (a.build, a.bind, a.port))
    try:
        while True:
            server.serve_one()
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
