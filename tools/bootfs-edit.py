#!/usr/bin/env python3
"""A boot image with files added or replaced, for the update tests
(tools/update-test.sh, tools/update-net-test.sh): another build to fetch.

    bootfs-edit.py <in.img> <out.img> <name>=<file> [<name>=<file> ...]

Every file of in.img is kept (as tools/mkbootfs.py packed it) except those
named, which come from the given files, or are left out with no file
(`update.pub=`: a build without an update key); tools/mkbootfs.py packs the
result."""
import os
import struct
import subprocess
import sys
import tempfile

HEADER = struct.Struct("<8sIIQ")    # tools/mkbootfs.py's
ENTRY = struct.Struct("<56sQQ")


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    img = open(src, "rb").read()
    magic, _, count, _ = HEADER.unpack_from(img, 0)
    if magic != b"JAMBOOTF":
        sys.exit("bootfs-edit: %s is not a boot image" % src)
    given = dict(arg.split("=", 1) for arg in sys.argv[3:])
    with tempfile.TemporaryDirectory(prefix="bootfs-edit.") as tmp:
        files = {}
        for i in range(count):
            name, off, n = ENTRY.unpack_from(img, HEADER.size + ENTRY.size * i)
            name = name.split(b"\0")[0].decode()
            path = os.path.join(tmp, "%d" % i)
            with open(path, "wb") as f:
                f.write(img[off:off + n])
            files[name] = path
        files.update(given)
        files = {name: path for name, path in files.items() if path}
        tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mkbootfs.py")
        args = [sys.executable, tool, out] + ["%s=%s" % kv for kv in sorted(files.items())]
        sys.exit(subprocess.run(args, stdout=subprocess.DEVNULL).returncode)


if __name__ == "__main__":
    main()
