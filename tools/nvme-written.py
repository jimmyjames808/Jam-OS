#!/usr/bin/env python3
"""Check a Jam OS drive's image after a stage-2 QEMU run (tools/nvme-test.sh's
`write` case): what drv/nvme wrote there, and nothing else.

    nvme-written.py <image> [--pattern none|some|all] [--ranges] [--before <image>]

  1. Its table is Jam OS's, as the plan has it (docs/M12.7-PLAN.md, "Stage
     2"): a protective MBR (one entry, type EEh), both GPT headers good
     (signature, revision, CRC, MyLBA and AlternateLBA) and agreeing, one
     entry array of 128 entries with its CRC, exactly two used entries:
     "Jam OS ESP" of the ESP's type, 512 MiB at 1 MiB, then "Jam OS data"
     of Jam OS's type right after it, ending on a MiB.
  2. Every block that isn't all zeros lies in the table's blocks (the
     first 2 + 32 and the last 1 + 32) or in one of the two partitions:
     nothing was written in the gaps.
  3. Any block that starts with utest's pattern (user/tests/utest/
     nvmewrite.c: "JAMOSW1", a zero, the partition and the block, 32 bits
     each) is exactly the pattern of that block, at that block of that
     partition: a write that landed in the wrong place shows. --pattern all:
     every block nvmew_write writes (each partition's first and last 8
     blocks and 64 KiB in its middle) holds it; none: none does; some
     (the default): either.
--before names the image as it was before the run (a drive that wasn't
blank, erased by `disk prepare`: tools/nvme-test.sh's `erase`): rule 2 is
then every block that differs from it, not every block that isn't zeros,
so the old data outside the table and the partitions must be there still.
--ranges prints the blocks a write may touch, "a-b,c-d,..." (b exclusive),
for tools/nvme-trace-check.py's --write-ranges, after the checks pass.
512-byte blocks only (QEMU's drives in the test). Reads only the file.
Exit 0 if every check passed, 1 naming what didn't."""
import argparse
import struct
import sys
import uuid
import zlib

BS = 512
MIB = 1 << 20
ARRAY_BLOCKS = 128 * 128 // BS
ESP = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B").bytes_le
JAMOS = uuid.UUID("71BAE565-D4A8-4315-8C81-9B35462CC158").bytes_le
MAGIC = b"JAMOSW1\0"
RUN = 8
MID = (64 << 10) // BS


def fail(why):
    print("nvme-written: FAILED: %s" % why)
    sys.exit(1)


def header(img, lba, other, blocks):
    img.seek(lba * BS)
    h = img.read(BS)
    sig, rev, size, crc, res, my, alt, first, last, guid, alba, count, esize, acrc = \
        struct.unpack_from("<8sIIIIQQQQ16sQIII", h)
    if sig != b"EFI PART" or rev != 0x10000 or size != 92 or res:
        fail("the header at block %d isn't a GPT header" % lba)
    if zlib.crc32(h[:16] + b"\0\0\0\0" + h[20:92]) & 0xffffffff != crc:
        fail("the header at block %d: its CRC" % lba)
    if my != lba or alt != other:
        fail("the header at block %d: MyLBA %d, AlternateLBA %d" % (lba, my, alt))
    if count != 128 or esize != 128:
        fail("the header at block %d: %d entries of %d bytes" % (lba, count, esize))
    img.seek(alba * BS)
    arr = img.read(count * esize)
    if zlib.crc32(arr) & 0xffffffff != acrc:
        fail("the entry array of the header at block %d: its CRC" % lba)
    if first != 2 + ARRAY_BLOCKS or last != blocks - 2 - ARRAY_BLOCKS:
        fail("the usable range %d..%d" % (first, last))
    return (guid, first, last, acrc), arr


