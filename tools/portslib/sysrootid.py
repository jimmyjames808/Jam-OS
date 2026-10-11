"""The sysroot's identity for a package's `built-with sysroot` line: which
musl, call layer and libjam a port's programs carry, as a stamp that is
the same on every build host for the same sources.

The builder's own stamp (build.py's sysroot_stamp) hashes build/sysroot's
bytes as they are, which is right for "is this port out of date" but
differs between the Mac and the PC: the sysroot is built with -g, so its
objects carry the build directory's path, and libc.a's members carry
times and owners. A program links none of that (the stage strips debug
information), so the identity hashes each object and archive as `strip
--strip-debug -D` leaves it (no debug information; an archive's members
with zero times and owners) and every other file as it is, with its path.
"""
import hashlib
import os
import subprocess
import tempfile

from . import toolchain

_memo = {}   # the sysroot's raw stamp -> its identity, for one run


def identity(sysroot, raw_stamp):
    """The sysroot's identity (64 hex digits); raw_stamp is build.py's
    stamp of the same tree, which says when to compute it again."""
    if raw_stamp in _memo:
        return _memo[raw_stamp]
    strip = toolchain.cross() + "strip"
    h = hashlib.sha256(b"jamos-sysroot-id 1\n")
    with tempfile.TemporaryDirectory(prefix="sysroot-id.") as tmp:
        out = os.path.join(tmp, "x")
        for d, dirs, files in os.walk(sysroot):
            dirs.sort()
            for f in sorted(files):
                path = os.path.join(d, f)
                if f.endswith((".a", ".o")) and os.path.getsize(path) > 8:
                    subprocess.run([strip, "--strip-debug", "-D", "-o", out, path], check=True)
                    path = out
                with open(path, "rb") as fh:
                    digest = hashlib.sha256(fh.read()).hexdigest()
                rel = os.path.relpath(os.path.join(d, f), sysroot)
                h.update(("%s %s\n" % (rel, digest)).encode())
    _memo[raw_stamp] = h.hexdigest()
    return _memo[raw_stamp]
