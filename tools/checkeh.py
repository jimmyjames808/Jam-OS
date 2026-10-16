#!/usr/bin/env python3
"""Check that the libc.a members a backtrace passes through carry unwind
tables (.eh_frame).

    checkeh.py <readelf> <libc.a> <member> ...

backtrace() (user/posix/glibc/execinfo.c) and C++ exceptions walk the
stack by the program's own .eh_frame: a signal handler is called from the
call layer's delivery, a thread from musl's start, a qsort comparator from
musl's qsort. A member built without -fasynchronous-unwind-tables ends the
trace there, with no error anywhere else (posixtest's execinfo section is
the test that notices, late). An object kept from a build before the flag
was set is the way it happens, so this looks at the archive itself: every
member of each name given (an archive may hold two of one name) must hold
.eh_frame, and each name must be there. Prints what is wrong and exits 1.

Run from the repository root (`make check` does)."""
import subprocess
import sys


def members(readelf, archive):
    """{member name: [True if that copy has .eh_frame, ...]} from readelf -S."""
    out = subprocess.run([readelf, "-SW", archive], capture_output=True, text=True,
                         check=True).stdout
    found = {}
    cur = None
    for line in out.splitlines():
        if line.startswith("File: "):
            name = line.rsplit("(", 1)[-1].rstrip(")")
            cur = [False]
            found.setdefault(name, []).append(cur)
        elif cur is not None and " .eh_frame" in line:
            cur[0] = True
    return {name: [c[0] for c in copies] for name, copies in found.items()}


def main(argv):
    if len(argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    readelf, archive, wanted = argv[1], argv[2], argv[3:]
    have = members(readelf, archive)
    bad = 0
    for name in wanted:
        if name not in have:
            print(f"checkeh: {archive} has no member {name}")
            bad += 1
        elif not all(have[name]):
            print(f"checkeh: {archive}({name}) has no .eh_frame: built without "
                  "-fasynchronous-unwind-tables, or kept from a build before that flag")
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
