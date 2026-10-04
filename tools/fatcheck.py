#!/usr/bin/env python3
"""Check a FAT32 volume for damage, read-only (docs/M11.6-PLAN.md: the
kill-storm's checker; tools/fat-storm-test.sh runs it on QEMU's images, the
owner on the stick after the PC's storm).

    fatcheck.py <image or device>[@@<offset>] [-p N] [--require-clean]
                [--list N] [-q]
    fatcheck.py --selftest

The volume starts at <offset> bytes into the file (a number with an
optional K, M or G: mtools' img@@64M), or at partition N of the MBR at the
file's start (-p 2: the Jam OS stick's /data). On a Mac a stick's
partition can be read straight from its raw device (sudo fatcheck.py
/dev/rdisk4s2, after `diskutil unmountDisk`); nothing is ever written.

What is checked:
  - the boot sector (BPB): sizes and counts that a FAT32 volume can have;
  - every FAT holds the same entries (the 28 bits FAT32 uses);
  - every cluster chain, from each directory entry: every link in range,
    never to a free, reserved or bad cluster, no loop, and no cluster in
    two chains (cross-linked);
  - every directory entry: a short name of valid characters, attributes
    that make sense, "." and ".." where they belong and pointing right,
    long-name pieces in order and matching their short entry's checksum;
  - each file's size against its chain: exactly ceil(size / cluster)
    clusters (an empty file has none); a directory's size 0 and a chain;
  - lost clusters: allocated in the FAT but reached by no entry, counted
    and listed as chains (their first clusters);
  - notes, not damage: the FSInfo sector's free count and hint, and the
    volume's dirty bit (FAT[1]; damage with --require-clean).

Exit status: 0 clean; 1 damage (anything above that isn't a note or a lost
cluster); 2 lost clusters only; 3 not a FAT32 volume, or unreadable.

--selftest [-v] builds small FAT32 images in memory, damages each in one known
way, and checks that each is found (exit 0 on PASS)."""
import io
import struct
import sys

EOC = 0x0FFFFFF8      # this and above: the end of a chain
BAD = 0x0FFFFFF7      # a bad cluster
MASK = 0x0FFFFFFF     # FAT32 uses 28 bits of each entry
CLEAN_BIT = 0x08000000   # FAT[1]: the volume was unmounted cleanly
HARD_ERR_BIT = 0x04000000   # FAT[1]: no disk error was seen
ATTR_RO, ATTR_HIDDEN, ATTR_SYSTEM, ATTR_VOLUME, ATTR_DIR, ATTR_ARCHIVE = 1, 2, 4, 8, 16, 32
ATTR_LFN = 0x0F
SHORT_OK = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !#$%&'()-@^_`{}~") | set(range(0x80, 0x100))


class NotFat(Exception):
    pass


