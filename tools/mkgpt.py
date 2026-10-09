#!/usr/bin/env python3
"""Make disk images with a GPT (or an MBR) for the partition table
library's tests: tools/gptcheck.c on the Mac, and QEMU's NVMe drives.

    mkgpt.py <out.img> [options]      one image
    mkgpt.py --cases <dir>            every case gptcheck knows, into <dir>
    mkgpt.py --list                   the cases' names

One image's options:
  --layout L        windows (the PC's drive: protective MBR, ESP 100 MiB,
                    MSR 16 MiB, basic data, recovery 750 MiB at the end),
                    jamos (Jam OS's mark: "Jam OS ESP" of 512 MiB at 1 MiB,
                    "Jam OS data" to the end, 1 MiB aligned), empty (a GPT
                    with no partitions), mbr (an MBR, two FAT32
                    partitions), blank (zeros), none (only --part's)
  --size N          the disk's size in bytes (K, M, G suffixes; default 2G
                    for windows and jamos, 64M otherwise)
  --block-size N    512 (default) or 4096
  --entries N       GPT entries (default 128), --entry-size N (default 128)
  --part T:S:N[:NAME[:ATTRS]]  one more GPT entry (MBR entry with
                    --layout mbr): type (esp, msr, basic, recovery, jamos,
                    a GUID; a hex byte for MBR), first block, blocks, name
  --fill K=FILE     copy FILE into partition K (1-based) at its first
                    block (a FAT image for QEMU, made with mtools)
  --hybrid          an MBR entry (type 0C) beside the protective EEh
  --mbr KIND        block 0: pmbr (default), pmbr-status (its status byte
                    7Fh), fat32 (an MBR listing one FAT32 partition and no
                    EEh: GPT without a protective MBR), zero
  --break WHAT      damage after writing (repeatable): primary-crc,
                    backup-crc, primary-array, backup-array, disagree (the
                    backup's disk GUID differs, its CRCs right), primary-zero,
                    backup-zero
  --grow N          after writing, grow the image by N bytes (the backup
                    is then no longer at the last block)
  --poke OFF        one nonzero byte at byte offset OFF (negative: from the
                    end), for "almost blank"

Images are sparse files (only the tables and --fill's data are written),
and the same every time: GUIDs come from the image's options, not from a
random source.

With --cases, each image <dir>/<case>.img gets <dir>/<case>.expect: the
partitions the library must list, one line each, computed here with
Python's own reading of the rules (names to UTF-8 included), so gptcheck
checks the C against a second implementation:
    part <entry> <start> <blocks> <mbr_type> <type-guid> <unique-guid> <attrs> <name-hex|->
"""
import os
import struct
import sys
import uuid
import zlib

TYPES = {
    "esp": "C12A7328-F81F-11D2-BA4B-00A0C93EC93B",
    "msr": "E3C9E316-0B5C-4DB8-817D-F92DF00215AE",
    "basic": "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7",
    "recovery": "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC",
    "jamos": "71BAE565-D4A8-4315-8C81-9B35462CC158",
}
MIB = 1 << 20


def size_arg(s):
    mult = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}.get(s[-1:].upper())
    return int(s[:-1]) * mult if mult else int(s, 0)


def guid_bytes(g):
    return uuid.UUID(g).bytes_le


def made_guid(*parts):
    """A GUID that depends only on its parts: reproducible images."""
    return uuid.uuid5(uuid.NAMESPACE_URL, "jamos-mkgpt:" + ":".join(map(str, parts))).bytes_le


