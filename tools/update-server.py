#!/usr/bin/env python3
"""Serve the Mac's build to the PC's `update` (docs/M9-PLAN.md, "update: a
new build from the Mac"): build/jamos.elf, build/bootfs.img, the boot menu
boot/limine.conf and their manifest, over UDP.

    update-server.py [--build DIR] [--menu FILE | --no-menu] [--port 5022] [--bind ADDR]
                     [--client ADDR] [--key KEY]
        serve DIR/jamos.elf and DIR/bootfs.img (default build/) and the
        boot menu FILE (default the repository's boot/limine.conf, which
        `update -w` also writes to the stick) until ^C, each manifest
        signed with KEY (default ~/.config/jamos/update.key)
    update-server.py --manifest KERNEL BOOTFS [--menu FILE] [--version V] [--git G] [--net N]
                     [--key KEY] [--extra LINE]...
        print the manifest of those two files (and of the menu FILE: its
        `menu` line) and exit (signed with KEY if one is given, else with a
        bare signature line: unsigned), with each LINE as an extension line
        before the signature line
    update-server.py --build-net BOOTFS
        print the boot image's network default ("vlan21", "untagged", or
        "unknown" for a build made before build.txt had one); exit 1 if
        unknown (tools/flash-usb.sh asks it)
    update-server.py --self-test
        the server against a Python client on 127.0.0.1: a whole fetch
        with requests and replies lost, a snapshot kept while the files
        change, a dropped snapshot, malformed requests, signed manifests;
        exit 0 on PASS

The manifest (user/include/update.h has the C side's rules, and its strict
parser is user/lib/update.c):

    jamos-update 2
    version <the kernel's version string>
    git <short hash, -dirty if the tree has changes>
    net <the build's network default: vlan<id> or untagged>
    kernel <size> <sha256>
    bootfs <size> <sha256>
    menu <size> <sha256>          (an extension line: the boot menu)
    signature <128 hex digits>

Format 2 is the stable base: a later build adds extension lines
(`<key> [<value>]` before the signature line, signed with the rest), which
an older build skips, or must-understand ones (`!<key> ...`), which it
refuses ("needs a newer build"); so an old build can always be updated.
`menu` is one: a build that knows it fetches the menu too and `update -w`
writes it to the stick (if it passes the PC's check, which `make check`
also runs on boot/limine.conf: build/host/menucheck; the server logs its
verdict for each snapshot), an older build skips it.

The signature is Ed25519's (RFC 8032) over every byte before its line, made
by build/host/jamos-sign (tools/jamos-sign.c: the same Monocypher the PC
checks with) with the owner's key; the PC refuses a manifest its own build's
key didn't sign, and a build without a key refuses every one. The key is
made once: `build/host/jamos-sign keygen` (after `make`).

The protocol (user/include/updwire.h has the byte layout; every field is
little-endian): a request names a snapshot, a file (0 the manifest, 1 the
kernel, 2 the boot image, 3 the boot menu), an offset and a length of at most 1400 bytes,
and the manifest format the asking build reads (0 from builds older than
that field: format 2); the reply carries the same, the file's size and
the bytes. A new snapshot's manifest is made in the newest format the
asker reads (FORMAT_MIN..FORMAT_MAX are made here; one older than
FORMAT_MIN is refused, BAD, with a log line saying the build needs `make
flash`), so a newer server always serves an older build. A request for
the manifest with snapshot 0 makes a snapshot: the files read into memory
then (read again if any changed while being read, so a `make` running
meanwhile can't mix two builds), and every later request for that snapshot
is answered from that copy. The server keeps the last SNAPSHOTS snapshots
and nothing per client: a request for another snapshot gets GONE and the
fetcher starts again from the manifest. Malformed datagrams get no answer.

The version comes from the kernel's own `jamos_version` symbol in the ELF
file (so it is the build's, not the source's), else from kernel/main.c;
the git hash from the boot image's build.txt (the Makefile writes the
commit the build was made from), else the tree's when the snapshot is made;
the network default from the same build.txt's "net" line, and a boot image
without one is not served (init on the PC refuses a build whose default
isn't its own unless `update -f`).

For tests, tools/netpeer.py --update SPEC answers port 5022 with
peer_handler(SPEC): this server with a plan of damaged builds (below)."""
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
MANIFEST, KERNEL, BOOTFS, MENU = 0, 1, 2, 3
OK, GONE, RANGE, BAD = 0, 1, 2, 3
CHUNK_MAX = 1400
REQ = struct.Struct("<IBBBBIIHH")           # 20 bytes
REP = struct.Struct("<IBBBBIIIHH")          # 24 bytes, then the data
FILE_MAX = 32 << 20                         # <update.h> UPDATE_FILE_MAX
MENU_MAX = 64 << 10                         # <update.h> UPDATE_MENU_MAX
MANIFEST_MAX = 1024
FORMAT_MIN = FORMAT_MAX = 2                 # the manifest formats made here (<update.h>)
SNAPSHOTS = 8
SETTLE = 1.0                                # seconds both files must be unchanged
VERSION_RE = re.compile(rb"[A-Za-z0-9._+-]{1,47}$")
GIT_RE = re.compile(rb"[0-9a-f]{7,40}(-dirty)?$")
SIG_RE = re.compile(rb"signature( [0-9a-f]{128})?$")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIGN_TOOL = os.path.join(REPO, "build/host/jamos-sign")
MENU_TOOL = os.path.join(REPO, "build/host/menucheck")   # the PC's menu check (tools/menucheck.c)
DEFAULT_KEY = os.path.expanduser("~/.config/jamos/update.key")
DEFAULT_MENU = os.path.join(REPO, "boot/limine.conf")
# What a stick has once `update -w` has written a build (the Makefile's STICK_FILES).
STICK_FILES = ("/boot/jamos.elf", "/boot/bootfs.img", "/boot/prev-jamos.elf",
               "/boot/prev-bootfs.img")


