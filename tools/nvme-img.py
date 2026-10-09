#!/usr/bin/env python3
"""Make the disk images tools/nvme-test.sh gives QEMU's emulated NVMe drives.

    nvme-img.py <out.img> <layout> [--block-size 512|4096] [--damage <what>]

Layouts (a sparse file; the data in it is seeded, so every run makes the
same bytes and the tests can name hashes):
  windows   the PC's drive: protective MBR, GPT with four partitions
              1  EFI system partition       100 MiB  FAT32 (512-byte blocks)
                 EFI/Microsoft/Boot/bootmgfw.efi and BCD in it
              2  Microsoft reserved          16 MiB  zeros
              3  Basic data partition      1280 MiB  noise at the region
                 list's samples (plan: docs/M12.7-PLAN.md, "The region list")
              4  Windows recovery           750 MiB  noise in places
  small     a one-partition GPT disk (64 MiB of basic data): a second
              namespace for the "not served" case
  blank     all zeros, 64 MiB
--block-size sets the logical block size the table is written for (the
image is the same bytes either way; the QEMU device must be given the same
logical_block_size). At 4096 the ESP is noise, not FAT (fat reads 512 only).
--damage breaks one thing after the table is written:
  primary-header   the primary GPT header's CRC
  primary-entries  one byte of the primary entry array (its CRC no longer matches)
  backup-header    the backup GPT header's CRC
  backup-entries   one byte of the backup entry array
  both-headers     both headers' CRCs

For the `windows` layout it also writes <out>.files/ (bootmgfw.efi, BCD):
the bytes that went into the ESP, for the test to compare with what the guest
reads. When tools/mkgpt.py (N2) lands this can be rebuilt on it; the layout
is the same.

The ESP is formatted with mtools (mformat, mcopy), as the Makefile does.
This writes only the file named <out>.img and <out>.img.files/; it never
touches a device."""
import argparse
import binascii
import os
import random
import struct
import subprocess
import sys
import uuid

MiB = 1 << 20
GUID_ESP = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
GUID_MSR = "E3C9E316-0B5C-4DB8-817D-F92DF00215AE"
GUID_BASIC = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
GUID_RECOVERY = "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC"
DISK_GUID = "5A6F5E34-6A61-4D4F-8A44-4A414D4F5301"


def guid_bytes(s):
    return uuid.UUID(s).bytes_le


def crc32(b):
    return binascii.crc32(b) & 0xFFFFFFFF


def make_entries(parts, bs, n_entries=128):
    """The entry array: parts are (type, unique, first, last, name)."""
    arr = bytearray(n_entries * 128)
    for i, (typ, uniq, first, last, name) in enumerate(parts):
        e = guid_bytes(typ) + guid_bytes(uniq) + struct.pack("<QQQ", first, last, 0)
        e += name.encode("utf-16-le").ljust(72, b"\0")
        arr[i * 128:(i + 1) * 128] = e
    return bytes(arr)


