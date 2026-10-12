#!/usr/bin/env python3
"""Create an empty disk image with an MBR holding Jam OS's two partitions.

  1. the EFI System Partition (type 0xEF): from 1 MiB to esp_end_mib, the
     one UEFI boots (Limine, the kernel, bootfs). Jam OS never writes it.
  2. the data partition (type 0x0C, FAT32 LBA): from esp_end_mib to the end
     of the image, mounted at /data, formatted with the label JAMOS-DATA.

The Makefile formats both (each with its own size: mformat -T) and copies
the boot files in with mtools (image@@1M). On a real stick,
tools/write-usb.sh then grows partition 2 to the end of the stick
(tools/mbr-grow.py), and Jam OS formats it on first boot.
The MBR gets a random, non-zero disk id (bytes 440-443): the loader tells
the kernel the id of the disk it booted from, and devmgr takes the Jam OS
disk with that id as the boot disk, so two sticks made with `make usb`
are told apart.
Usage: mkimage.py <out.img> <esp_end_mib> <size_mib>
       mkimage.py --data-at <img>   print where an image's data partition
                                    starts, as mtools' offset ("256M"): the
                                    tests write /data's files with it
                                    (mcopy -i img@@256M), so they follow
                                    the Makefile's ESP_END_MIB"""
import secrets
import struct
import sys

if sys.argv[1:2] == ["--data-at"] and len(sys.argv) == 3:
    with open(sys.argv[2], "rb") as f:
        mbr = f.read(512)
    if len(mbr) < 512 or mbr[510:512] != b"\x55\xaa" or mbr[462 + 4] != 0x0C:
        sys.exit(f"mkimage: {sys.argv[2]} has no Jam OS data partition (MBR entry 2, type 0x0C)")
    start, = struct.unpack_from("<I", mbr, 462 + 8)
    if start % 2048:
        sys.exit(f"mkimage: {sys.argv[2]}'s data partition doesn't start on a MiB")
    print(f"{start // 2048}M")
    sys.exit(0)

out, esp_end_mib, size_mib = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
sectors = size_mib * 2048
esp_start, data_start = 2048, esp_end_mib * 2048
assert esp_start < data_start < sectors, "the ESP must end inside the image"


def entry(ptype, start, count):
    # status, CHS start (unused), type, CHS end (unused), LBA start, count
    return struct.pack("<B3sB3sII", 0x00, b"\xfe\xff\xff", ptype, b"\xfe\xff\xff",
                       start, count)


mbr = bytearray(512)
mbr[446:462] = entry(0xEF, esp_start, data_start - esp_start)
mbr[462:478] = entry(0x0C, data_start, sectors - data_start)
mbr[440:444] = struct.pack("<I", secrets.randbelow(0xFFFFFFFF) + 1)
mbr[510:512] = b"\x55\xaa"

with open(out, "wb") as f:
    f.write(mbr)
    f.truncate(sectors * 512)