# ---- the manifest ---------------------------------------------------------------

NET_RE = re.compile(rb"(untagged|vlan([1-9][0-9]{0,3}))$")


def net_ok(net):
    m = NET_RE.match(net.encode()) if net else None
    return bool(m) and (not m.group(2) or int(m.group(2)) <= 4094)


EXT_RE = re.compile(rb"!?[a-z0-9-]{1,32}( [\x20-\x7e]{1,200})?$")
BASE_KEYS = (b"jamos-update", b"version", b"git", b"net", b"kernel", b"bootfs", b"signature")


def ext_ok(line):
    """An extension line (<update.h>): a key of a-z 0-9 - (1..32 bytes, or
    '!' and 1..31: must-understand), none of format 2's names, then
    optionally a space and 1..200 printable bytes."""
    key = line.split(b" ", 1)[0]
    return bool(EXT_RE.match(line)) and len(key) <= 32 and key not in BASE_KEYS


def menu_line(menu):
    """The `menu` extension line for a boot menu's bytes."""
    if not 1 <= len(menu) <= MENU_MAX:
        raise ValueError("the boot menu is %d bytes (1..%d)" % (len(menu), MENU_MAX))
    return "menu %d %s" % (len(menu), hashlib.sha256(menu).hexdigest())


def manifest(kernel, bootfs, version, git, net, extra=(), fmt=FORMAT_MAX, menu=None):
    """The manifest's bytes for these two files' contents, in format fmt;
    menu: the boot menu's bytes (its `menu` line, after bootfs), or None;
    extra: extension lines (str), put before the signature line."""
    if not FORMAT_MIN <= fmt <= FORMAT_MAX:
        raise ValueError("manifest format %d isn't made here" % fmt)
    if not VERSION_RE.match(version.encode()) or not GIT_RE.match(git.encode()):
        raise ValueError("bad version %r or git %r" % (version, git))
    if not net_ok(net):
        raise ValueError("the build has no network default (build.txt's net line: %r); "
                         "make it again" % (net,))
    lines = ["jamos-update %d" % fmt, "version " + version, "git " + git, "net " + net]
    for name, data in (("kernel", kernel), ("bootfs", bootfs)):
        if not 1 <= len(data) <= FILE_MAX:
            raise ValueError("%s is %d bytes (1..%d)" % (name, len(data), FILE_MAX))
        lines.append("%s %d %s" % (name, len(data), hashlib.sha256(data).hexdigest()))
    if menu is not None:
        lines.append(menu_line(menu))
    for line in extra:
        if not ext_ok(line.encode()):
            raise ValueError("not an extension line: %r" % (line,))
        lines.append(line)
    lines.append("signature")
    text = ("\n".join(lines) + "\n").encode()
    assert len(text) <= MANIFEST_MAX
    return text


def sign(text, key):
    """The manifest text (its last line a bare "signature") signed with the
    secret key file `key`, by build/host/jamos-sign."""
    if not os.access(SIGN_TOOL, os.X_OK):
        raise OSError("no %s: run `make` first" % SIGN_TOOL)
    p = subprocess.run([SIGN_TOOL, "sign", key], input=text, capture_output=True)
    if p.returncode:
        raise OSError("can't sign: %s" % p.stderr.decode(errors="replace").strip())
    return p.stdout


MENU_RE = re.compile(rb"menu ([1-9][0-9]{0,9}) ([0-9a-f]{64})$")