def make_header(my, alt, first_usable, last_usable, entries_lba, entries):
    h = bytearray(92)
    h[0:8] = b"EFI PART"
    struct.pack_into("<IIII", h, 8, 0x00010000, 92, 0, 0)
    struct.pack_into("<QQQQ", h, 24, my, alt, first_usable, last_usable)
    h[56:72] = guid_bytes(DISK_GUID)
    struct.pack_into("<QIII", h, 72, entries_lba, len(entries) // 128, 128, crc32(entries))
    struct.pack_into("<I", h, 16, crc32(bytes(h)))
    return bytes(h)


def protective_mbr(total_blocks):
    mbr = bytearray(512)
    n = min(total_blocks - 1, 0xFFFFFFFF)
    mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff", 1, n)
    mbr[510:512] = b"\x55\xaa"
    return bytes(mbr)


class Img:
    def __init__(self, path, size, bs):
        self.f = open(path, "wb")
        self.f.truncate(size)
        self.size, self.bs = size, bs

    def put(self, off, data):
        self.f.seek(off)
        self.f.write(data)

    def putlba(self, lba, data):
        self.put(lba * self.bs, data)

    def noise(self, rng, off, length):
        self.put(off, rng.randbytes(length))

    def close(self):
        self.f.close()


def write_gpt(img, parts, bs):
    total = img.size // bs
    n_entries = 128
    arr_blocks = (n_entries * 128 + bs - 1) // bs
    first_usable = 2 + arr_blocks
    last_usable = total - 2 - arr_blocks
    entries = make_entries(parts, bs, n_entries)
    pad = entries.ljust(arr_blocks * bs, b"\0")
    img.putlba(0, protective_mbr(total).ljust(bs, b"\0"))
    img.putlba(1, make_header(1, total - 1, first_usable, last_usable, 2, entries).ljust(bs, b"\0"))
    img.putlba(2, pad)
    img.putlba(total - 1 - arr_blocks, pad)
    img.putlba(total - 1, make_header(total - 1, 1, first_usable, last_usable,
                                       total - 1 - arr_blocks, entries).ljust(bs, b"\0"))
    return first_usable, last_usable, arr_blocks


def damage(path, bs, what):
    size = os.path.getsize(path)
    total = size // bs
    arr_blocks = (128 * 128 + bs - 1) // bs
    offsets = {
        "primary-header": bs + 16,                       # the header's own CRC field
        "primary-entries": 2 * bs + 5,                   # inside entry 1's type GUID
        "backup-header": (total - 1) * bs + 16,
        "backup-entries": (total - 1 - arr_blocks) * bs + 5,
    }
    which = ["primary-header", "backup-header"] if what == "both-headers" else [what]
    with open(path, "r+b") as f:
        for w in which:
            f.seek(offsets[w])
            b = f.read(1)
            f.seek(offsets[w])
            f.write(bytes([b[0] ^ 0xFF]))


def layout_windows(path, bs):
    mib = lambda n: n * MiB // bs     # MiB -> blocks
    esp, msr, basic, rec = 100, 16, 1280, 750
    start = 1                          # MiB; the table takes the first one
    sizes = [esp, msr, basic, rec]
    total_mib = start + sum(sizes) + 1  # +1: the backup table
    img = Img(path, total_mib * MiB, bs)
    rng = random.Random(0x4E564D45)
    types = [GUID_ESP, GUID_MSR, GUID_BASIC, GUID_RECOVERY]
    names = ["EFI system partition", "Microsoft reserved partition", "Basic data partition", ""]
    parts, at = [], start
    for i, (t, n, sz) in enumerate(zip(types, names, sizes)):
        first = mib(at)
        last = mib(at + sz) - 1
        parts.append((t, "00000000-0000-4000-8000-00000000000%d" % (i + 1), first, last, n))
        at += sz
    write_gpt(img, parts, bs)
    # 3: noise in the first and last 64 MiB and at the 32 sample points of
    # 1 MiB (the region list's formula), so a hash of any of them is not a
    # hash of zeros; the rest of C: stays a hole.
    p3 = parts[2]
    first, size_blocks = p3[2], p3[3] - p3[2] + 1
    img.noise(rng, first * bs, 64 * MiB)
    img.noise(rng, (first + size_blocks) * bs - 64 * MiB, 64 * MiB)
    for k in range(32):
        img.noise(rng, (first + k * (size_blocks // 32)) * bs, MiB)
    # 4, the recovery partition: noise at its ends and middle (it is hashed whole)
    p4 = parts[3]
    r_first, r_bytes = p4[2] * bs, (p4[3] - p4[2] + 1) * bs
    for off in (0, r_bytes // 2, r_bytes - MiB):
        img.noise(rng, r_first + off, MiB)
    # 1, the ESP: FAT32 with the files a Windows ESP has
    esp_first, esp_blocks = parts[0][2], parts[0][3] - parts[0][2] + 1
    files = path + ".files"
    os.makedirs(files, exist_ok=True)
    boot = rng.randbytes(1_250_000)
    bcd = rng.randbytes(262_144)
    with open(os.path.join(files, "bootmgfw.efi"), "wb") as f:
        f.write(boot)
    with open(os.path.join(files, "BCD"), "wb") as f:
        f.write(bcd)
    if bs == 512:
        img.close()
        spec = "%s@@%d" % (path, esp_first * bs)
        run = lambda *a: subprocess.run(a, check=True, stdout=subprocess.DEVNULL)
        run("mformat", "-i", spec, "-T", str(esp_blocks), "-F", "-v", "SYSTEM", "::")
        run("mmd", "-i", spec, "::/EFI", "::/EFI/Microsoft", "::/EFI/Microsoft/Boot", "::/EFI/Boot")
        run("mcopy", "-i", spec, os.path.join(files, "bootmgfw.efi"), os.path.join(files, "BCD"),
            "::/EFI/Microsoft/Boot/")
        run("mcopy", "-i", spec, os.path.join(files, "bootmgfw.efi"), "::/EFI/Boot/bootx64.efi")
    else:
        img.noise(rng, esp_first * bs, 4 * MiB)
        img.close()


def layout_small(path, bs):
    img = Img(path, 64 * MiB, bs)
    n = 64 * MiB // bs
    first = MiB // bs
    last = n - 2 - (128 * 128 + bs - 1) // bs   # the last usable block
    write_gpt(img, [(GUID_BASIC, "00000000-0000-4000-8000-0000000000a1", first, last,
                     "Second namespace")], bs)
    img.noise(random.Random(7), first * bs, MiB)
    img.close()


def layout_blank(path, bs):
    img = Img(path, 64 * MiB, bs)
    img.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("layout", choices=["windows", "small", "blank"])
    ap.add_argument("--block-size", type=int, default=512, choices=[512, 4096])
    ap.add_argument("--damage", choices=["primary-header", "primary-entries", "backup-header",
                                          "backup-entries", "both-headers"])
    a = ap.parse_args()
    if a.out.startswith("/dev/") or not a.out.endswith(".img"):
        sys.exit("nvme-img: the output must be a file named *.img, never a device")
    {"windows": layout_windows, "small": layout_small, "blank": layout_blank}[a.layout](
        a.out, a.block_size)
    if a.damage:
        if a.layout == "blank":
            sys.exit("nvme-img: nothing to damage in a blank disk")
        damage(a.out, a.block_size, a.damage)


main()
