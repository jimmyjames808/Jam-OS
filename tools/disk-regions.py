#!/usr/bin/env python3
"""Hash a disk's regions, read-only: Linux's half of "Windows' drive untouched".

    sudo python3 disk-regions.py /dev/nvme0n1 > regions-linux-1.txt
    python3 disk-regions.py --image disk.img          # a file (the tests)

Prints the region list of docs/M12.7-PLAN.md ("The region list"), made from
the partition table alone, which Jam OS's `disk digest` prints the same way
(user/services/volumes via drv/nvme's storage.digest): diffing the two
outputs proves Jam OS read every region right. Taken before and after a Jam
OS session, it also proves the regions did not change.

The list, in this order, one line each:
    gpt-head         the first 34 blocks (protective MBR, primary GPT)
    for each used partition, in table order (at most 16; N counts the used
    ones from 1, not the entry number):
      partN          the partition whole, if it is under 1 GiB
      partN-head     else its first 64 MiB,
      partN-tail     its last 64 MiB,
      partN-sNN      and 32 samples (NN = 00..31) of 1 MiB at
                     start + NN * (blocks_in_partition // 32)
    gpt-tail         the last 33 blocks (backup entries, backup GPT)
A line is `<name> lba <first> count <blocks> sha256 <hex>`. Sizes in bytes
(1 GiB, 64 MiB, 1 MiB) are divided by the block size; the last two lines
and every name are as above whatever the block size. A partition is "used"
as drv/nvme's parttable library says: a GPT entry whose type GUID isn't
zero, inside the header's usable range and not overlapping an earlier used
one (others are left out and reported in a `#` line); on a disk with no
GPT, the MBR's four entries (non-zero type, inside the disk, no overlap).
The GPT is the primary one if its header and entry array are good, else the
backup. `#` lines are comments (the device, its geometry, what was left out).

`--stable` prints only the regions Windows doesn't change while it runs
(gpt-head, gpt-tail, the Microsoft reserved and Windows recovery
partitions), as tools/disk-regions.ps1 does by default.

Read-only by construction: the device is opened O_RDONLY (this program has
no write call at all) and a path that is not a block device is refused
unless --image says it is a file. Run it on the owner's Linux live stick,
never from a Jam OS session or an agent."""
import argparse
import binascii
import hashlib
import os
import stat
import struct
import sys
import uuid

MiB = 1 << 20
GiB = 1 << 30
GUID_MSR = "e3c9e316-0b5c-4db8-817d-f92df00215ae"
GUID_RECOVERY = "de94bba4-06d1-4d40-a16a-bfd50179d6ac"
MAX_PARTS = 16


def crc32(b):
    return binascii.crc32(b) & 0xFFFFFFFF


