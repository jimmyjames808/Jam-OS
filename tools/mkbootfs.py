#!/usr/bin/env python3
"""Pack files into a bootfs image (format: kernel/include/jam/bootfs.h).

    mkbootfs.py <out.img> <name>=<file> [<name>=<file> ...]

Layout: a 24-byte header, one 72-byte entry per file, then each file's
bytes starting on a 4 KiB boundary, and the image padded to a whole number
of pages (so a page-rounded VMO over any file stays inside the image).
Names are bootfs paths like "bin/init": relative, '/'-separated, printable
ASCII, no empty, "." or ".." components, unique, at most 55 bytes.
Entries are written sorted by name so the image is reproducible."""
import struct
import sys

MAGIC = b"JAMBOOTF"
VERSION = 1
NAME_MAX = 56          # including the NUL
MAX_FILES = 256        # the kernel's limit too
PAGE = 4096
HEADER = struct.Struct("<8sIIQ")
ENTRY = struct.Struct(f"<{NAME_MAX}sQQ")


def check_name(name):
    if not name or len(name.encode()) >= NAME_MAX:
        return f"must be 1..{NAME_MAX - 1} bytes"
    if any(not (0x21 <= ord(c) <= 0x7e) for c in name):
        return "printable ASCII without spaces only"
    if name.startswith("/") or any(p in ("", ".", "..") for p in name.split("/")):
        return "must be relative with no empty, '.' or '..' components"
    return None


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, files = sys.argv[1], {}
    for arg in sys.argv[2:]:
        name, sep, path = arg.partition("=")
        if not sep:
            sys.exit(f"mkbootfs: '{arg}': want <name>=<file>")
        err = check_name(name)
        if err:
            sys.exit(f"mkbootfs: bad name '{name}': {err}")
        if name in files:
            sys.exit(f"mkbootfs: '{name}' given twice")
        data = open(path, "rb").read()
        if not data:
            sys.exit(f"mkbootfs: '{path}' is empty (bootfs has no empty files)")
        files[name] = data
    if len(files) > MAX_FILES:
        sys.exit(f"mkbootfs: {len(files)} files, at most {MAX_FILES}")

    names = sorted(files)
    pos = (HEADER.size + ENTRY.size * len(names) + PAGE - 1) // PAGE * PAGE
    entries, blobs = [], []
    for name in names:
        data = files[name]
        entries.append(ENTRY.pack(name.encode(), pos, len(data)))
        blobs.append((pos, data))
        pos = (pos + len(data) + PAGE - 1) // PAGE * PAGE

    image = bytearray(pos)
    image[0:HEADER.size] = HEADER.pack(MAGIC, VERSION, len(names), pos)
    for i, e in enumerate(entries):
        off = HEADER.size + i * ENTRY.size
        image[off:off + ENTRY.size] = e
    for off, data in blobs:
        image[off:off + len(data)] = data
    with open(out, "wb") as f:
        f.write(image)
    print(f"mkbootfs: {out}: {len(names)} files, {len(image) // 1024} KiB")


if __name__ == "__main__":
    main()
