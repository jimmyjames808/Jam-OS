#!/usr/bin/env python3
"""Create an empty disk image with an MBR holding Jam OS's two partitions.

  1. the EFI System Partition (type 0xEF): from 1 MiB to esp_end_mib, the
     one UEFI boots (Limine, the kernel, bootfs). Jam OS never writes it.
  2. the data partition: from esp_end_mib to the end of the image, mounted
     at /data, labelled JAMOS-DATA. Type 0x83 when it is to be an ext volume
     (`--data ext`: the Makefile's default, docs/M13.6-PLAN.md "Ext by
     default"), type 0x0C (FAT32 LBA) when it is FAT32 (`--data fat`, the
     default here: the tests' own disks).

The Makefile formats the ESP with mtools (mformat, mcopy at image@@1M: the
firmware reads only FAT) and the data partition with jamext (an ext volume,
`jamext --part 2 mkfs`) or mformat (FAT). On a real stick,
tools/write-usb.sh then grows partition 2 to the end of the stick
(tools/mbr-grow.py) and makes its filesystem again at that size
(tools/stick-data.sh for ext; Jam OS formats a FAT one on first boot).
The MBR gets a random, non-zero disk id (bytes 440-443): the loader tells
the kernel the id of the disk it booted from, and devmgr takes the Jam OS
disk with that id as the boot disk, so two sticks made with `make usb`
are told apart.
Usage: mkimage.py <out.img> <esp_end_mib> <size_mib> [--data ext|fat]
       mkimage.py --data-at <img>   print where an image's data partition
                                    starts, as mtools' offset ("256M"): a
                                    FAT /data's files are written with it
                                    (mcopy -i img@@256M), so they follow
                                    the Makefile's ESP_END_MIB (a GPT
                                    image's: its "Jam OS data" partition)
       mkimage.py --data-fs <img>   print "ext" or "fat": what the image's
                                    data partition is meant to hold (its
                                    MBR type 0x83 or 0x0C; a GPT image's
                                    is not told by its table: it prints
                                    "gpt")
       mkimage.py --gpt <stick.img> <out.img>
                                    the stick image as an installed drive
                                    (an NVMe drive in QEMU: qemu-test.sh's
                                    QEMU_BOOT_NVME): Jam OS's GPT layout
                                    (tools/mkgpt.py's "jamos": a protective
                                    MBR, "Jam OS ESP" of 512 MiB at 1 MiB,
                                    "Jam OS data" after it, both GPT
                                    headers), a random disk GUID (the
                                    loader names the disk by it), and the
                                    stick's two volumes copied in, each
                                    at its partition's start. The data
                                    partition is as big as the stick's."""
import secrets
import struct
import sys

JAMOS_DATA = bytes.fromhex("65e5ba71a8d415438c819b35462cc158")   # its type GUID, on disk
TYPE_FAT32_LBA, TYPE_LINUX = 0x0C, 0x83   # the data partition's MBR types: FAT, ext


def gpt_data_start(path):
    """The "Jam OS data" partition's first block on a GPT image (the second
    entry of the primary table), or exit."""
    with open(path, "rb") as f:
        f.seek(512)
        hdr = f.read(92)
        if hdr[:8] != b"EFI PART":
            sys.exit(f"mkimage: {path} has a protective MBR and no GPT header")
        array, count, size = struct.unpack_from("<QII", hdr, 72)
        f.seek(array * 512 + size)
        e = f.read(48)
    if count < 2 or e[:16] != JAMOS_DATA:
        sys.exit(f"mkimage: {path} has no Jam OS data partition (GPT entry 2)")
    return struct.unpack_from("<Q", e, 32)[0]


if sys.argv[1:2] == ["--data-at"] and len(sys.argv) == 3:
    with open(sys.argv[2], "rb") as f:
        mbr = f.read(512)
    if len(mbr) == 512 and mbr[510:512] == b"\x55\xaa" and mbr[446 + 4] == 0xEE:
        start = gpt_data_start(sys.argv[2])
    elif len(mbr) < 512 or mbr[510:512] != b"\x55\xaa" or \
            mbr[462 + 4] not in (TYPE_FAT32_LBA, TYPE_LINUX):
        sys.exit(f"mkimage: {sys.argv[2]} has no Jam OS data partition "
                 "(MBR entry 2, type 0x0C or 0x83)")
    else:
        start, = struct.unpack_from("<I", mbr, 462 + 8)
    if start % 2048:
        sys.exit(f"mkimage: {sys.argv[2]}'s data partition doesn't start on a MiB")
    print(f"{start // 2048}M")
    sys.exit(0)