def parse_manifest(text, old=False):
    """The C parser's rules, for the self-test: (version, git, [(size, sha)],
    net, needs, menu) or None; needs is the first must-understand extension
    line's key ("" if none), menu the `menu` line's (size, sha) or None.
    Other extension lines are checked and skipped; old=True parses as a
    build older than the `menu` line does (it skips that one too)."""
    lines = text.split(b"\n")
    if len(text) > MANIFEST_MAX or lines[0] != b"jamos-update 2" or lines[-1] != b"":
        return None
    needs, base, menu = "", [lines[0]], None
    for line in lines[1:-1]:
        if line.split(b" ", 1)[0] in BASE_KEYS:
            base.append(line)
        elif not ext_ok(line) or base[-1].startswith(b"signature"):
            return None
        elif line.split(b" ", 1)[0] == b"menu" and not old:
            m = MENU_RE.match(line)
            if not m or menu or int(m.group(1)) > MENU_MAX:
                return None
            menu = (int(m.group(1)), m.group(2).decode())
        elif line.startswith(b"!") and not needs:
            needs = line.split(b" ", 1)[0].decode()
    lines = base + [b""]
    if len(lines) != 8:
        return None
    v, g, n = lines[1].split(b" ", 1), lines[2].split(b" ", 1), lines[3].split(b" ", 1)
    if v[0] != b"version" or len(v) != 2 or not VERSION_RE.match(v[1]):
        return None
    if g[0] != b"git" or len(g) != 2 or not GIT_RE.match(g[1]):
        return None
    if n[0] != b"net" or len(n) != 2 or not net_ok(n[1].decode(errors="replace")):
        return None
    files = []
    for name, line in zip((b"kernel", b"bootfs"), lines[4:6]):
        w = line.split(b" ")
        if len(w) != 3 or w[0] != name or not re.match(rb"[1-9][0-9]{0,9}$", w[1]) or \
                not re.match(rb"[0-9a-f]{64}$", w[2]) or int(w[1]) > FILE_MAX:
            return None
        files.append((int(w[1]), w[2].decode()))
    if not SIG_RE.match(lines[6]):
        return None
    return v[1].decode(), g[1].decode(), files, n[1].decode(), needs, menu


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


BOOTFS_HEADER = struct.Struct("<8sIIQ")    # tools/mkbootfs.py's
BOOTFS_ENTRY = struct.Struct("<56sQQ")


def bootfs_file(img, name):
    """The bytes of file `name` in a boot image, or None."""
    if len(img) < BOOTFS_HEADER.size:
        return None
    magic, _, count, _ = BOOTFS_HEADER.unpack_from(img, 0)
    if magic != b"JAMBOOTF":
        return None
    for i in range(count):
        at = BOOTFS_HEADER.size + BOOTFS_ENTRY.size * i
        if at + BOOTFS_ENTRY.size > len(img):
            return None
        n, off, size = BOOTFS_ENTRY.unpack_from(img, at)
        if n.split(b"\0")[0] == name.encode():
            return img[off:off + size] if off + size <= len(img) else None
    return None


def build_git(bootfs):
    """The commit in the boot image's build.txt ("git 2079f35\\n"), or None."""
    text = bootfs_file(bootfs, "build.txt") or b""
    m = re.match(rb"git ([0-9a-f]{7,40}(-dirty)?)\n", text)
    return m.group(1).decode() if m else None


def build_net(bootfs):
    """The network default in the boot image's build.txt (its line
    "net vlan21" or "net untagged"), or None."""
    text = bootfs_file(bootfs, "build.txt") or b""
    for line in text.split(b"\n")[:-1]:
        if line.startswith(b"net ") and net_ok(line[4:].decode(errors="replace")):
            return line[4:].decode()
    return None