class Disk:
    def __init__(self, path, image, block_size):
        st = os.stat(path)
        if not image and not stat.S_ISBLK(st.st_mode):
            sys.exit("disk-regions: %s is not a block device (--image for a file)" % path)
        self.fd = os.open(path, os.O_RDONLY)
        self.size = os.lseek(self.fd, 0, os.SEEK_END)
        self.bs = block_size or self.sector_size(image)
        self.blocks = self.size // self.bs

    def sector_size(self, image):
        if not image:
            try:
                import fcntl
                buf = fcntl.ioctl(self.fd, 0x1268, b"\0" * 4)   # BLKSSZGET
                return struct.unpack("I", buf)[0]
            except (ImportError, OSError):
                sys.exit("disk-regions: can't read the sector size: give --block-size")
        return 512

    def read(self, lba, count):
        want, off, out = count * self.bs, lba * self.bs, []
        while want:
            chunk = os.pread(self.fd, min(want, 4 * MiB), off)
            if not chunk:
                sys.exit("disk-regions: short read at lba %d" % lba)
            out.append(chunk)
            want -= len(chunk)
            off += len(chunk)
        return b"".join(out)

    def sha(self, lba, count):
        h = hashlib.sha256()
        done = 0
        while done < count:
            n = min(count - done, 4 * MiB // self.bs or 1)
            h.update(self.read(lba + done, n))
            done += n
        return h.hexdigest()


def gpt_header(d, lba, my, alt):
    """The header at lba if it is good (UEFI 2.10 5.3.2), else None."""
    raw = d.read(lba, 1)
    h = raw[:92]
    if len(h) < 92 or h[:8] != b"EFI PART":
        return None
    rev, size, crc, rsvd = struct.unpack_from("<IIII", h, 8)
    if rev != 0x00010000 or size < 92 or size > d.bs or rsvd != 0:
        return None
    hb = bytearray(raw[:size])
    struct.pack_into("<I", hb, 16, 0)
    if crc32(bytes(hb)) != crc:
        return None
    my_lba, alt_lba, first, last = struct.unpack_from("<QQQQ", h, 24)
    ent_lba, n, esize, ecrc = struct.unpack_from("<QIII", h, 72)
    if my_lba != my or alt_lba != alt or first > last or last >= d.blocks:
        return None
    if esize < 128 or esize & (esize - 1) or not (128 <= n <= 1024) or n * esize > 32768:
        return None
    ent_blocks = (n * esize + d.bs - 1) // d.bs
    if ent_lba + ent_blocks > d.blocks:
        return None
    arr = d.read(ent_lba, ent_blocks)[:n * esize]
    if crc32(arr) != ecrc:
        return None
    return dict(first=first, last=last, n=n, esize=esize, arr=arr)


def gpt_parts(d, notes):
    if d.blocks < 3 or d.read(0, 1)[510:512] != b"\x55\xaa":
        return None
    prim = gpt_header(d, 1, 1, d.blocks - 1)
    back = gpt_header(d, d.blocks - 1, d.blocks - 1, 1)
    notes.append("GPT primary header %s, backup %s" %
                 ("good" if prim else "bad", "good" if back else "bad"))
    h = prim or back
    if not h:
        return None
    parts, used = [], []
    for i in range(h["n"]):
        e = h["arr"][i * h["esize"]:i * h["esize"] + 128]
        if e[:16] == b"\0" * 16:
            continue
        first, last = struct.unpack_from("<QQ", e, 32)
        guid = str(uuid.UUID(bytes_le=e[:16]))
        if not (h["first"] <= first <= last <= h["last"]):
            notes.append("entry %d (%s) outside the usable range: left out" % (i + 1, guid))
            continue
        if any(first <= b and a <= last for a, b in used):
            notes.append("entry %d (%s) overlaps an earlier one: left out" % (i + 1, guid))
            continue
        used.append((first, last))
        parts.append((i + 1, guid, first, last - first + 1))
    return parts


def mbr_parts(d, notes):
    mbr = d.read(0, 1)
    if mbr[510:512] != b"\x55\xaa":
        notes.append("no partition table")
        return []
    parts, used = [], []
    for i in range(4):
        e = mbr[446 + 16 * i:462 + 16 * i]
        typ, start, count = e[4], struct.unpack_from("<I", e, 8)[0], struct.unpack_from("<I", e, 12)[0]
        if typ == 0 or count == 0:
            continue
        if start + count > d.blocks or any(start < b and a < start + count for a, b in used):
            notes.append("MBR entry %d outside the disk or overlapping: left out" % (i + 1))
            continue
        used.append((start, start + count))
        parts.append((i + 1, "mbr-%02x" % typ, start, count))
    return parts


def regions(d, stable):
    notes = []
    parts = gpt_parts(d, notes)
    if parts is None:
        notes.append("no good GPT: MBR")
        parts = mbr_parts(d, notes)
    out = []
    if d.blocks >= 34:
        out.append(("gpt-head", 0, 34, True))
    big = GiB // d.bs
    for idx, (n, guid, start, count) in enumerate(parts[:MAX_PARTS]):
        is_stable = guid in (GUID_MSR, GUID_RECOVERY)
        name = "part%d" % (idx + 1)
        if count < big:
            out.append((name, start, count, is_stable))
            continue
        s64, s1 = 64 * MiB // d.bs, MiB // d.bs
        out.append((name + "-head", start, s64, False))
        out.append((name + "-tail", start + count - s64, s64, False))
        for k in range(32):
            out.append(("%s-s%02d" % (name, k), start + k * (count // 32), s1, False))
    if len(parts) > MAX_PARTS:
        notes.append("%d more partition(s) not listed" % (len(parts) - MAX_PARTS))
    if d.blocks >= 34:
        out.append(("gpt-tail", d.blocks - 33, 33, True))
    if stable:
        out = [r for r in out if r[3]]
    return notes, out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("device")
    ap.add_argument("--image", action="store_true", help="the path is a file, not a block device")
    ap.add_argument("--block-size", type=int, help="the logical block size (default: the device's)")
    ap.add_argument("--stable", action="store_true",
                    help="only the regions Windows doesn't change while it runs")
    a = ap.parse_args()
    d = Disk(a.device, a.image, a.block_size)
    notes, regs = regions(d, a.stable)
    print("# disk-regions: %s block_size %d blocks %d" % (a.device, d.bs, d.blocks))
    for n in notes:
        print("# " + n)
    for name, lba, count, _ in regs:
        print("%s lba %d count %d sha256 %s" % (name, lba, count, d.sha(lba, count)))
    print("# disk-regions: %d regions" % len(regs))


main()
