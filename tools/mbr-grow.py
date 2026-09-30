#!/usr/bin/env python3
"""Grow Jam OS's data partition (MBR entry 2) to the end of the disk, and
wipe its first MiB so Jam OS formats it, full size, on the next boot.

tools/write-usb.sh runs it on the stick right after writing the image
(which holds a 64 MiB data partition). It refuses a disk whose MBR isn't
Jam OS's layout (entry 1 type 0xEF, entry 2 type 0x0C), a shrink, and a
size it can't use.
Usage: mbr-grow.py <disk or image> <total 512-byte sectors>"""
import struct
import sys

path, total = sys.argv[1], int(sys.argv[2])
ENTRY = "<B3sB3sII"

with open(path, "r+b", buffering=0) as f:
    mbr = bytearray(f.read(512))
    if mbr[510:512] != b"\x55\xaa":
        sys.exit("mbr-grow: no MBR signature: not a Jam OS disk")
    esp = struct.unpack(ENTRY, mbr[446:462])
    data = list(struct.unpack(ENTRY, mbr[462:478]))
    if esp[2] != 0xEF or data[2] != 0x0C:
        sys.exit("mbr-grow: partitions are not ESP + FAT32 data: not a Jam OS disk")
    start = data[4]
    if total >= 1 << 32:
        sys.exit("mbr-grow: disk too big for an MBR partition (2 TiB)")
    if total <= start + data[5]:
        sys.exit("mbr-grow: the disk is no bigger than the image: nothing to grow")
    data[5] = total - start
    mbr[462:478] = struct.pack(ENTRY, *data)
    f.seek(0)
    f.write(bytes(mbr))
    f.seek(start * 512)
    f.write(bytes(1 << 20))   # no FAT here now: Jam OS formats it, full size
print(f"mbr-grow: data partition now {data[5]} sectors ({data[5] * 512 >> 20} MiB) from "
      f"sector {start}; it is formatted on the first boot")
