#!/usr/bin/env python3
"""The hash bigpkg prints (tools/bigpkg.c): FNV-1a, 64 bits, over the
512 words of every STEP-th page of its table and of the last page (the table
of PAGES pages holds the word i in every place of page i), as 16 hex digits.
tools/pkg-test.sh puts it in the scripts that run the program.

    bigpkg-hash.py PAGES STEP
"""
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    pages, step = int(sys.argv[1]), int(sys.argv[2])
    h = 1469598103934665603
    seen = list(range(0, pages, step))
    if (pages - 1) % step:
        seen.append(pages - 1)
    for p in seen:
        for _ in range(512):
            h = ((h ^ p) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    print("%016x" % h)


if __name__ == "__main__":
    main()