def other_net(net):
    """The other kind of network default: untagged for a VLAN, VLAN 21 for untagged."""
    return "vlan21" if net == "untagged" else "untagged"


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
    """The files' bytes, once none has changed for `settle` seconds, and
    read again if any changed during the read. (A snapshot taken while
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


def menu_verdict(menu):
    """The PC's check of a boot menu's bytes (build/host/menucheck, with the
    files a written stick has), as its one line, or None without the tool."""
    if not os.access(MENU_TOOL, os.X_OK):
        return None
    with tempfile.NamedTemporaryFile(suffix=".conf") as f:
        f.write(menu)
        f.flush()
        p = subprocess.run([MENU_TOOL, f.name] + list(STICK_FILES), capture_output=True,
                           text=True)
    return p.stdout.strip().replace(f.name, "the boot menu")


class Server:
    def __init__(self, kernel_path, bootfs_path, sock, client=None, log=print,
                 version=None, git=None, settle=SETTLE, key=None, net=None, menu_path=None):
        self.paths = [kernel_path, bootfs_path]
        self.menu_path = menu_path              # None: no boot menu served
        self.sock, self.client, self.log = sock, client, log
        self.version, self.git, self.settle, self.key = version, git, settle, key
        self.net = net
        self.snaps = collections.OrderedDict()   # id -> [manifest, kernel, bootfs, menu]

    def read_files(self):
        """The build's files as they are now, and the menu (None if not served)."""
        if not self.menu_path:
            return read_stable(self.paths, self.settle) + [None]
        return read_stable(self.paths + [self.menu_path], self.settle)

    def snapshot(self, fmt=FORMAT_MAX):
        """A new snapshot, its manifest in format fmt; its id."""
        kernel, bootfs, menu = self.read_files()
        text = manifest(kernel, bootfs, self.version or build_version(kernel),
                        self.git or build_git(bootfs) or git_hash(),
                        self.net or build_net(bootfs), fmt=fmt, menu=menu)
        text, kernel, bootfs, menu = self.prepare(text, kernel, bootfs, menu)
        text = self.signed(text)
        sid = 0
        while not sid or sid in self.snaps:
            sid = secrets.randbits(32)
        self.snaps[sid] = [text, kernel, bootfs, menu]
        while len(self.snaps) > SNAPSHOTS:
            self.snaps.popitem(last=False)
        self.log("update-server: snapshot %08x: %s" % (sid, text.decode().split("\n")[1:4]))
        if menu is not None:
            self.log("update-server: snapshot %08x: boot menu %d bytes; %s" %
                     (sid, len(menu), menu_verdict(menu) or "no build/host/menucheck to "
                      "check it with (make)"))
        return sid

    def prepare(self, text, kernel, bootfs, menu):
        """A snapshot's manifest and files as served (PlannedServer's hook)."""
        return text, kernel, bootfs, menu

    def signed(self, text):
        """The manifest as served: signed with the key (PlannedServer's hook)."""
        return sign(text, self.key) if self.key else text

    def answer(self, dgram):
        """The reply to one datagram, or None (not a request of ours)."""
        if len(dgram) != REQ.size:
            return None
        magic, ver, typ, f, fmt, sid, off, length, res2 = REQ.unpack(dgram)
        if magic != MAGIC or ver != VERSION or typ != REQUEST or res2 or \
                f > MENU or not 1 <= length <= CHUNK_MAX or (not sid and f != MANIFEST):
            return None
        # The manifest format the asker reads (0: a build older than the
        # field, which reads 2): the newest made here that it reads.
        want = 2 if fmt == 0 else min(fmt, FORMAT_MAX)
        if want < FORMAT_MIN:
            self.log("update-server: a build that reads manifest format %d asked; the oldest "
                     "made here is %d (signed): it needs `make flash`" % (fmt, FORMAT_MIN))
            return REP.pack(MAGIC, VERSION, REPLY, f, BAD, sid, off, 0, 0, 0)
        if not sid:
            try:
                sid = self.snapshot(want)
            except (OSError, ValueError) as e:
                self.log("update-server: can't snapshot the build: %s" % e)
                return None
        snap = self.snaps.get(sid)
        if snap is None:
            return REP.pack(MAGIC, VERSION, REPLY, f, GONE, sid, off, 0, 0, 0)
        data = snap[f]
        if data is None:   # no boot menu in this snapshot: nothing to ask for
            return REP.pack(MAGIC, VERSION, REPLY, f, RANGE, sid, off, 0, 0, 0)
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


# ---- a server for tests: a plan of damaged builds ------------------------------------

PLANS = ("good", "damage", "wronghash", "truncated", "gone", "unsigned", "badsig", "othernet",
         "extension", "mustknow", "menudamage", "nomenu")
EXTENSION_LINE = "future-note a line a later build may add"   # the "extension" plan's
MUSTKNOW_LINE = "!future-must 1"                               # the "mustknow" plan's
GONE_AFTER = 300        # replies to a "gone" client before the server stops answering it


