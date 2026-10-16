#!/usr/bin/env python3
"""The SHA-256 of what user/tests/exttest writes, made on the host
(tools/extfs-test.sh's big case):

    extbigsum.py big <MiB>     the file `exttest big <path> <MiB>` makes
    extbigsum.py sparse        the file `exttest sparse <path>` makes

The pattern and the sparse file's layout are described in
user/tests/exttest/main.c; both sides make the same bytes."""
import hashlib
import struct
import sys

BLK = 4096
GIB = 1 << 30
M64 = (1 << 64) - 1


def pattern():
    s = 0x9E3779B97F4A7C15
    out = bytearray()
    for _ in range((1 << 20) // 8):
        s ^= (s << 13) & M64
        s ^= s >> 7
        s ^= (s << 17) & M64
        out += struct.pack("<Q", s)
    return bytes(out)


PAT = pattern()


def block(n):
    o = (n % ((1 << 20) // BLK)) * BLK
    return struct.pack("<Q", n) + PAT[o + 8:o + BLK]


def zeros(h, n):
    z = bytes(1 << 20)
    while n:
        k = min(n, len(z))
        h.update(z[:k])
        n -= k


def big(mib):
    h = hashlib.sha256()
    for n in range(mib * 256):
        h.update(block(n))
    return h.hexdigest()


def sparse():
    h = hashlib.sha256()
    runs = [(0, 16), (5 * GIB - 8192, 16), (6 * GIB, 256)]
    odd_at = 5 * GIB + (5 << 20) + 77
    odd = bytes((i * 7 + 3) & 255 for i in range(100))
    size = 6 * GIB + (1 << 20) + 8192
    pieces = sorted([(a, "run", b) for a, b in runs] + [(odd_at, "odd", 0)])
    at = 0
    for start, kind, blocks in pieces:
        zeros(h, start - at)
        at = start
        if kind == "odd":
            h.update(odd)
            at += len(odd)
        else:
            for k in range(blocks):
                h.update(block(start // BLK + k))
            at += blocks * BLK
    zeros(h, size - at)
    return h.hexdigest()


if len(sys.argv) == 3 and sys.argv[1] == "big":
    print(big(int(sys.argv[2])))
elif len(sys.argv) == 2 and sys.argv[1] == "sparse":
    print(sparse())
else:
    sys.exit(__doc__)
