#!/usr/bin/env python3
"""A sparse copy of a file, for the base_unittests stick (tools/cr/base-stick.py
makes it, tools/cr/base-run.py copies it for every boot): holes in the source
stay holes, and so do 64 KiB blocks of zeros. The stick's /data is 6 GiB so
that DriveInfoTest sees a big enough volume, and almost all of it is never
written: a plain copy (shutil.copyfile) would write all of it each time.

    sparsecopy.py <src> <dst> [<bytes>]   the first <bytes> of <src> (all of it
                                          left out), <dst> exactly that long
    sparsecopy.py --selftest

Python 3.9 and later; on a file system without SEEK_DATA every block is read
(the zero ones are still left as holes)."""
import errno
import hashlib
import os
import sys
import tempfile

BLOCK = 1 << 16


def next_data(fd, pos):
    """The offset of the first data at or after pos in fd's file, or None when
    only a hole is left; pos itself when the file system can't say."""
    seek_data = getattr(os, "SEEK_DATA", None)
    if seek_data is None:
        return pos
    try:
        return os.lseek(fd, pos, seek_data)
    except OSError as e:
        return None if e.errno == errno.ENXIO else pos


def copy(src, dst, size=None):
    """src (its first size bytes) copied to dst, which is exactly that long."""
    with open(src, "rb", buffering=0) as s, open(dst, "wb", buffering=0) as d:
        total = os.fstat(s.fileno()).st_size if size is None else size
        pos = 0
        while pos < total:
            pos = next_data(s.fileno(), pos)
            if pos is None or pos >= total:
                break
            s.seek(pos)
            block = s.read(min(BLOCK, total - pos))
            if not block:
                break
            if block.count(0) != len(block):
                d.seek(pos)
                view = memoryview(block)
                while view:
                    view = view[d.write(view):]
            pos += len(block)
        d.truncate(total)


def digest(path, n):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while n:
            block = f.read(min(n, 1 << 20))
            if not block:
                break
            h.update(block)
            n -= len(block)
    return h.hexdigest()


def real(path):
    return os.stat(path).st_blocks * 512


def selftest():
    mib = 1 << 20
    with tempfile.TemporaryDirectory(prefix="sparsecopy.") as t:
        a = os.path.join(t, "a")
        with open(a, "wb") as f:
            f.write(b"head")
            f.seek(5 * mib + 7)                # a hole, then data off every boundary
            f.write(os.urandom(300000))
            f.write(bytes(3 * BLOCK))          # zeros written out
            f.write(b"tail")
            f.truncate(64 * mib)               # and a hole to the end
        size = os.stat(a).st_size
        holes = real(a) < 2 * mib              # a file system that keeps holes (tmpfs, ext4; not APFS here)
        b = os.path.join(t, "b")
        copy(a, b)
        assert os.stat(b).st_size == size and digest(a, size) == digest(b, size), "the copy differs"
        if holes:
            assert real(b) < mib, "the copy of a sparse file is %d bytes on the disk" % real(b)
        c = os.path.join(t, "c")
        copy(a, c, 6 * mib)                    # the head of it
        assert os.stat(c).st_size == 6 * mib and digest(a, 6 * mib) == digest(c, 6 * mib), "the head differs"
        d = os.path.join(t, "d")
        copy(a, d, 70 * mib)                   # longer than the file: zeros to the end
        assert os.stat(d).st_size == 70 * mib and digest(a, size) == digest(d, size) and \
            digest(d, 70 * mib) == hashlib.sha256(open(a, "rb").read() + bytes(6 * mib)).hexdigest(), \
            "the long copy differs"
        z = os.path.join(t, "z")               # a dense file of zeros: no blocks at all
        with open(z, "wb") as f:
            f.write(bytes(4 * mib))
        zc = os.path.join(t, "zc")
        copy(z, zc)
        assert os.stat(zc).st_size == 4 * mib and digest(z, 4 * mib) == digest(zc, 4 * mib), "zeros differ"
        if holes:
            assert real(zc) < mib, "a file of zeros copied to %d bytes on the disk" % real(zc)
    print("sparsecopy: selftest: PASS%s" % ("" if holes else " (this file system keeps no holes: sizes not checked)"))


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        selftest()
    elif len(sys.argv) in (3, 4):
        copy(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) == 4 else None)
    else:
        sys.exit(__doc__.split("\n\n")[1])