class Volume:
    """The volume's geometry and FATs, read from file f at byte `base`."""

    def __init__(self, f, base):
        self.f, self.base = f, base
        bs = self.read(0, 512)
        if len(bs) < 512 or bs[510:512] != b"\x55\xaa":
            raise NotFat("no boot sector signature (55 aa) at the volume's start")
        (self.bps, self.spc, self.reserved, self.nfats, root_entries, tot16, self.media,
         fatsz16) = struct.unpack_from("<HBHBHHBH", bs, 11)
        tot32, fatsz32 = struct.unpack_from("<II", bs, 32)
        self.ext_flags, self.version, self.root, self.fsinfo, self.bkboot = \
            struct.unpack_from("<HHIHH", bs, 40)
        if self.bps not in (512, 1024, 2048, 4096):
            raise NotFat("bytes per sector %d" % self.bps)
        if self.spc == 0 or self.spc & (self.spc - 1):
            raise NotFat("sectors per cluster %d" % self.spc)
        if fatsz16 or root_entries or not fatsz32:
            raise NotFat("a FAT12/16 boot sector (FAT32 only)")
        if not self.reserved or not 1 <= self.nfats <= 4:
            raise NotFat("%d reserved sectors, %d FATs" % (self.reserved, self.nfats))
        self.total = tot32 or tot16
        self.fatsz = fatsz32
        self.data_start = self.reserved + self.nfats * self.fatsz
        if self.data_start >= self.total:
            raise NotFat("the FATs are bigger than the volume")
        self.csize = self.bps * self.spc
        self.nclusters = (self.total - self.data_start) // self.spc
        self.max = self.nclusters + 1   # the highest cluster number
        if self.nclusters < 65525:
            raise NotFat("%d clusters: too few for FAT32" % self.nclusters)
        if self.fatsz * self.bps < (self.max + 1) * 4:
            raise NotFat("the FAT is too small for %d clusters" % self.nclusters)
        if not 2 <= self.root <= self.max:
            raise NotFat("root directory cluster %d" % self.root)
        self.fats = []
        for i in range(self.nfats):
            raw = self.read((self.reserved + i * self.fatsz) * self.bps, (self.max + 1) * 4)
            if len(raw) < (self.max + 1) * 4:
                raise NotFat("the volume ends inside FAT %d" % (i + 1))
            self.fats.append([e & MASK for e in struct.unpack("<%dI" % (self.max + 1), raw)])
            if i == 0:
                self.fat1_raw = struct.unpack_from("<I", raw, 4)[0]
        self.fat = self.fats[0]

    def read(self, off, n):
        self.f.seek(self.base + off)
        return self.f.read(n)

    def cluster(self, c):
        return self.read((self.data_start + (c - 2) * self.spc) * self.bps, self.csize)