class PlannedServer(Server):
    """The server as the PC's tests meet it (tools/update-net-test.sh, through
    tools/netpeer.py --update): each new client (address and port: one
    `update` run) gets the next plan in the list, then "good":
      good       the build as it is
      damage     a byte of the kernel changed after the manifest was made
                 (bytes damaged on the way): init refuses its SHA-256
      wronghash  the manifest's SHA-256 for the boot image is wrong: init
                 refuses it
      truncated  the boot image served is half the size the manifest says:
                 its pieces carry that size, so the fetcher ignores them
                 and gives up
      gone       no answer after GONE_AFTER replies (the server stopped
                 mid-fetch): the fetcher gives up
      unsigned   the manifest's signature line bare: init refuses it
      badsig     the manifest changed after it was signed (its version's
                 last character): init refuses the signature
      othernet   the manifest (signed as usual) says the other network
                 default (untagged for a VLAN build, vlan21 for an untagged
                 one): init refuses it unless forced (`update -f`)
      extension  the build, its manifest (signed) with an extension line
                 no build knows (EXTENSION_LINE): taken as "good" is
      mustknow   the same with a must-understand line (MUSTKNOW_LINE):
                 init refuses it, saying it needs a newer build
      menudamage a byte of the boot menu changed after the manifest was made
                 (the spec has a menu): init refuses the menu's SHA-256, and
                 with it the whole offer
      nomenu     the build without its boot menu and the manifest without
                 its `menu` line, as a server older than the line serves it
    Every other manifest is signed with the spec's key (a throwaway test
    key: the PC under test has its public half)."""

    def __init__(self, kernel_path, bootfs_path, plan, log=print, version=None, git=None,
                 key=None, net=None, menu_path=None):
        super().__init__(kernel_path, bootfs_path, None, log=log, version=version, git=git,
                         settle=0.0, key=key, net=net, menu_path=menu_path)
        for p in plan:
            if p not in PLANS:
                raise ValueError("no plan %r (%s)" % (p, ", ".join(PLANS)))
        self.plan = list(plan)
        self.clients = {}       # (address, port) -> [plan, replies]
        self.current = "good"   # the plan of the request being answered

    def read_files(self):
        kernel, bootfs, menu = super().read_files()
        return [kernel, bootfs, None if self.current == "nomenu" else menu]

    def prepare(self, text, kernel, bootfs, menu):
        if self.current == "menudamage" and menu:
            m = bytearray(menu)
            m[len(m) // 2] ^= 0x20
            menu = bytes(m)
        elif self.current == "damage":
            k = bytearray(kernel)
            k[len(k) // 2] ^= 0x20
            kernel = bytes(k)
        elif self.current == "wronghash":
            good = hashlib.sha256(bootfs).hexdigest().encode()
            text = text.replace(good, hashlib.sha256(b"not the boot image").hexdigest().encode())
        elif self.current == "truncated":
            bootfs = bootfs[:len(bootfs) // 2]
        elif self.current == "othernet":
            lines = text.split(b"\n")
            lines[3] = b"net " + other_net(lines[3][4:].decode()).encode()
            text = b"\n".join(lines)
        elif self.current in ("extension", "mustknow"):
            line = EXTENSION_LINE if self.current == "extension" else MUSTKNOW_LINE
            text = text.replace(b"\nsignature\n", b"\n" + line.encode() + b"\nsignature\n")
        return text, kernel, bootfs, menu

    def signed(self, text):
        if self.current == "unsigned":
            return text
        text = super().signed(text)
        if self.current == "badsig":
            at = text.index(b"\ngit ") - 1   # the version's last character
            text = text[:at] + (b"X" if text[at:at + 1] != b"X" else b"Y") + text[at + 1:]
        return text

    def handle(self, client, dgram):
        """The reply to dgram from client (address, port), or None."""
        if client not in self.clients:
            plan = self.plan.pop(0) if self.plan else "good"
            self.clients[client] = [plan, 0]
            self.log("update-server: %s:%d gets the plan %r" % (client[0], client[1], plan))
        c = self.clients[client]
        if c[0] == "gone" and c[1] >= GONE_AFTER:
            return None
        self.current = c[0]
        rep = self.answer(dgram)
        if rep is not None:
            c[1] += 1
        return rep


def peer_handler(spec_path, log):
    """tools/netpeer.py's handler for port 5022: a PlannedServer from the
    JSON file spec_path ({"kernel": path, "bootfs": path, "plan": [...],
    and optionally "version", "git", "net", "key": the secret key to sign
    with, "menu": a boot menu to serve with the build; none without it})."""
    import json
    with open(spec_path) as f:
        spec = json.load(f)
    server = PlannedServer(spec["kernel"], spec["bootfs"], spec.get("plan", []), log=log,
                           version=spec.get("version"), git=spec.get("git"),
                           key=spec.get("key"), net=spec.get("net"),
                           menu_path=spec.get("menu"))

    def handle(peer, src, sport, dst, payload):
        return server.handle((socket.inet_ntoa(src), sport), payload)
    return handle


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
            self.sock.sendto(REQ.pack(MAGIC, VERSION, REQUEST, f, FORMAT_MAX, sid, off, length,
                                      0), self.addr)

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


def check_menu(kpath, bpath, tmp, check):
    """The boot menu: its `menu` line (an extension line an older build's
    grammar skips), served as file 3 of a snapshot; a server without one
    answers RANGE for it; the "menudamage" and "nomenu" plans."""
    mpath = os.path.join(tmp, "limine.conf")
    menu = open(os.path.join(REPO, "boot/limine.conf"), "rb").read()
    open(mpath, "wb").write(menu)
    line = menu_line(menu).encode()
    check("the menu line is an extension line", ext_ok(line))
    text = manifest(b"k", b"b", "1.0", "abcdef0", "vlan21", menu=menu)
    m, old = parse_manifest(text), parse_manifest(text, old=True)
    check("a manifest with a menu parses", m is not None and
          m[5] == (len(menu), hashlib.sha256(menu).hexdigest()))
    check("an older build's parser skips the menu line",
          old is not None and old[:5] == m[:5] and old[5] is None)
    for what, bad in (("twice", text.replace(line, line + b"\n" + line)),
                      ("size 0", text.replace(b"menu %d " % len(menu), b"menu 0 ")),
                      ("too big", text.replace(b"menu %d " % len(menu), b"menu 65537 ")),
                      ("short hash", text.replace(line, line[:-1]))):
        check("a menu line refused: " + what, parse_manifest(bad) is None)
    for size in (0, MENU_MAX + 1):
        try:
            manifest(b"k", b"b", "1.0", "abcdef0", "vlan21", menu=b"x" * size)
            check("no manifest with a %d-byte menu" % size, False)
        except ValueError:
            pass
    s = PlannedServer(kpath, bpath, ["menudamage", "nomenu"], log=lambda s: None,
                      version="0.0.29-test", git="abcdef0", net="vlan21", menu_path=mpath)

    def ask(client, f, sid, off, length):
        rep = s.handle(client, REQ.pack(MAGIC, VERSION, REQUEST, f, 0, sid, off, length, 0))
        return (REP.unpack_from(rep), rep[REP.size:]) if rep else (None, b"")
    for n, plan in enumerate(["menudamage", "nomenu", "good"]):
        client = ("10.2.21.5", 52000 + n)
        fields, text = ask(client, MANIFEST, 0, 0, CHUNK_MAX)
        m, sid = parse_manifest(text), fields[5]
        got = b"".join(ask(client, MENU, sid, off, CHUNK_MAX)[1]
                       for off in range(0, len(menu), CHUNK_MAX))
        if plan == "nomenu":
            check("the plan 'nomenu': no menu line, file 3 is RANGE", m[5] is None and
                  ask(client, MENU, sid, 0, 10)[0][4] == RANGE)
        else:
            good = m[5] == (len(got), hashlib.sha256(got).hexdigest())
            check("the plan %r: the menu %s its line" % (plan, "matches" if good else "breaks"),
                  good == (plan == "good") and got != b"" and m[5][0] == len(menu))


def check_plans(kpath, bpath, check):
    """PlannedServer: each client its plan, in order, then "good"."""
    plans = ["damage", "wronghash", "truncated", "gone", "othernet", "extension", "mustknow"]
    s = PlannedServer(kpath, bpath, plans, log=lambda s: None, version="0.0.29-test",
                      git="abcdef0", net="vlan21")

    def ask(client, f, sid, off, length):
        rep = s.handle(client, REQ.pack(MAGIC, VERSION, REQUEST, f, 0, sid, off, length, 0))
        return (REP.unpack_from(rep), rep[REP.size:]) if rep else (None, b"")

    def whole(client, f, sid, size):
        return b"".join(ask(client, f, sid, off, CHUNK_MAX)[1] for off in range(0, size, CHUNK_MAX))

    results = {}
    for n, plan in enumerate(plans + ["good"]):
        client = ("10.2.21.5", 50000 + n)
        fields, text = ask(client, MANIFEST, 0, 0, CHUNK_MAX)
        m = parse_manifest(text)
        sid = fields[5]
        (ksize, ksha), (bsize, bsha) = m[2]
        if plan == "othernet":
            results[plan] = m[3] == "untagged"
        elif plan in ("extension", "mustknow"):
            line = (EXTENSION_LINE if plan == "extension" else MUSTKNOW_LINE).encode()
            results[plan] = b"\n" + line + b"\nsignature" in text and \
                m[4] == ("" if plan == "extension" else MUSTKNOW_LINE.split()[0])
        elif plan == "truncated":
            results[plan] = ask(client, BOOTFS, sid, bsize - 10, 10)[0][4] == RANGE
        elif plan == "gone":
            for _ in range(GONE_AFTER):
                ask(client, KERNEL, sid, 0, 10)
            results[plan] = ask(client, KERNEL, sid, 0, 10)[0] is None
        else:
            k, b = whole(client, KERNEL, sid, ksize), whole(client, BOOTFS, sid, bsize)
            good = hashlib.sha256(k).hexdigest() == ksha and hashlib.sha256(b).hexdigest() == bsha
            results[plan] = good if plan == "good" else not good
    for plan, ok in results.items():
        check("the test plan %r" % plan, ok)


def check_signing(kpath, bpath, tmp, check):
    """Signed manifests (build/host/jamos-sign): a throwaway key; a manifest
    signed with it parses and checks; one changed byte, the "unsigned" and
    "badsig" plans, and another key's signature don't."""
    if not os.access(SIGN_TOOL, os.X_OK):
        print("update-server self-test: signing skipped: no %s (run make)" % SIGN_TOOL)
        return
    keys = [os.path.join(tmp, d) for d in ("key1", "key2")]
    for d in keys:
        subprocess.run([SIGN_TOOL, "keygen", d], check=True, capture_output=True)

    def verified(text, d):
        return subprocess.run([SIGN_TOOL, "verify", os.path.join(d, "update.pub")], input=text,
                              capture_output=True).returncode == 0
    text = sign(manifest(b"k", b"b", "1.0", "abcdef0", "vlan21"),
                os.path.join(keys[0], "update.key"))
    check("a signed manifest parses", parse_manifest(text) is not None)
    check("its signature checks", verified(text, keys[0]))
    check("another key's doesn't", not verified(text, keys[1]))
    check("a changed byte doesn't", not verified(text.replace(b"1.0", b"1.1"), keys[0]))
    s = PlannedServer(kpath, bpath, ["unsigned", "badsig"], log=lambda s: None,
                      version="0.0.29-test", git="abcdef0",
                      key=os.path.join(keys[0], "update.key"), net="vlan21")
    for n, plan in enumerate(["unsigned", "badsig", "good"]):
        rep = s.handle(("10.2.21.5", 51000 + n),
                       REQ.pack(MAGIC, VERSION, REQUEST, MANIFEST, 0, 0, 0, CHUNK_MAX, 0))
        text = rep[REP.size:]
        check("the plan %r: %s" % (plan, "signed" if plan == "good" else "refused"),
              parse_manifest(text) is not None and verified(text, keys[0]) == (plan == "good"))


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
                    settle=0.0, net="untagged")
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
    check("the manifest parses", m is not None and m[0] == "0.0.29-test" and m[1] == "abcdef0"
          and m[3] == "untagged")
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
           REQ.pack(MAGIC, VERSION, REQUEST, 4, 0, sid2, 0, 10, 0),            # no such file
           REQ.pack(MAGIC, VERSION, REQUEST, KERNEL, 0, 0, 0, 10, 0),          # snapshot 0
           REQ.pack(MAGIC, VERSION, REQUEST, KERNEL, 2, sid2, 0, 10, 1),       # reserved
           REQ.pack(MAGIC ^ 1, VERSION, REQUEST, KERNEL, 0, sid2, 0, 10, 0),   # magic
           REQ.pack(MAGIC, VERSION, REPLY, KERNEL, 0, sid2, 0, 10, 0),         # a reply
           REQ.pack(MAGIC, VERSION, REQUEST, KERNEL, 0, sid2, 0, 10, 0) + b"x", b"", b"\0" * 7]
    for d in bad:
        q.sock.sendto(d, addr)
    check("malformed requests get no answer", q.one(0.5) is None)
    # 3b. The manifest in the format the asker reads: 2 for an older build's
    # request (no format said), for 2, and for a newer build's 3 (the newest
    # made here); none for 1 (before signing), which can't be made.
    for fmt, want in ((0, b"jamos-update 2\n"), (2, b"jamos-update 2\n"),
                      (3, b"jamos-update 2\n"), (1, None)):
        q.sock.sendto(REQ.pack(MAGIC, VERSION, REQUEST, MANIFEST, fmt, 0, 0, CHUNK_MAX, 0), addr)
        got = q.one()
        if want is None:
            check("a format-1 asker refused (BAD)", got and got[0][4] == BAD and not got[1])
        else:
            check("a format-%d asker gets format 2" % fmt,
                  got and got[0][4] == OK and got[1].startswith(want))
    # 4. Old snapshots are dropped: the first fetch's is gone after SNAPSHOTS more.
    for _ in range(SNAPSHOTS):
        Client(addr).fetch_manifest()
    q.request(KERNEL, sid, 0, 100)
    got = q.one()
    check("an old snapshot is dropped", got and got[0][4] == GONE)
    # 5. The manifest's own rules (the C parser's, user/lib/update.c).
    good = manifest(b"k", b"b", "1.0", "abcdef0", "vlan21")
    check("a manifest parses", parse_manifest(good) is not None)
    check("an untagged manifest parses",
          parse_manifest(manifest(b"k", b"b", "1.0", "abcdef0", "untagged"))[3] == "untagged")
    for what, broken in (("no last newline", good[:-1]), ("a blank line after", good + b"\n"),
                         ("a short signature", good.replace(b"signature", b"signature 00")),
                         ("format 1", good.replace(b"jamos-update 2", b"jamos-update 1")),
                         ("format 3", good.replace(b"jamos-update 2", b"jamos-update 3")),
                         ("no net line", good.replace(b"net vlan21\n", b"")),
                         ("vlan 0", good.replace(b"net vlan21", b"net vlan0")),
                         ("vlan 4095", good.replace(b"net vlan21", b"net vlan4095")),
                         ("vlan 021", good.replace(b"net vlan21", b"net vlan021")),
                         ("net off", good.replace(b"net vlan21", b"net off")),
                         ("a leading zero", good.replace(b"kernel 1", b"kernel 01"))):
        check("a manifest refused: " + what, parse_manifest(broken) is None)
    ext = manifest(b"k", b"b", "1.0", "abcdef0", "vlan21", [EXTENSION_LINE, "x", MUSTKNOW_LINE])
    m = parse_manifest(ext)
    check("extension lines skipped, the first must-understand one named",
          m is not None and m[0] == "1.0" and m[4] == MUSTKNOW_LINE.split()[0])
    check("no must-understand line: none named", parse_manifest(good)[4] == "")
    for what, broken in (("an upper-case key", b"Future 1"), ("an empty value", b"future "),
                         ("a 33-byte key", b"k" * 33), ("a bare !", b"!"),
                         ("a format line's name", b"version 2")):
        check("an extension line refused: " + what,
              parse_manifest(good.replace(b"signature", broken + b"\nsignature")) is None)
    check("no extension line after the signature",
          parse_manifest(good + b"future 1\n") is None)
    for line in ("Future 1", "version 2", "!", "x" * 33):
        try:
            manifest(b"k", b"b", "1.0", "abcdef0", "vlan21", [line])
            check("no manifest with the extension line %r" % (line,), False)
        except ValueError:
            pass
    for net in (None, "", "off", "vlan0", "vlan4095", "none"):
        try:
            manifest(b"k", b"b", "1.0", "abcdef0", net)
            check("no manifest with net %r" % (net,), False)
        except ValueError:
            pass
    # 6. The version comes from the ELF symbol when there is a real build.
    elf = os.path.join(REPO, "build/jamos.elf")
    if os.path.exists(elf):
        v = elf_symbol_string(open(elf, "rb").read(), b"jamos_version")
        check("build/jamos.elf's jamos_version is %r" % v, bool(v) and VERSION_RE.match(v.encode()))
    # 7. The git hash from a boot image's build.txt; the test plans.
    entry_at = BOOTFS_HEADER.size + BOOTFS_ENTRY.size
    img = (BOOTFS_HEADER.pack(b"JAMBOOTF", 1, 1, entry_at + 16) +
           BOOTFS_ENTRY.pack(b"build.txt", entry_at, 16) + b"git 0123abc-dirty\n"[:16])
    check("a cut build.txt has no git hash", build_git(img) is None)
    img = img[:entry_at - 16] + struct.pack("<QQ", entry_at, 18) + b"git 0123abc-dirty\n"
    check("a boot image's git hash", build_git(img) == "0123abc-dirty")
    check("a build.txt without a net line has no network default", build_net(img) is None)
    for text, want in ((b"git 0123abc\nnet vlan21\n", "vlan21"),
                       (b"git 0123abc\nnet untagged\n", "untagged"),
                       (b"git 0123abc\nnet vlan21", None), (b"git 0123abc\nnet vlan5000\n", None),
                       (b"git 0123abc\nnet none\n", None)):
        img = (BOOTFS_HEADER.pack(b"JAMBOOTF", 1, 1, entry_at + len(text)) +
               BOOTFS_ENTRY.pack(b"build.txt", entry_at, len(text)) + text)
        check("build.txt %r: network default %r" % (text, want), build_net(img) == want)
    check_plans(kpath, bpath, check)
    check_menu(kpath, bpath, tmp, check)
    check_signing(kpath, bpath, tmp, check)
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
    ap.add_argument("--key", help="the secret key to sign with (default %s)" % DEFAULT_KEY)
    ap.add_argument("--net", help="the network default to claim (vlan<id>, untagged)")
    ap.add_argument("--extra", action="append", default=[], metavar="LINE",
                    help="--manifest: an extension line to add (again for more)")
    ap.add_argument("--menu", metavar="FILE",
                    help="the boot menu to serve (default %s); with --manifest, its `menu` "
                         "line" % DEFAULT_MENU)
    ap.add_argument("--no-menu", action="store_true",
                    help="serve no boot menu (as a server older than the `menu` line)")
    ap.add_argument("--build-net", metavar="BOOTFS")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if a.build_net:
        net = build_net(open(a.build_net, "rb").read())
        print(net or "unknown")
        return 0 if net else 1
    if a.manifest:
        k, b = (open(p, "rb").read() for p in a.manifest)
        menu = open(a.menu, "rb").read() if a.menu else None
        text = manifest(k, b, a.version or build_version(k), a.git or build_git(b) or git_hash(),
                        a.net or build_net(b), a.extra, menu=menu)
        sys.stdout.write((sign(text, a.key) if a.key else text).decode())
        return 0
    key = a.key or DEFAULT_KEY
    if not os.path.exists(key):
        print("update-server: no update key %s: the PC takes only signed builds.\n"
              "  Make one, once: build/host/jamos-sign keygen (after make); then make and\n"
              "  make flash, so the stick has a build with its public half." % key)
        return 1
    sign(b"signature\n", key)   # the key and the tool work, before anyone asks
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((a.bind, a.port))
    menu = None if a.no_menu else a.menu or DEFAULT_MENU
    server = Server(os.path.join(a.build, "jamos.elf"), os.path.join(a.build, "bootfs.img"),
                    sock, client=a.client, version=a.version, git=a.git, key=key, net=a.net,
                    menu_path=menu)
    print("update-server: serving %s on %s:%d, signed with %s; boot menu: %s" %
          (a.build, a.bind, a.port, key, menu or "none (--no-menu)"))
    try:
        while True:
            server.serve_one()
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
