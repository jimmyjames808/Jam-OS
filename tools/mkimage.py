#!/usr/bin/env python3
"""Create an empty disk image with an MBR holding one EFI System Partition.

The partition starts at 1 MiB and fills the rest of the image; the Makefile
then formats it FAT32 and copies files in with mtools (image@@1M).
Usage: mkimage.py <out.img> <size_mib>"""
import struct
import sys

out, size_mib = sys.argv[1], int(sys.argv[2])
sectors = size_mib * 2048
start = 2048

mbr = bytearray(512)
# Partition entry 1: status, CHS start (unused), type 0xEF (EFI System),
# CHS end (unused), LBA start, sector count.
mbr[446:462] = struct.pack("<B3sB3sII", 0x00, b"\xfe\xff\xff", 0xEF,
                           b"\xfe\xff\xff", start, sectors - start)
mbr[510:512] = b"\x55\xaa"

with open(out, "wb") as f:
    f.write(mbr)
    f.truncate(sectors * 512)