def table(img, blocks):
    """The checks of rule 1; the partitions' [start, end)."""
    img.seek(0)
    mbr = img.read(BS)
    if mbr[510:512] != b"\x55\xaa" or mbr[446 + 4] != 0xEE or any(mbr[462:510]):
        fail("block 0 isn't a protective MBR with one entry")
    p, arr = header(img, 1, blocks - 1, blocks)
    b, barr = header(img, blocks - 1, 1, blocks)
    if p != b or arr != barr:
        fail("the two headers disagree")
    parts = []
    for i in range(128):
        e = arr[i * 128:(i + 1) * 128]
        if not any(e[:16]):
            continue
        start, end = struct.unpack_from("<QQ", e, 32)
        name = e[56:128].decode("utf-16-le").split("\0")[0]
        parts.append((i, e[:16], start, end + 1, name))
    want = [(0, ESP, MIB // BS, (MIB + (512 << 20)) // BS, "Jam OS ESP")]
    if len(parts) != 2 or parts[0] != want[0]:
        fail("the entries %s aren't Jam OS's ESP and data partition" % parts)
    i, t, start, end, name = parts[1]
    if (i, t, start, name) != (1, JAMOS, want[0][3], "Jam OS data") or end * BS % MIB or \
            end > p[2] + 1:
        fail("the data partition %s isn't Jam OS's" % (parts[1],))
    return [(want[0][2], want[0][3]), (start, end)]


def pattern(part, block):
    head = MAGIC + struct.pack("<II", part, block)
    return head + bytes((block * 7 + k + part * 13) & 0xFF for k in range(16, BS))


def runs(blocks):
    return [(0, RUN), (blocks // 2 // RUN * RUN, MID), (blocks - RUN, RUN)]


def scan(img, size, allowed, parts, before=None):
    """Rules 2 and 3: every non-zero block (with `before`: every block
    that differs from it) allowed, every pattern block in its place.
    Returns the pattern blocks found, as (part, block)."""
    found = set()
    zero = bytes(MIB)
    img.seek(0)
    if before:
        before.seek(0)
    for off in range(0, size, MIB):
        chunk = img.read(MIB)
        old = before.read(MIB) if before else zero[:len(chunk)]
        if chunk == old:
            continue
        for k in range(0, len(chunk), BS):
            blk = chunk[k:k + BS]
            if blk == old[k:k + BS]:
                continue
            lba = (off + k) // BS
            if not any(a <= lba < b for a, b in allowed):
                fail("block %d %s: outside the table and the partitions" %
                     (lba, "changed" if before else "isn't zeros"))
            if blk[:8] != MAGIC:
                continue
            part, block = struct.unpack_from("<II", blk, 8)
            if part > 1 or parts[part][0] + block != lba or blk != pattern(part, block):
                fail("block %d holds partition %d's block %d's pattern: misplaced or wrong" %
                     (lba, part, block))
            found.add((part, block))
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--pattern", default="some", choices=["none", "some", "all"])
    ap.add_argument("--ranges", action="store_true")
    ap.add_argument("--before")
    a = ap.parse_args()
    with open(a.image, "rb") as img:
        img.seek(0, 2)
        size = img.tell()
        blocks = size // BS
        parts = table(img, blocks)
        allowed = [(0, 2 + ARRAY_BLOCKS), (blocks - 1 - ARRAY_BLOCKS, blocks)] + parts
        before = open(a.before, "rb") if a.before else None
        if before and before.seek(0, 2) != size:
            fail("--before %s isn't the image's size" % a.before)
        found = scan(img, size, allowed, parts, before)
    want = {(p, s + i) for p, (a0, b0) in enumerate(parts) for s, n in runs(b0 - a0)
            for i in range(n)}
    if a.pattern == "all" and want - found:
        fail("%d of utest's %d written blocks aren't there" % (len(want - found), len(want)))
    if a.pattern == "none" and found:
        fail("%d of utest's blocks are there already" % len(found))
    if found - want:
        fail("pattern blocks utest doesn't write: %s" % sorted(found - want)[:4])
    print("nvme-written: Jam OS's table; ESP %d+%d, data %d+%d; nothing %s outside them; %d "
          "pattern blocks, each in its place" % (parts[0][0], parts[0][1] - parts[0][0],
                                                parts[1][0], parts[1][1] - parts[1][0],
                                                "changed" if a.before else "written",
                                                len(found)))
    if a.ranges:
        print(",".join("%d-%d" % r for r in allowed))
    return 0


sys.exit(main())