if sys.argv[1:2] == ["--data-fs"] and len(sys.argv) == 3:
    with open(sys.argv[2], "rb") as f:
        mbr = f.read(512)
    if len(mbr) < 512 or mbr[510:512] != b"\x55\xaa":
        sys.exit(f"mkimage: {sys.argv[2]} has no MBR")
    print("gpt" if mbr[446 + 4] == 0xEE else
          {TYPE_FAT32_LBA: "fat", TYPE_LINUX: "ext"}.get(mbr[462 + 4], "other"))
    sys.exit(0)

if sys.argv[1:2] == ["--gpt"] and len(sys.argv) == 4:
    import uuid
    import mkgpt
    stick, out = sys.argv[2], sys.argv[3]
    with open(stick, "rb") as f:
        mbr = f.read(512)
    if len(mbr) < 512 or mbr[510:512] != b"\x55\xaa" or mbr[446 + 4] != 0xEF or \
            mbr[462 + 4] not in (TYPE_FAT32_LBA, TYPE_LINUX):
        sys.exit(f"mkimage: {stick} is not a Jam OS stick image (an ESP, then a FAT32 or ext "
                 "data partition)")
    (esp_at, esp_n), (data_at, data_n) = (struct.unpack_from("<II", mbr, o + 8) for o in (446, 462))
    MIB = 1 << 20
    if esp_n * 512 > 512 * MIB or data_n * 512 % MIB:
        sys.exit(f"mkimage: {stick}'s ESP is over 512 MiB, or its data partition not whole MiB")
    # 1 MiB, the ESP, the data partition, and 1 MiB for the backup table
    # (mkgpt's layout ends the data partition on the MiB below it).
    disk = mkgpt.Disk(1 * MIB + 512 * MIB + data_n * 512 + 1 * MIB, 512)
    g = mkgpt.Gpt(disk, guid=uuid.uuid4().bytes_le)   # random, as `disk prepare` makes it
    mkgpt.layout_jamos(g)
    disk.put(0, mkgpt.mbr_block([(0, 0xEE, 1, min(disk.blocks - 1, 0xFFFFFFFF))]))
    g.write()
    disk.save(out)
    (_, _, esp_start, _, _, _), (_, _, data_start, data_blocks, _, _) = g.entries[0], g.entries[1]
    assert data_blocks == data_n, "the data partition as big as the stick's"
    with open(stick, "rb") as src, open(out, "r+b") as dst:
        for at, n, to in ((esp_at, esp_n, esp_start), (data_at, data_n, data_start)):
            src.seek(at * 512)
            dst.seek(to * 512)
            left = n * 512
            while left:
                chunk = src.read(min(left, 4 * MIB))
                if not chunk:
                    sys.exit(f"mkimage: {stick} ends inside a partition")
                dst.write(chunk)
                left -= len(chunk)
    sys.exit(0)

args = sys.argv[1:]
data_type = TYPE_FAT32_LBA
if len(args) == 5 and args[3] == "--data" and args[4] in ("ext", "fat"):
    data_type = TYPE_LINUX if args[4] == "ext" else TYPE_FAT32_LBA
    args = args[:3]
if len(args) != 3:
    sys.exit(__doc__)
out, esp_end_mib, size_mib = args[0], int(args[1]), int(args[2])
sectors = size_mib * 2048
esp_start, data_start = 2048, esp_end_mib * 2048
assert esp_start < data_start < sectors, "the ESP must end inside the image"


def entry(ptype, start, count):
    # status, CHS start (unused), type, CHS end (unused), LBA start, count
    return struct.pack("<B3sB3sII", 0x00, b"\xfe\xff\xff", ptype, b"\xfe\xff\xff",
                       start, count)


mbr = bytearray(512)
mbr[446:462] = entry(0xEF, esp_start, data_start - esp_start)
mbr[462:478] = entry(data_type, data_start, sectors - data_start)
mbr[440:444] = struct.pack("<I", secrets.randbelow(0xFFFFFFFF) + 1)
mbr[510:512] = b"\x55\xaa"

with open(out, "wb") as f:
    f.write(mbr)
    f.truncate(sectors * 512)
