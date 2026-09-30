#!/usr/bin/env python3
"""The volume label of a FAT32 volume inside an image file, as the other
systems read it.

    fat-label.py <image> <offset in MiB> [--fix]

mtools' mformat -v writes the label into the boot sector and into the root
directory, but puts a long-name entry in front of the root directory's
label entry, which a label must not have (macOS's fsck_msdos: "Invalid long
filename entry for volume label"). Without --fix this checks: the root
directory's first cluster holds a label entry (attribute 08) with no
long-name entry before it, and the boot sector's label field says the
same; exit 1 (and why) otherwise. With --fix it removes the long-name
entries in front of the label entry first.
"""
import struct
import sys

path, offset = sys.argv[1], int(sys.argv[2]) << 20
fix = "--fix" in sys.argv[3:]

with open(path, "r+b" if fix else "rb") as f:
    f.seek(offset)
    boot = f.read(512)
    bps, spc, reserved, nfats = struct.unpack_from("<HBHB", boot, 11)
    if boot[510:512] != b"\x55\xaa" or boot[82:87] != b"FAT32" or not bps or not spc:
        sys.exit(f"fat-label: {path}: no FAT32 volume at {offset >> 20} MiB")
    fat_size, = struct.unpack_from("<I", boot, 36)
    root, = struct.unpack_from("<I", boot, 44)
    at = offset + (reserved + nfats * fat_size + (root - 2) * spc) * bps
    f.seek(at)
    cluster = bytearray(f.read(spc * bps))
    entries = [cluster[i:i + 32] for i in range(0, len(cluster), 32)]
    used = [e for e in entries if e[0] != 0]
    label = next((i for i, e in enumerate(used) if e[11] == 0x08 and e[0] != 0xe5), None)
    if label is None:
        sys.exit(f"fat-label: {path}: the root directory has no volume label entry")
    first = label
    while first > 0 and used[first - 1][11] == 0x0f:
        first -= 1
    if first != label and fix:
        del used[first:label]
        label = first
        cluster[:] = b"".join(used).ljust(len(cluster), b"\0")
        f.seek(at)
        f.write(cluster)
    elif first != label:
        sys.exit(f"fat-label: {path}: a long-name entry in front of the volume label entry")
    if bytes(used[label][:11]) != boot[71:82]:
        sys.exit(f"fat-label: {path}: the root directory says {bytes(used[label][:11])!r}, "
                 f"the boot sector {boot[71:82]!r}")