class Check:
    def __init__(self, vol, listmax):
        self.v, self.listmax = vol, listmax
        self.damage, self.notes = [], []
        self.owner = {}      # cluster -> the path whose chain holds it
        self.lost = []       # lost chains: (first cluster, length)

    def bad(self, what):
        self.damage.append(what)

    # ---- the FATs -----------------------------------------------------------
    def fats_agree(self):
        v = self.v
        for i in range(1, v.nfats):
            diff = [c for c in range(2, v.max + 1) if v.fats[i][c] != v.fat[c]]
            if diff:
                self.bad("FAT %d differs from FAT 1 in %d entries (the first: cluster %d, "
                         "%#x vs %#x)" % (i + 1, len(diff), diff[0], v.fats[i][diff[0]],
                                          v.fat[diff[0]]))
        if v.fat[0] & 0xFF != v.media:
            self.bad("FAT[0] %#x doesn't hold the media byte %#x" % (v.fat[0], v.media))
        if not v.fat1_raw & CLEAN_BIT:
            self.notes.append("the volume is marked dirty (FAT[1]'s clean bit is 0)")
        if not v.fat1_raw & HARD_ERR_BIT:
            self.notes.append("the volume is marked as having had disk errors (FAT[1])")

    # ---- chains -------------------------------------------------------------
    def chain(self, first, path):
        """The clusters of the chain starting at first, claimed for path; None if broken."""
        v, seen, out, c = self.v, set(), [], first
        while True:
            if not 2 <= c <= v.max:
                self.bad("%s: its chain reaches cluster %d, out of range (after %d clusters)"
                         % (path, c, len(out)))
                return None
            if c in seen:
                self.bad("%s: its chain loops back to cluster %d" % (path, c))
                return None
            nxt = v.fat[c]
            if nxt == 0:
                self.bad("%s: its chain reaches cluster %d, which is free" % (path, c))
                return None
            if nxt == 1 or nxt == BAD:
                self.bad("%s: its chain reaches cluster %d, marked %s" %
                         (path, c, "bad" if nxt == BAD else "reserved"))
                return None
            if c in self.owner:
                self.bad("%s and %s are cross-linked at cluster %d" % (self.owner[c], path, c))
                return None
            seen.add(c)
            out.append(c)
            self.owner[c] = path
            if nxt >= EOC:
                return out
            c = nxt

    # ---- directories --------------------------------------------------------
    @staticmethod
    def lfn_sum(name11):
        s = 0
        for b in name11:
            s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
        return s

    def short_name_ok(self, name11, path):
        body = name11[:8].rstrip(b" "), name11[8:].rstrip(b" ")
        if name11[0] == 0x20:
            self.bad("%s: a short name starting with a space" % path)
            return False
        for k, b in enumerate(name11):
            if b not in SHORT_OK and not (b == 0x05 and k == 0):   # 0x05: a first byte 0xE5
                self.bad("%s: the short name %r has a character a FAT name can't (%#x)"
                         % (path, bytes(name11), b))
                return False
        if b" " in body[0] or b" " in body[1]:
            self.bad("%s: the short name %r has a space inside it" % (path, bytes(name11)))
            return False
        return True

    def entries(self, clusters):
        """(index, 32-byte entry) of a directory's chain, up to its end mark."""
        i = 0
        for c in clusters:
            data = self.v.cluster(c)
            for off in range(0, len(data), 32):
                e = data[off:off + 32]
                if e[0] == 0:
                    return
                yield i, e
                i += 1

    def walk(self):
        v = self.v
        root = self.chain(v.root, "/")
        if root is None:
            return
        todo = [("/", root, v.root, 0)]   # path, its clusters, its first cluster, parent's
        while todo:
            path, clusters, me, parent = todo.pop()
            for sub in self.directory(path, clusters, me, parent):
                todo.append(sub)

    def directory(self, path, clusters, me, parent):
        """Check one directory's entries; the subdirectories to walk next."""
        subs, lfn, dots = [], [], 0
        is_root = path == "/"
        for i, e in self.entries(clusters):
            if e[0] == 0xE5:
                lfn = []
                continue
            attr = e[11]
            if attr & 0x3F == ATTR_LFN:
                lfn.append(e)
                continue
            name = e[0:11]
            pending, lfn = lfn, []
            if attr & ATTR_VOLUME:
                if not is_root or attr & ATTR_DIR:
                    self.bad("%s: a volume label entry (entry %d) outside the root" % (path, i))
                if pending:   # mtools writes these; fsck_msdos warns, nothing is lost
                    self.notes.append("long-name pieces before the volume label")
                continue
            if name in (b".          ", b"..         "):
                dots += 1
                self.dot(path, name, e, me, parent, i, is_root)
                continue
            full = path.rstrip("/") + "/" + self.display(name, pending)
            if pending:
                self.lfn_ok(pending, name, full)
            if not self.short_name_ok(name, full):
                continue
            sub = self.entry(full, e, attr)
            if sub:
                subs.append((full, sub[0], sub[1], me if not is_root else 0))
        if not is_root and dots != 2:
            self.bad("%s: %d of its \".\" and \"..\" entries, not 2" % (path, dots))
        return subs

    def display(self, name, pending):
        if pending:
            chars = b""
            for e in reversed(pending):
                chars += e[1:11] + e[14:26] + e[28:32]
            text = chars.decode("utf-16-le", "replace").split("\x00")[0]
            return text
        base, ext = name[:8].rstrip(b" "), name[8:].rstrip(b" ")
        return (base + (b"." + ext if ext else b"")).decode("latin-1")

    def lfn_ok(self, pending, name, full):
        want = self.lfn_sum(name)
        n = len(pending)
        for k, e in enumerate(pending):
            seq = e[0] & 0x3F
            last = bool(e[0] & 0x40)
            if seq != n - k or last != (k == 0) or e[13] != want:
                self.bad("%s: its long name's pieces are out of order or don't match its short "
                         "entry (piece %d of %d)" % (full, k + 1, n))
                return

    def dot(self, path, name, e, me, parent, i, is_root):
        first = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
        if is_root:
            self.bad("/: a %r entry in the root" % name.strip().decode())
            return
        want = me if name.startswith(b". ") else parent
        if first != want or not e[11] & ATTR_DIR:
            self.bad("%s: its %r entry points to cluster %d, not %d" %
                     (path, name.strip().decode(), first, want))
        if i > 1:
            self.bad("%s: %r is entry %d, not one of the first two" % (path, name.strip().decode(), i))

    def entry(self, full, e, attr):
        """Check a file's or a directory's entry; (clusters, first) of a directory."""
        v = self.v
        first = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
        size = struct.unpack_from("<I", e, 28)[0]
        if attr & 0xC0:
            self.bad("%s: attribute bits 6-7 set (%#x)" % (full, attr))
        if attr & ATTR_DIR:
            if size:
                self.bad("%s: a directory with a size (%d)" % (full, size))
            if not first:
                self.bad("%s: a directory with no cluster" % full)
                return None
            clusters = self.chain(first, full)
            return (clusters, first) if clusters else None
        if not first:
            if size:
                self.bad("%s: %d bytes but no cluster" % (full, size))
            return None
        clusters = self.chain(first, full)
        if clusters is None:
            return None
        want = (size + v.csize - 1) // v.csize
        if len(clusters) != want:
            self.bad("%s: %d bytes need %d cluster(s), its chain has %d"
                     % (full, size, want, len(clusters)))
        return None

    # ---- what nobody reached --------------------------------------------------
    def lost_clusters(self):
        v = self.v
        lost = [c for c in range(2, v.max + 1)
                if v.fat[c] not in (0, BAD) and c not in self.owner]
        if not lost:
            return 0
        lostset = set(lost)
        pointed = {v.fat[c] for c in lost if v.fat[c] in lostset}
        for c in lost:
            if c in pointed:
                continue
            n, x, seen = 0, c, set()
            while x in lostset and x not in seen:
                seen.add(x)
                n += 1
                x = v.fat[x]
            self.lost.append((c, n))
        if not self.lost:   # only loops among the lost: list their smallest clusters
            self.lost.append((lost[0], len(lost)))
        return len(lost)

    def fsinfo(self, free):
        v = self.v
        if not v.fsinfo or v.fsinfo >= v.reserved:
            self.notes.append("no FSInfo sector")
            return
        s = v.read(v.fsinfo * v.bps, 512)
        if s[0:4] != b"RRaA" or s[484:488] != b"rrAa" or s[510:512] != b"\x55\xaa":
            self.notes.append("the FSInfo sector's signatures are wrong")
            return
        said, hint = struct.unpack_from("<II", s, 488)
        if said != 0xFFFFFFFF and said != free:
            self.notes.append("FSInfo says %d free clusters, the FAT %d" % (said, free))

    def run(self):
        v = self.v
        self.fats_agree()
        self.walk()
        nlost = self.lost_clusters()
        free = sum(1 for c in range(2, v.max + 1) if v.fat[c] == 0)
        self.fsinfo(free)
        return nlost, free


