#!/usr/bin/env python3
"""Pack files into a bootfs image (format: kernel/include/jam/bootfs.h).

    mkbootfs.py <out.img> (<name>=<file> | @<list>) ...
    mkbootfs.py --selftest      the list files and the size limit (make check)

Layout: a 24-byte header, one 72-byte entry per file, then each file's
bytes starting on a 4 KiB boundary, and the image padded to a whole number
of pages (so a page-rounded VMO over any file stays inside the image).
Names are bootfs paths like "bin/init": relative, '/'-separated, printable
ASCII, no empty, "." or ".." components, unique, at most 55 bytes.
Entries are written sorted by name so the image is reproducible. Files with
identical contents are stored once: their entries have the same offset and
size (the multi-call ports, ports/sbase, are installed under a name for
every tool they hold). The kernel's check (kernel/proc/bootfs.c) allows
exactly that and no other sharing of pages.

@<list> is a file of `<name>=<file>` lines (`#` starts a comment): what
tools/ports.py writes for the boot image's ports.

The image's size is printed against the limit of `update`: it refuses a
file over UPDATE_FILE_MAX (user/include/update.h), so an image over it is
an error (the output is removed), and one over 75% of it a warning."""
import os
import re
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


def update_limit():
    """UPDATE_FILE_MAX, the largest file `update` takes, from update.h."""
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "user", "include", "update.h")
    with open(path) as f:
        m = re.search(r"#define\s+UPDATE_FILE_MAX\s+\((\d+)u?\s*<<\s*(\d+)\)", f.read())
    if not m:
        sys.exit(f"mkbootfs: UPDATE_FILE_MAX is not in {path} in the form (N << M)")
    return int(m.group(1)) << int(m.group(2))


def read_args(args):
    """The <name>=<file> words of args, with each @<list> file's lines in
    its place."""
    out = []
    for arg in args:
        if not arg.startswith("@"):
            out.append(arg)
            continue
        with open(arg[1:]) as f:
            for line in f:
                line = line.split("#")[0].strip()
                if line:
                    out.append(line)
    return out


def selftest():
    """@list files read, identical files stored once, the limit's warning
    and error (no stale output after it); 0 on PASS."""
    import subprocess
    import tempfile
    fails = []

    def run(*args):
        p = subprocess.run([sys.executable, os.path.abspath(__file__)] + list(args),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        return p.returncode, p.stdout + p.stderr

    limit = update_limit()
    with tempfile.TemporaryDirectory() as tmp:
        def make(name, size, fill):
            path = os.path.join(tmp, name)
            with open(path, "wb") as f:
                f.write(bytes([fill]) * size)
            return path
        small, same, other = make("a", 5000, 1), make("b", 5000, 1), make("c", 100, 2)
        lst = os.path.join(tmp, "list")
        with open(lst, "w") as f:
            f.write(f"# a comment\nusr/bin/x={small}  # sha256 and so on\nusr/bin/y={same}\n\n")
        img = os.path.join(tmp, "out.img")
        rc, text = run(img, f"bin/z={other}", f"@{lst}")
        if rc != 0 or "3 files" not in text or "warning" in text:
            fails.append(f"a list file with a comment: rc {rc}: {text.strip()}")
        elif os.path.getsize(img) != 4 * 4096:   # the table, the shared 5000, the 100
            fails.append(f"identical files stored twice: image is {os.path.getsize(img)} bytes")
        rc, text = run(img, f"bin/z={make('d', limit * 3 // 4 + 4096, 3)}")
        if rc != 0 or "warning" not in text or "75%" not in text:
            fails.append(f"over 75% gave no warning: rc {rc}: {text.strip()}")
        rc, text = run(img, f"bin/z={make('e', limit, 4)}")
        if rc == 0 or "UPDATE_FILE_MAX" not in text:
            fails.append(f"over the limit not an error: rc {rc}: {text.strip()}")
        if os.path.exists(img):
            fails.append("an image over the limit left behind")
        rc, text = run(img, f"@{os.path.join(tmp, 'nothere')}")
        if rc == 0:
            fails.append("a missing list file taken")
    for f in fails:
        print("mkbootfs --selftest: " + f)
    print("mkbootfs --selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


def main():
    if sys.argv[1:] == ["--selftest"]:
        sys.exit(selftest())
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, files = sys.argv[1], {}
    for arg in read_args(sys.argv[2:]):
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
    entries, blobs, placed = [], [], {}
    for name in names:
        data = files[name]
        if data not in placed:
            placed[data] = pos
            blobs.append((pos, data))
            pos = (pos + len(data) + PAGE - 1) // PAGE * PAGE
        entries.append(ENTRY.pack(name.encode(), placed[data], len(data)))

    image = bytearray(pos)
    image[0:HEADER.size] = HEADER.pack(MAGIC, VERSION, len(names), pos)
    for i, e in enumerate(entries):
        off = HEADER.size + i * ENTRY.size
        image[off:off + ENTRY.size] = e
    for off, data in blobs:
        image[off:off + len(data)] = data
    limit = update_limit()
    if len(image) > limit:
        if os.path.exists(out):
            os.remove(out)   # no stale image to be flashed after the error
        sys.exit(f"mkbootfs: error: {out} would be {len(image) // 1024} KiB, and `update` "
                 f"refuses a file over {limit // 1024} KiB (UPDATE_FILE_MAX, "
                 f"user/include/update.h): take something out of the boot image")
    with open(out, "wb") as f:
        f.write(image)
    print(f"mkbootfs: {out}: {len(names)} files, {len(image) // 1024} KiB, "
          f"{len(image) * 100 // limit}% of update's {limit // 1024} KiB", flush=True)
    if len(image) * 4 > limit * 3:
        print(f"mkbootfs: warning: the boot image is over 75% of what `update` takes "
              f"({limit // 1024} KiB): the next big port needs room made first",
              file=sys.stderr)


if __name__ == "__main__":
    main()
