#!/usr/bin/env python3
"""Create a disk image standing in for someone else's USB stick (the tests
of /usb0, /usb1, ...: tools/sticks-test.sh).

    mkstick.py <out.img> <size_mib> [<type>[:<mib>] ...] [--fill]

With types (hex MBR partition types, e.g. 0c): an MBR whose partitions
start at 1 MiB and follow one another, each <mib> MiB long (the last one
without a size takes the rest). Without any: no partition table at all
(format the whole image for a "superfloppy", or leave it as it is).
--fill puts pseudo-random bytes everywhere outside the MBR instead of
zeros (seeded: the same image every time), and makes sure block 0 of an
image without a table has no boot signature.
Nothing is formatted here: the caller does that with mtools
(`mformat -i img@@1M ...`)."""
import random
import struct
import sys

args = [a for a in sys.argv[1:] if a != "--fill"]
fill = "--fill" in sys.argv[1:]
out, size_mib, parts = args[0], int(args[1]), args[2:]
sectors = size_mib * 2048
assert len(parts) <= 4, "an MBR has four entries"

data = bytearray(random.Random(out.rsplit("/", 1)[-1]).randbytes(sectors * 512)) if fill \
    else bytearray(sectors * 512)
if parts:
    data[0:512] = bytes(512)
    start = 2048
    for i, p in enumerate(parts):
        ptype, _, mib = p.partition(":")
        count = int(mib) * 2048 if mib else sectors - start
        assert count > 0 and start + count <= sectors, "the partitions must fit the image"
        # status, CHS start (unused), type, CHS end (unused), LBA start, count
        data[446 + 16 * i:462 + 16 * i] = struct.pack(
            "<B3sB3sII", 0, b"\xfe\xff\xff", int(ptype, 16), b"\xfe\xff\xff", start, count)
        start += count
    data[510:512] = b"\x55\xaa"
elif fill:
    data[510:512] = b"\x00\x00"

with open(out, "wb") as f:
    f.write(data)
