#!/usr/bin/env python3
"""The records of `pagertest rec` (user/tests/pagertest/rec.c), looked at on
the host: tools/pager-test.sh reads a file out of an ext volume with
debugfs and asks whether Linux's view of the disk holds them.

    pagerrec.py <file> <records> <salt> <rsz> <acked>
    pagerrec.py --dump <number> <salt> <rsz>        (the record, as hex)

A record is rsz bytes: "JAMREC01", its number, the run's salt (8 bytes
each, little endian), a body of 64-bit words from a mixer run on the number
and the salt (cut at the sum), and the sum (CRC-32 as a 64-bit word) of
everything before it. Each record of the file is
  whole    exactly that,
  zero     all zero (never written),
  torn     every non-zero byte is the record's own (the write-back was cut
           in the middle), or
  foreign  a byte that is neither.
Exit 0 if the file is records * rsz bytes, the first <acked> records are
whole and none is foreign; 1 otherwise (with the first fault said)."""
import sys
import zlib

M = (1 << 64) - 1


def mix(x):
    x = (x + 0x9E3779B97F4A7C15) & M
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & M
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & M
    return x ^ (x >> 31)


def record(i, salt, rsz):
    s = mix(((i * 0x2545F4914F6CDD1D) & M) ^ salt)
    nbody = rsz - 8 - 24
    words = []
    for _ in range((nbody + 7) // 8):
        s = mix(s)
        words.append(s.to_bytes(8, "little"))
    pre = b"JAMREC01" + i.to_bytes(8, "little") + salt.to_bytes(8, "little") + \
        b"".join(words)[:nbody]
    return pre + zlib.crc32(pre).to_bytes(8, "little")


def classify(got, exp):
    if got == exp:
        return "whole"
    if not any(got):
        return "zero"
    if all(g == 0 or g == e for g, e in zip(got, exp)):
        return "torn"
    return "foreign"


def main():
    if len(sys.argv) == 5 and sys.argv[1] == "--dump":
        print(record(int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])).hex())
        return 0
    if len(sys.argv) != 6:
        print(__doc__.split("\n\n")[0], file=sys.stderr)
        return 2
    path = sys.argv[1]
    n, salt, rsz, acked = (int(a) for a in sys.argv[2:6])
    data = open(path, "rb").read()
    kinds = {"whole": 0, "zero": 0, "torn": 0, "foreign": 0}
    first_bad = n
    if len(data) != n * rsz:
        print("pagerrec: %s: %d bytes, %d wanted" % (path, len(data), n * rsz))
        return 1
    for i in range(n):
        got = data[i * rsz:(i + 1) * rsz]
        k = classify(got, record(i, salt, rsz))
        kinds[k] += 1
        if k != "whole" and i < first_bad:
            first_bad = i
    ok = first_bad >= acked and kinds["foreign"] == 0
    print("pagerrec: %s: %d whole, %d zero, %d torn, %d foreign of %d; first not whole %d, "
          "%d acked: %s" % (path, kinds["whole"], kinds["zero"], kinds["torn"], kinds["foreign"],
                            n, first_bad, acked, "ok" if ok else "WRONG"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
