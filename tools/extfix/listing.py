#!/usr/bin/env python3
"""List an ext2/3/4 image as Linux's debugfs reads it, for extcheck
(tools/extcheck.c): every name reachable from the root, with what its
inode says. Run by tools/extfix/make.sh on Linux, beside the image.

    listing.py <image> <out.list> [header lines...]

The header lines (the commands that made the image) become comments at
the top; a header line "orphans A B ..." becomes the line
"#orphans A B ..." that extcheck checks the orphan list against. Then one
line per name, sorted by path, its fields separated by tabs:

    path type ino mode uid gid links size blockcount atime ctime mtime what

  type        f (regular), d, l (symbolic link), c, b, p (FIFO), s (socket)
  mode        i_mode in octal, the type bits included
  blockcount  i_blocks as stored (512-byte units unless the inode's
              HUGE_FILE flag says blocks)
  times       seconds.nanoseconds, as debugfs's raw words decode (the
              seconds' two epoch bits from the extra word, the
              nanoseconds above them)
  what        regular files: the sparse hash (below); links: the target;
              devices: major,minor; others: -

The sparse hash is SHA-256 over every 4096-byte chunk of the file that is
not all zeros (the last one may be short), each preceded by its index as
8 bytes little-endian: a file of 5 GiB that is mostly holes hashes in a
moment on both sides, and a hole read as anything but zeros changes it.
Paths are the names from the root joined by '/' ("." and ".." left out;
the root itself is "/"). Names are bytes; one that is not printable ASCII
or contains a tab is written as "hex:" and its bytes in hex, paths and
targets alike."""
import hashlib
import re
import struct
import subprocess
import sys

CHUNK = 4096


def debugfs(img, cmd, binary=False):
    r = subprocess.run(["debugfs", "-R", cmd, img], capture_output=True, check=True)
    return r.stdout if binary else r.stdout.decode("utf-8", "surrogateescape")


def text(b):
    """A name or target as the list writes it."""
    if all(0x20 < c < 0x7f for c in b):
        return b.decode()
    return "hex:" + b.hex()


def ls(img, ino):
    """(name bytes, inode) of every entry of directory `ino`, but . and .."""
    out = []
    raw = subprocess.run(["debugfs", "-R", "ls -p <%d>" % ino, img], capture_output=True,
                         check=True).stdout
    for line in raw.split(b"\n"):
        if not line.startswith(b"/"):
            continue
        # /ino/mode/uid/gid/name/size/: the name is everything between the
        # fifth and the second-last slash (names have no slash).
        parts = line.split(b"/")
        name = b"/".join(parts[5:-2])
        if name in (b".", b"..") or parts[1] == b"0":
            continue
        out.append((name, int(parts[1])))
    return out


def decode_time(st, which):
    m = re.search(r"^\s*%s: 0x([0-9a-f]+)(?::([0-9a-f]+))?" % which, st, re.M)
    if not m:
        return "0.0"
    sec = struct.unpack("<i", struct.pack("<I", int(m.group(1), 16)))[0]
    extra = int(m.group(2), 16) if m.group(2) else 0
    return "%d.%d" % (sec + ((extra & 3) << 32), extra >> 2)


def sparse_hash(img, ino):
    h = hashlib.sha256()
    p = subprocess.Popen(["debugfs", "-R", "cat <%d>" % ino, img], stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL)
    zero = bytes(CHUNK)
    i = 0
    while True:
        chunk = p.stdout.read(CHUNK)
        if not chunk:
            break
        if chunk != zero[:len(chunk)]:
            h.update(struct.pack("<Q", i) + chunk)
        i += 1
    if p.wait() != 0:
        raise SystemExit("debugfs cat <%d> failed" % ino)
    return h.hexdigest()


def entry(img, path, ino):
    st = debugfs(img, "stat <%d>" % ino)
    m = re.search(r"Mode:\s+(\d+)", st)
    ls_mode = re.search(r"Type: (\S+)", st).group(1)
    user = re.search(r"User:\s+(\d+)\s+Group:\s+(\d+).*?Size:\s+(\d+)", st)
    links = int(re.search(r"Links: (\d+)", st).group(1))
    blocks = int(re.search(r"Blockcount: (\d+)", st).group(1))
    kinds = {"regular": "f", "directory": "d", "symlink": "l", "character": "c",
             "block": "b", "FIFO": "p", "socket": "s"}
    kind = kinds[ls_mode]
    mode = int(m.group(1), 8) | {"f": 0o100000, "d": 0o040000, "l": 0o120000, "c": 0o020000,
                                 "b": 0o060000, "p": 0o010000, "s": 0o140000}[kind]
    size = int(user.group(3))
    if kind == "f":
        what = sparse_hash(img, ino)
    elif kind == "l":
        fast = re.search(r'Fast link dest: "(.*)"', st, re.S)
        if fast:
            target = fast.group(1).encode("utf-8", "surrogateescape")
        else:
            target = debugfs(img, "cat <%d>" % ino, binary=True)[:size]
        what = text(target)
    elif kind in "cb":
        d = re.search(r"Device major/minor number: (\d+):(\d+)", st)
        what = "%d,%d" % (int(d.group(1)), int(d.group(2)))
    else:
        what = "-"
    return "\t".join([path, kind, str(ino), "%o" % mode, user.group(1), user.group(2),
                      str(links), str(size), str(blocks), decode_time(st, "atime"),
                      decode_time(st, "ctime"), decode_time(st, "mtime"), what])


def main():
    img, out = sys.argv[1], sys.argv[2]
    lines = []
    todo = [("/", 2)]
    seen = set()
    while todo:
        path, ino = todo.pop()
        lines.append(entry(img, path, ino))
        if path != "/" and lines[-1].split("\t")[1] != "d":
            continue
        if ino in seen:
            continue
        seen.add(ino)
        for name, child in ls(img, ino):
            cpath = ("" if path == "/" else path) + "/" + text(name)
            if lines and child in seen:
                raise SystemExit("directory loop at " + cpath)
            todo.append((cpath, child))
    with open(out, "w") as f:
        version = subprocess.run(["debugfs", "-V"], capture_output=True).stderr.decode()
        f.write("# %s, listed by tools/extfix/listing.py with %s\n" %
                (img.split("/")[-1], version.split("\n")[0]))
        for h in sys.argv[3:]:
            if h.startswith("orphans "):
                f.write("#orphans " + h[len("orphans "):] + "\n")
            else:
                f.write("# " + h + "\n")
        for line in sorted(lines):
            f.write(line + "\n")


main()