def parse_offset(spec):
    spec = spec.strip().upper()
    mult = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}.get(spec[-1:], 1)
    return int(spec.rstrip("KMG")) * mult


def partition_offset(f, n):
    f.seek(0)
    mbr = f.read(512)
    if len(mbr) < 512 or mbr[510:512] != b"\x55\xaa" or not 1 <= n <= 4:
        raise NotFat("no MBR with a partition %d" % n)
    ptype, start = mbr[446 + 16 * (n - 1) + 4], struct.unpack_from("<I", mbr, 446 + 16 * (n - 1) + 8)[0]
    if not ptype or not start:
        raise NotFat("the MBR's partition %d is empty" % n)
    return start * 512


def check(f, base, listmax=20, require_clean=False, quiet=False, out=sys.stdout):
    """Check the volume at byte base of f; the exit status (0, 1, 2 or 3)."""
    try:
        v = Volume(f, base)
    except NotFat as e:
        print("fatcheck: not a FAT32 volume: %s" % e, file=out)
        return 3
    c = Check(v, listmax)
    nlost, free = c.run()
    if require_clean:
        c.damage += [n for n in c.notes if "dirty" in n]
    files = len({p for p in c.owner.values()})
    if not quiet or c.damage or nlost:
        print("fatcheck: FAT32, %d FATs, %d clusters of %d bytes, %d free; %d chains walked"
              % (v.nfats, v.nclusters, v.csize, free, files), file=out)
    for d in c.damage[:listmax]:
        print("fatcheck: DAMAGE: %s" % d, file=out)
    if len(c.damage) > listmax:
        print("fatcheck: ... and %d more" % (len(c.damage) - listmax), file=out)
    if nlost:
        print("fatcheck: LOST: %d cluster(s) allocated but reached by no entry, in %d chain(s)"
              % (nlost, len(c.lost)), file=out)
        for first, n in c.lost[:listmax]:
            print("fatcheck:   lost chain from cluster %d, %d cluster(s)" % (first, n), file=out)
    for n in c.notes:
        print("fatcheck: note: %s" % n, file=out)
    status = 1 if c.damage else 2 if nlost else 0
    print("fatcheck: %s" % ("DAMAGED" if status == 1 else "LOST CLUSTERS" if status == 2 else
                            "clean"), file=out)
    return status