def name_units(name):
    """The name as UTF-16 code units; lone surrogates pass through."""
    raw = name.encode("utf-16-le", "surrogatepass")
    return list(struct.unpack("<%dH" % (len(raw) // 2), raw))


def expected_name(units):
    """The library's rule: up to the first zero unit; a surrogate pair is
    one character; an unpaired surrogate or a control character is '?'."""
    out, i = [], 0
    units = units[:36]
    while i < len(units) and units[i]:
        c = units[i]
        if 0xD800 <= c < 0xDC00 and i + 1 < len(units) and 0xDC00 <= units[i + 1] < 0xE000:
            c = 0x10000 + ((c - 0xD800) << 10) + (units[i + 1] - 0xDC00)
            i += 1
        if 0xD800 <= c < 0xE000 or c < 0x20 or c == 0x7F:
            c = ord("?")
        out.append(chr(c))
        i += 1
    return "".join(out).encode("utf-8")


class Disk:
    """A sparse image: blocks written by offset, everything else zeros."""

    def __init__(self, size, bs):
        assert size % bs == 0, "the size must be whole blocks"
        self.size, self.bs, self.blocks = size, bs, size // bs
        self.writes = {}   # byte offset -> bytes

    def put(self, off, data):
        if off < 0:
            off += self.size
        assert 0 <= off and off + len(data) <= self.size, "a write outside the image"
        self.writes[off] = bytes(data)

    def save(self, path, grow=0):
        with open(path, "wb") as f:
            f.truncate(self.size + grow)
            for off in sorted(self.writes):
                f.seek(off)
                f.write(self.writes[off])


def mbr_entry(status, ptype, start, count):
    # status, CHS start (unused), type, CHS end (unused), LBA start, count
    return struct.pack("<B3sB3sII", status, b"\xfe\xff\xff", ptype, b"\xfe\xff\xff",
                       start, count)


def mbr_block(entries, disk_id=0):
    b = bytearray(512)
    struct.pack_into("<I", b, 440, disk_id)
    for i, e in enumerate(entries):
        b[446 + 16 * i:462 + 16 * i] = mbr_entry(*e)
    b[510:512] = b"\x55\xaa"
    return b


def gpt_header(bs, my, alt, first, last, guid, array_lba, count, size, array_crc):
    h = bytearray(struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x00010000, 92, 0, 0,
                              my, alt, first, last, guid, array_lba, count, size, array_crc))
    struct.pack_into("<I", h, 16, zlib.crc32(h))
    return h + bytes(bs - len(h))


def gpt_entry(e, size):
    """An entry from (type GUID bytes, unique GUID bytes, start, blocks, attrs, units)."""
    tg, ug, start, blocks, attrs, units = e
    units = (units + [0] * 36)[:36]
    raw = struct.pack("<16s16sQQQ36H", tg, ug, start, start + blocks - 1, attrs, *units)
    return raw + bytes(size - len(raw))


class Gpt:
    """A GPT about to be written: the entries, then both copies."""

    def __init__(self, disk, count=128, size=128, guid=None):
        self.d, self.count, self.size = disk, count, size
        self.guid = guid or made_guid("disk", disk.size, disk.bs, count, size)
        self.array_blocks = -(-count * size // disk.bs)
        self.first = 2 + self.array_blocks
        self.last = disk.blocks - 2 - self.array_blocks
        self.entries = {}   # index -> entry tuple

    def add(self, index, ptype, start, blocks, name="", attrs=0):
        tg = guid_bytes(TYPES.get(ptype, ptype))
        units = name if isinstance(name, list) else name_units(name)
        self.entries[index] = (tg, made_guid("part", index, start, blocks), start, blocks,
                               attrs, units)

    def array(self):
        a = bytearray(self.count * self.size)
        for i, e in self.entries.items():
            a[i * self.size:(i + 1) * self.size] = gpt_entry(e, self.size)
        return a

    def write(self, breaks=()):
        d, bs = self.d, self.d.bs
        a = self.array()
        crc = zlib.crc32(a)
        end = d.blocks - 1
        backup_array = end - self.array_blocks
        p = gpt_header(bs, 1, end, self.first, self.last, self.guid, 2, self.count, self.size, crc)
        bguid = made_guid("other disk") if "disagree" in breaks else self.guid
        b = gpt_header(bs, end, 1, self.first, self.last, bguid, backup_array, self.count,
                       self.size, crc)
        pa, ba = bytearray(a), bytearray(a)
        for which, hdr, arr in (("primary", p, pa), ("backup", b, ba)):
            if which + "-crc" in breaks:
                hdr[16] ^= 0x01
            if which + "-array" in breaks:
                arr[56] ^= 0x01   # the first entry's name: the CRC no longer matches
            if which + "-zero" in breaks:
                hdr[:] = bytes(bs)
        d.put(bs, p)
        d.put(2 * bs, pa)
        d.put(backup_array * bs, ba)
        d.put(end * bs, b)

    def expect(self):
        """The partitions the library lists, by the rules: used entries in
        the usable range, not overlapping an earlier listed one, 16 at most."""
        out, listed = [], []
        for i in sorted(self.entries):
            tg, ug, start, blocks, attrs, units = self.entries[i]
            end = start + blocks - 1
            if not (self.first <= start <= end <= self.last):
                continue
            if any(start <= le and ls <= end for ls, le in listed) or len(listed) == 16:
                continue
            listed.append((start, end))
            name = expected_name(units)
            out.append(expect_line(i, start, blocks, 0, tg, ug, attrs, name))
        return out


def expect_line(entry, start, blocks, mbr_type, tg, ug, attrs, name):
    guid = lambda b: str(uuid.UUID(bytes_le=b)).upper()
    return "part %d %d %d %d %s %s %d %s" % (entry, start, blocks, mbr_type, guid(tg), guid(ug),
                                            attrs, name.hex() if name else "-")


def mbr_expect(ents, blocks):
    """An MBR's listing by usb-storage's rules: entries in use, inside the
    disk, past block 0, not overlapping an earlier listed one."""
    out, listed = [], []
    for i, (status, ptype, start, count) in enumerate(ents):
        if not ptype or not count:
            continue
        if not start or start >= blocks or count > blocks - start:
            continue
        if any(start < le and ls < start + count for ls, le in listed):
            continue
        listed.append((start, start + count))
        out.append(expect_line(i, start, count, ptype, bytes(16), bytes(16), 0, b""))
    return out


def mib_blocks(d, mib):
    return mib * MIB // d.bs


def layout_windows(g):
    d = g.d
    esp, msr = mib_blocks(d, 100), mib_blocks(d, 16)
    rec = mib_blocks(d, 750)
    s = mib_blocks(d, 1)
    g.add(0, "esp", s, esp, "EFI system partition")
    g.add(1, "msr", s + esp, msr, "Microsoft reserved partition")
    data_start = s + esp + msr
    rec_start = g.last + 1 - rec
    g.add(2, "basic", data_start, rec_start - data_start, "Basic data partition")
    g.add(3, "recovery", rec_start, rec, "", 0x8000000000000001)


def layout_jamos(g):
    d = g.d
    s, esp = mib_blocks(d, 1), mib_blocks(d, 512)
    g.add(0, "esp", s, esp, "Jam OS ESP")
    data_end = (g.last + 1) // mib_blocks(d, 1) * mib_blocks(d, 1)
    g.add(1, "jamos", s + esp, data_end - s - esp, "Jam OS data")


def make(out, layout="windows", size=None, bs=512, entries=128, entry_size=128, parts=(),
         fills=(), hybrid=False, mbr="pmbr", breaks=(), grow=0, poke=None, raw=None,
         listed=True):
    """Write one image; return the .expect lines. listed=False: the case's
    table is unreadable (both copies damaged, or the backup gone), so
    nothing is listed."""
    size = size or (2 << 30 if layout in ("windows", "jamos") else 64 << 20)
    d = Disk(size, bs)
    expect = []
    if layout == "mbr":
        ents = [(0, int(t, 16), s, n) for t, s, n, *_ in parts] or \
               [(0x80, 0x0C, 2048, d.blocks // 2 - 2048), (0, 0x0C, d.blocks // 2, d.blocks // 2)]
        d.put(0, mbr_block(ents, 0x4A414D21))
        expect = mbr_expect(ents, d.blocks)
    elif layout != "blank":
        g = Gpt(d, entries, entry_size)
        {"windows": layout_windows, "jamos": layout_jamos}.get(layout, lambda g: None)(g)
        for i, (t, s, n, *rest) in enumerate(parts):
            idx = max(list(g.entries) + [-1]) + 1
            g.add(idx, t, s, n, rest[0] if rest else "", int(rest[1], 0) if len(rest) > 1 else 0)
        if raw:
            raw(g)
        ents = [(0, 0xEE, 1, min(d.blocks - 1, 0xFFFFFFFF))]
        if hybrid:
            ents.append((0, 0x0C, mib_blocks(d, 1), mib_blocks(d, 100)))
        if mbr == "pmbr":
            d.put(0, mbr_block(ents))
        elif mbr == "pmbr-status":   # a protective MBR with a junk status byte
            d.put(0, mbr_block([(0x7F,) + ents[0][1:]]))
        g.write(breaks)
        expect = g.expect() if listed and mbr.startswith("pmbr") else []
        if mbr == "fat32":
            ents = [(0, 0x0C, mib_blocks(d, 1), mib_blocks(d, 100))]
            d.put(0, mbr_block(ents))
            expect = mbr_expect(ents, d.blocks)
    for k, path in fills:
        tg, ug, start, blocks, attrs, units = g.entries[k - 1]
        data = open(path, "rb").read()
        assert len(data) <= blocks * bs, "--fill: the file is bigger than the partition"
        d.put(start * bs, data)
    if poke is not None:
        d.put(poke, b"\x01")
    d.save(out, grow)
    return expect


# ---- the cases gptcheck checks (its own table says what each must give) ----

def odd_names(g):
    """Names that test the UTF-16 rules: lone surrogates, a pair, a control
    character, non-ASCII, and the longest names in 2- and 3-byte UTF-8."""
    s = mib_blocks(g.d, 1)
    names = [
        [0x41, 0xD800, 0x42],                 # a lone high surrogate
        [0xDC00, 0x43],                       # a lone low one
        [0xD83D, 0xDE00, 0x44],               # U+1F600, a pair
        [0x45, 0x0A, 0x46, 0x7F],             # a newline and DEL
        name_units("Ünïcødé"),
        [0xE9] * 36,                          # 72 bytes of UTF-8
        [0x20AC] * 36,                        # 108 bytes: the limit
        [0x41] * 35 + [0xD800],               # a high surrogate in the last unit
        [0xD83D, 0xDE00] * 18,                # 18 pairs: 72 bytes
        [0x47, 0, 0x48],                      # stops at the zero
        [0xD800, 0xE000, 0xFFFF],             # a high surrogate before a non-surrogate
    ]
    for i, n in enumerate(names):
        g.add(i, "basic", s + i * 2048, 2048, n)


def outside(g):
    s, f, l = mib_blocks(g.d, 1), g.first, g.last
    g.add(0, "basic", s, 2048, "fine")
    g.add(1, "basic", f - 1, 10, "before first usable")
    g.add(2, "basic", l - 5, 10, "past last usable")
    g.add(3, "basic", 0, 10, "at block 0")
    g.add(4, "basic", l, 1, "the last usable block")
    g.add(5, "basic", f, 1, "the first usable block")
    # start > end: written by hand below (add() makes end = start + blocks - 1)
    g.entries[6] = g.entries[0][:2] + (s + 4096, 0, 0, name_units("start past end"))


def overlap(g):
    s = mib_blocks(g.d, 1)
    g.add(0, "basic", s, 4096, "first")
    g.add(1, "basic", s + 4095, 10, "overlaps the first")
    g.add(2, "basic", s + 4096, 10, "right after it")
    g.add(3, "basic", s - 10, 11, "overlaps the first's start")
    g.add(5, "basic", s + 2048, 1, "inside the first")


def many(g):
    s = mib_blocks(g.d, 1)
    for i in range(20):
        g.add(i * 3, "basic", s + i * 100, 100, "p%d" % i)


def entries_edge(g):
    s = mib_blocks(g.d, 1)
    g.add(0, "basic", s, 100, "first entry")
    g.add(g.count - 1, "basic", s + 100, 100, "last entry")


def jamos_extra(g):
    g.add(2, "basic", g.last - 100, 50, "extra")


def jamos_name(g):
    tg, ug, s, n, a, _ = g.entries[1]
    g.entries[1] = (tg, ug, s, n, a, name_units("Jam OS Data"))


CASES = {
    "windows": dict(layout="windows"),
    "windows-4k": dict(layout="windows", bs=4096),
    "primary-crc": dict(layout="windows", breaks=("primary-crc",)),
    "backup-crc": dict(layout="windows", breaks=("backup-crc",)),
    "primary-array": dict(layout="windows", breaks=("primary-array",)),
    "backup-array": dict(layout="windows", breaks=("backup-array",)),
    "both-arrays": dict(layout="windows", breaks=("primary-array", "backup-array"), listed=False),
    "both-crc": dict(layout="windows", breaks=("primary-crc", "backup-crc"), listed=False),
    "disagree": dict(layout="windows", breaks=("disagree",)),
    "primary-zero": dict(layout="windows", breaks=("primary-zero",)),
    "outside": dict(layout="none", raw=outside),
    "overlap": dict(layout="none", raw=overlap),
    "entries-129": dict(layout="none", entries=129, raw=entries_edge),
    "entries-1024": dict(layout="none", entries=1024, raw=entries_edge),
    "entries-127": dict(layout="none", entries=127, raw=entries_edge, listed=False),
    "entry-size-256": dict(layout="none", entry_size=256, raw=entries_edge),
    "entry-size-1024": dict(layout="none", entry_size=1024, raw=entries_edge),
    "entries-1024-4k": dict(layout="none", bs=4096, entries=1024, raw=entries_edge),
    "too-many": dict(layout="none", raw=many),
    "hybrid": dict(layout="windows", hybrid=True),
    "no-pmbr": dict(layout="windows", mbr="fat32"),
    "zero-mbr": dict(layout="windows", mbr="zero"),
    "odd-names": dict(layout="none", raw=odd_names),
    "empty": dict(layout="empty"),
    "blank": dict(layout="blank"),
    "blank-4k": dict(layout="blank", bs=4096),
    "almost-blank-head": dict(layout="blank", poke=20 * 512),
    "almost-blank-tail": dict(layout="blank", poke=-5 * 512),
    "almost-blank-edge": dict(layout="blank", poke=64 * 1024 - 1),
    "jamos": dict(layout="jamos"),
    "jamos-4k": dict(layout="jamos", bs=4096),
    "jamos-backup-crc": dict(layout="jamos", breaks=("backup-crc",)),
    "jamos-extra": dict(layout="jamos", raw=jamos_extra),
    "jamos-hybrid": dict(layout="jamos", hybrid=True),
    "jamos-name": dict(layout="jamos", raw=jamos_name),
    "jamos-cut-mbr": dict(layout="jamos", mbr="zero"),
    "jamos-cut-primary": dict(layout="jamos", mbr="zero", breaks=("primary-zero",)),
    "jamos-cut-bad": dict(layout="jamos", mbr="zero", breaks=("primary-crc",)),
    "jamos-cut-backup-bad": dict(layout="jamos", mbr="zero", breaks=("backup-crc",)),
    "pmbr-status": dict(layout="windows", mbr="pmbr-status"),
    "mbr": dict(layout="mbr"),
    "mbr-bad": dict(layout="mbr", size=64 << 20, parts=[
        ("0c", 2048, 4096), ("0c", 0, 100), ("0c", 4000, 100), ("07", 100000, 200000)]),
    "grown": dict(layout="windows", grow=1 << 20, listed=False),
}


def superfloppy(path, junk=False):
    """No table: a FAT32 boot sector at block 0, the disk served whole.
    junk: boot code's message where the MBR's entries would be (their type
    and count bytes nonzero, their status bytes text), one type byte EEh."""
    d = Disk(64 << 20, 512)
    b = bytearray(512)
    if junk:
        b[446:510] = b"Non-system disk or disk error. Replace and press any key when ready"[:64]
        b[446 + 16 + 4] = 0xEE
    b[0:3] = b"\xeb\x58\x90"
    struct.pack_into("<HBHB", b, 11, 512, 8, 32, 2)      # bytes/sector, cluster, reserved, FATs
    struct.pack_into("<I", b, 32, d.blocks)              # total sectors
    b[82:90] = b"FAT32   "
    b[510:512] = b"\x55\xaa"
    d.put(0, b)
    d.save(path)


def main():
    args = sys.argv[1:]
    if args[:1] == ["--list"]:
        print("\n".join(list(CASES) + ["superfloppy", "superfloppy-junk"]))
        return
    if args[:1] == ["--cases"] and len(args) == 2:
        os.makedirs(args[1], exist_ok=True)
        for name, kw in CASES.items():
            path = os.path.join(args[1], name + ".img")
            exp = make(path, **kw)
            with open(os.path.join(args[1], name + ".expect"), "w") as f:
                f.write("".join(line + "\n" for line in exp or []))
        for name, junk in (("superfloppy", False), ("superfloppy-junk", True)):
            superfloppy(os.path.join(args[1], name + ".img"), junk)
            with open(os.path.join(args[1], name + ".expect"), "w") as f:
                f.write(expect_line(0, 0, (64 << 20) // 512, 0, bytes(16), bytes(16), 0, b"") +
                        "\n")
        return
    if not args or args[0].startswith("-"):
        sys.exit(__doc__)
    out, kw, parts, fills, breaks = args[0], {}, [], [], []
    it = iter(args[1:])
    for a in it:
        if a in ("--hybrid",):
            kw["hybrid"] = True
            continue
        v = next(it, None)
        if v is None:
            sys.exit("mkgpt: %s needs a value" % a)
        if a == "--layout":
            kw["layout"] = v
        elif a == "--size":
            kw["size"] = size_arg(v)
        elif a == "--block-size":
            kw["bs"] = int(v)
        elif a == "--entries":
            kw["entries"] = int(v)
        elif a == "--entry-size":
            kw["entry_size"] = int(v)
        elif a == "--part":
            t, s, n, *rest = v.split(":")
            parts.append((t, int(s, 0), int(n, 0), *rest))
        elif a == "--fill":
            k, _, path = v.partition("=")
            fills.append((int(k), path))
        elif a == "--mbr":
            kw["mbr"] = v
        elif a == "--break":
            breaks.append(v)
        elif a == "--grow":
            kw["grow"] = size_arg(v)
        elif a == "--poke":
            kw["poke"] = int(v, 0)
        else:
            sys.exit("mkgpt: unknown option %s" % a)
    make(out, parts=parts, fills=fills, breaks=tuple(breaks), **kw)


if __name__ == "__main__":
    main()