# ---- the self-test: a small FAT32 volume made here, then damaged ------------------

class Maker:
    """A FAT32 volume in memory: 512-byte sectors, one sector a cluster."""

    def __init__(self, clusters=66000, nfats=2):
        self.bps, self.spc, self.reserved, self.nfats = 512, 1, 32, nfats
        self.fatsz = ((clusters + 2) * 4 + 511) // 512
        self.data = self.reserved + nfats * self.fatsz
        self.total = self.data + clusters
        self.max = clusters + 1
        self.img = bytearray(self.total * 512)
        self.fat = [0] * (self.max + 1)
        self.fat[0], self.fat[1] = 0x0FFFFFF8, 0x0FFFFFFF
        self.next = 3
        bs = bytearray(512)
        bs[0:3] = b"\xeb\x58\x90"
        bs[3:11] = b"SELFTEST"
        struct.pack_into("<HBHBHHBHHHII", bs, 11, 512, 1, self.reserved, nfats, 0, 0, 0xF8,
                         0, 63, 255, 0, self.total)
        struct.pack_into("<IHHIHH", bs, 36, self.fatsz, 0, 0, 2, 1, 6)
        bs[66] = 0x29
        bs[71:82] = b"SELFTEST   "
        bs[82:90] = b"FAT32   "
        bs[510:512] = b"\x55\xaa"
        self.img[0:512] = bs
        self.img[6 * 512:7 * 512] = bs
        fsi = bytearray(512)
        fsi[0:4], fsi[484:488], fsi[510:512] = b"RRaA", b"rrAa", b"\x55\xaa"
        struct.pack_into("<II", fsi, 488, 0xFFFFFFFF, 0xFFFFFFFF)
        self.img[512:1024] = fsi
        self.fat[2] = 0x0FFFFFFF   # the root: one cluster
        self.dirs = {2: []}        # first cluster -> its entries

    def alloc(self, n):
        first = self.next
        for k in range(n):
            self.fat[first + k] = first + k + 1 if k < n - 1 else 0x0FFFFFFF
        self.next += n
        return first

    @staticmethod
    def short(name, ext=""):
        return (name.upper().ljust(8) + ext.upper().ljust(3)).encode()

    def add(self, parent, name11, attr, first, size, lfn=None):
        ents = []
        if lfn:
            u = lfn.encode("utf-16-le") + b"\x00\x00"
            u += b"\xff" * (-len(u) % 26)
            pieces = [u[k:k + 26] for k in range(0, len(u), 26)]
            sumv = Check.lfn_sum(name11)
            for k in range(len(pieces), 0, -1):
                p = pieces[k - 1]
                e = bytearray(32)
                e[0] = k | (0x40 if k == len(pieces) else 0)
                e[1:11], e[11], e[13], e[14:26], e[28:32] = p[0:10], ATTR_LFN, sumv, p[10:22], p[22:26]
                ents.append(bytes(e))
        e = bytearray(32)
        e[0:11], e[11] = name11, attr
        struct.pack_into("<H", e, 20, first >> 16)
        struct.pack_into("<HI", e, 26, first & 0xFFFF, size)
        ents.append(bytes(e))
        self.dirs[parent] += ents

    def mkdir(self, parent, name11):
        first = self.alloc(1)
        self.dirs[first] = []
        self.add(first, b".          ", ATTR_DIR, first, 0)
        self.add(first, b"..         ", ATTR_DIR, 0 if parent == 2 else parent, 0)
        self.add(parent, name11, ATTR_DIR, first, 0)
        return first

    def file(self, parent, name11, size, lfn=None):
        first = self.alloc((size + 511) // 512) if size else 0
        self.add(parent, name11, ATTR_ARCHIVE, first, size, lfn)
        return first

    def build(self):
        img = bytearray(self.img)
        for d, ents in self.dirs.items():
            raw = b"".join(ents)
            assert len(raw) <= 512, "the self-test's directories are one cluster"
            off = (self.data + d - 2) * 512
            img[off:off + len(raw)] = raw
        raw = struct.pack("<%dI" % (self.max + 1), *self.fat)
        for i in range(self.nfats):
            off = (self.reserved + i * self.fatsz) * 512
            img[off:off + len(raw)] = raw
        return img


def sample():
    """The clean volume each case starts from; its notable clusters."""
    m = Maker()
    m.add(2, b"SELFTEST   ", ATTR_VOLUME | ATTR_ARCHIVE, 0, 0)
    sub = m.mkdir(2, Maker.short("DIR"))
    big = m.file(2, Maker.short("BIG", "BIN"), 5000, lfn="big file.bin")
    small = m.file(sub, Maker.short("SMALL", "TXT"), 100)
    m.file(sub, Maker.short("EMPTY"), 0)
    deep = m.mkdir(sub, Maker.short("DEEP"))
    m.file(deep, Maker.short("X"), 1024, lfn="A Long Name, With Case.txt")
    return m, {"sub": sub, "big": big, "small": small, "deep": deep}


def fat_off(m, c, which=0):
    return (m.reserved + which * m.fatsz) * 512 + 4 * c


def set_entry(img, m, c, value, fats=None):
    for i in fats if fats is not None else range(m.nfats):
        struct.pack_into("<I", img, fat_off(m, c, i), value)


def dir_entry_off(m, d, name11):
    off = (m.data + d - 2) * 512
    for k in range(16):
        if bytes(m.build()[off + 32 * k:off + 32 * k + 11]) == name11:
            return off + 32 * k
    raise KeyError(name11)


def selftest(verbose):
    m, at = sample()
    big, small, sub = at["big"], at["small"], at["sub"]
    cases = []

    def case(name, want, said, fn, require_clean=False):
        img = m.build()
        fn(img)
        cases.append((name, want, said, img, require_clean))

    off_big = dir_entry_off(m, 2, Maker.short("BIG", "BIN"))
    off_small = dir_entry_off(m, sub, Maker.short("SMALL", "TXT"))
    off_dir = dir_entry_off(m, 2, Maker.short("DIR"))
    off_dot = (m.data + sub - 2) * 512

    def dirty(img):
        set_entry(img, m, 1, 0x07FFFFFF)

    case("clean", 0, "fatcheck: clean", lambda img: None)
    case("FATs disagree", 1, "FAT 2 differs from FAT 1 in 1 entries",
         lambda img: set_entry(img, m, big + 2, 0x0FFFFFFF, fats=[1]))
    case("cross-linked", 1, "are cross-linked at cluster %d" % (big + 3),
         lambda img: set_entry(img, m, small, big + 3))
    case("a loop", 1, "loops back to cluster %d" % (big + 2),
         lambda img: set_entry(img, m, big + 9, big + 2))
    case("chain to a free cluster", 1, "which is free",
         lambda img: set_entry(img, m, big + 4, m.next + 50))
    case("chain out of range", 1, "out of range",
         lambda img: set_entry(img, m, big + 4, m.max + 7))
    case("chain to a bad cluster", 1, "marked bad", lambda img: set_entry(img, m, big + 4, BAD))
    case("size longer than the chain", 1, "9000 bytes need 18 cluster(s), its chain has 10",
         lambda img: struct.pack_into("<I", img, off_big + 28, 9000))
    case("size shorter than the chain", 1, "512 bytes need 1 cluster(s), its chain has 10",
         lambda img: struct.pack_into("<I", img, off_big + 28, 512))
    case("an empty file with a chain", 1, "0 bytes need 0 cluster(s), its chain has 1",
         lambda img: struct.pack_into("<I", img, off_small + 28, 0))
    case("a bad short name", 1, "a character a FAT name can't",
         lambda img: img.__setitem__(slice(off_small, off_small + 1), b"s"))
    case("long name out of step", 1, "long name's pieces",
         lambda img: img.__setitem__(slice(off_big - 32 + 13, off_big - 32 + 14), b"\x00"))
    case("a directory with a size", 1, "a directory with a size",
         lambda img: struct.pack_into("<I", img, off_dir + 28, 512))
    case("'.' pointing elsewhere", 1, "its '.' entry points to cluster %d" % big,
         lambda img: struct.pack_into("<H", img, off_dot + 26, big))
    case("a lost cluster", 2, "1 cluster(s) allocated but reached by no entry",
         lambda img: set_entry(img, m, m.next + 10, 0x0FFFFFFF))
    case("a lost chain of three", 2, "lost chain from cluster %d, 3 cluster(s)" % (m.next + 20),
         lambda img: (set_entry(img, m, m.next + 20, m.next + 21),
                      set_entry(img, m, m.next + 21, m.next + 22),
                      set_entry(img, m, m.next + 22, 0x0FFFFFFF)))
    case("lost and damaged: damage wins", 1, "LOST:",
         lambda img: (set_entry(img, m, m.next + 10, 0x0FFFFFFF),
                      struct.pack_into("<I", img, off_dir + 28, 512)))
    case("dirty, as a note", 0, "note: the volume is marked dirty", dirty)
    case("dirty, --require-clean", 1, "DAMAGE: the volume is marked dirty", dirty,
         require_clean=True)
    case("not FAT32", 3, "no boot sector signature",
         lambda img: img.__setitem__(slice(510, 512), b"\x00\x00"))

    ok = True
    for name, want, said, img, rc in cases:
        out = io.StringIO()
        got = check(io.BytesIO(bytes(img)), 0, require_clean=rc, out=out)
        if got != want or said not in out.getvalue():
            ok = False
            print("fatcheck --selftest: %s: exit %d (wanted %d), wanted it to say %r:\n%s"
                  % (name, got, want, said, out.getvalue()))
            continue
        what = [l for l in out.getvalue().splitlines() if said in l]
        if verbose:
            print("fatcheck --selftest: %-32s exit %d: %s" % (name, got, what[0][10:]))
    # Through a partition table, as the Jam OS stick holds /data.
    img = bytearray(1 << 20) + m.build()
    mbr = bytearray(512)
    struct.pack_into("<B3sB3sII", mbr, 446 + 16, 0, b"\xfe\xff\xff", 0x0C, b"\xfe\xff\xff",
                     2048, m.total)
    mbr[510:512] = b"\x55\xaa"
    img[0:512] = mbr
    f = io.BytesIO(bytes(img))
    if check(f, partition_offset(f, 2), quiet=True, out=io.StringIO()) != 0:
        ok = False
        print("fatcheck --selftest: through partition 2: not clean")
    print("fatcheck --selftest: %s (%d cases)" % ("PASS" if ok else "FAIL", len(cases) + 1))
    return 0 if ok else 1


def main(argv):
    if argv[1:2] == ["--selftest"]:
        return selftest("-v" in argv[2:])
    args, part, listmax, require_clean, quiet = [], None, 20, False, False
    it = iter(argv[1:])
    for a in it:
        if a == "-p":
            part = int(next(it))
        elif a == "--list":
            listmax = int(next(it))
        elif a == "--require-clean":
            require_clean = True
        elif a == "-q":
            quiet = True
        else:
            args.append(a)
    if len(args) != 1:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 3
    path, _, off = args[0].partition("@@")
    try:
        with open(path, "rb") as f:
            base = partition_offset(f, part) if part else parse_offset(off) if off else 0
            return check(f, base, listmax, require_clean, quiet)
    except (OSError, NotFat, ValueError) as e:
        print("fatcheck: %s: %s" % (args[0], e), file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main(sys.argv))
