#!/usr/bin/env python3
"""Pack files into a bootfs image (format: kernel/include/jam/bootfs.h).

    mkbootfs.py [--esp-mib N --kernel K [--esp-file F]...] <out.img> (<name>=<file> | @<list>) ...
    mkbootfs.py --esp-check <disk.img> <kernel> <bootfs>
    mkbootfs.py --selftest      the list files and the size limits (make check)

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

The image's size is printed against its limit, and an image over it is an
error (the output is removed), one over 75% of it a warning. The limit is
the smaller of two:
  - `update`'s: it refuses a file over UPDATE_FILE_MAX
    (user/include/update.h);
  - the ESP's third, with --esp-mib (the ESP's size: the Makefile's
    ESP_END_MIB - 1) and --kernel (the kernel the ESP gets): `update` keeps
    three builds on the ESP while it writes one (the stick's, the previous
    one, the new one: user/services/init/espwrite.c), each file with
    espwrite.c's SLACK spare, beside what else is there (--esp-file: Limine,
    the boot menu, and the menu's three other copies an update makes). The
    FAT's own room is reckoned as for 512-byte clusters, the most it can be.
--esp-check is the image's own check, once its files are in (the Makefile's
image rule): its ESP's free space, from mtools, must hold two more builds
of this size (the previous one and a new one), each with SLACK, and the
menu's copies: what `update` checks on the stick (espwrite.c's ROOM)."""
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


def esp_slack():
    """SLACK, the room espwrite.c keeps spare per file on the ESP."""
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "user", "services", "init", "espwrite.c")
    with open(path) as f:
        m = re.search(r"#define\s+SLACK\s+\((\d+)u?\s*<<\s*(\d+)\)", f.read())
    if not m:
        sys.exit(f"mkbootfs: SLACK is not in {path} in the form (N << M)")
    return int(m.group(1)) << int(m.group(2))


def menu_max():
    """UPDATE_MENU_MAX, the largest boot menu `update` writes."""
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "user", "include", "update.h")
    with open(path) as f:
        m = re.search(r"#define\s+UPDATE_MENU_MAX\s+\((\d+)u?\s*<<\s*(\d+)\)", f.read())
    if not m:
        sys.exit(f"mkbootfs: UPDATE_MENU_MAX is not in {path} in the form (N << M)")
    return int(m.group(1)) << int(m.group(2))


MIB = 1 << 20
MENU_COPIES = 3   # limine.conf.new, .prev and the spare boot/limine.conf (espmenu.c)


def esp_budget(esp_mib, kernel_bytes, other_bytes):
    """The largest boot image three builds of which (with a kernel of
    kernel_bytes) fit an ESP of esp_mib MiB beside other_bytes of other
    files, as `update` reckons room: (budget, the ESP's third)."""
    esp = esp_mib * MIB
    fat = esp // 64 + MIB              # two FATs of 4-byte entries per 512-byte cluster, and more
    room = esp - fat - other_bytes - MENU_COPIES * menu_max()
    third = room // 3
    return third - kernel_bytes - 2 * esp_slack(), third


def esp_free(image):
    """The free bytes of the image's ESP (mtools: partition 1 at 1 MiB)."""
    import subprocess
    p = subprocess.run(["mdir", "-i", image + "@@1M", "::/"], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, text=True)
    m = re.search(r"([\d ]+) bytes free", p.stdout)
    if p.returncode != 0 or not m:
        sys.exit(f"mkbootfs: can't read {image}'s ESP with mdir: {p.stderr.strip()}")
    return int(m.group(1).replace(" ", ""))


def esp_check(image, kernel, bootfs):
    """The image's ESP, its build in, has room for update's two more
    builds of this size; 0 if so."""
    build = os.path.getsize(kernel) + os.path.getsize(bootfs) + 2 * esp_slack()
    need, free = 2 * build + MENU_COPIES * menu_max(), esp_free(image)
    if free < need:
        print(f"mkbootfs: error: {image}'s ESP has {free // 1024} KiB free with this build on it, "
              f"and `update` needs {need // 1024} KiB more to write the next one beside it and "
              f"the previous one (three builds of {build // 1024} KiB): make the ESP bigger "
              f"(ESP_END_MIB) or the build smaller", file=sys.stderr)
        return 1
    print(f"mkbootfs: {image}: the ESP holds three builds of this size "
          f"({free // 1024} KiB free, {need // 1024} KiB needed for the next two)")
    return 0


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
        # The ESP's third: a 48 MiB ESP with a 2 MiB kernel and Limine.
        kernel, limine = make("k", 2 * MIB, 5), make("l", 300 * 1024, 6)
        budget, _ = esp_budget(48, 2 * MIB, 320 * 1024)
        esp = ["--esp-mib", "48", "--kernel", kernel, "--esp-file", limine]
        if not 8 * MIB < budget < 14 * MIB:
            fails.append(f"a 48 MiB ESP's budget is {budget // 1024} KiB, not about 11 MiB")
        rc, text = run(*esp, img, f"bin/z={make('f', budget - 4096, 7)}")
        if rc != 0 or "ESP's third" not in text:
            fails.append(f"a boot image inside the ESP's third refused: rc {rc}: {text.strip()}")
        rc, text = run(*esp, img, f"bin/z={make('g', budget + 1, 8)}")
        if rc == 0 or "three builds" not in text or os.path.exists(img):
            fails.append(f"a boot image past the ESP's third taken: rc {rc}: {text.strip()}")
        rc, text = run("--esp-mib", "1024", "--kernel", kernel, img,
                       f"bin/z={make('h', limit, 9)}")
        if rc == 0 or "UPDATE_FILE_MAX" not in text:
            fails.append(f"a big ESP lifted update's limit: rc {rc}: {text.strip()}")
        # --esp-check on a real image (mtools): room for two more, then not.
        disk = os.path.join(tmp, "disk.img")
        mkimage = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mkimage.py")
        bootfs = make("b", 6 * MIB, 10)
        made = subprocess.run([sys.executable, mkimage, disk, "32", "40"]).returncode == 0 and \
            subprocess.run(["mformat", "-i", disk + "@@1M", "-T", str(31 * 2048), "-F", "::"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        if not made:
            fails.append("can't make a test disk image with mtools")
        else:
            subprocess.run(["mcopy", "-i", disk + "@@1M", kernel, bootfs, "::/"], check=True)
            rc, text = run("--esp-check", disk, kernel, bootfs)   # 8 MiB builds: 16+ of 23 free
            if rc != 0:
                fails.append(f"--esp-check refused room for two more: {text.strip()}")
            big = make("c", 12 * MIB, 11)
            subprocess.run(["mcopy", "-o", "-i", disk + "@@1M", big, "::/b"], check=True)
            rc, text = run("--esp-check", disk, kernel, big)    # 14 MiB builds: 28 of 17 free
            if rc == 0 or "three builds" not in text:
                fails.append(f"--esp-check took a full ESP: rc {rc}: {text.strip()}")
    for f in fails:
        print("mkbootfs --selftest: " + f)
    print("mkbootfs --selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


def main():
    if sys.argv[1:] == ["--selftest"]:
        sys.exit(selftest())
    if sys.argv[1:2] == ["--esp-check"]:
        if len(sys.argv) != 5:
            sys.exit(__doc__)
        sys.exit(esp_check(*sys.argv[2:]))
    args, esp_mib, kernel, esp_files = sys.argv[1:], None, None, []
    while args and args[0] in ("--esp-mib", "--kernel", "--esp-file") and len(args) > 1:
        opt, val, args = args[0], args[1], args[2:]
        if opt == "--esp-mib":
            esp_mib = int(val)
        elif opt == "--kernel":
            kernel = val
        else:
            esp_files.append(val)
    if len(args) < 2 or (esp_mib is None) != (kernel is None):
        sys.exit(__doc__)
    out, files = args[0], {}
    for arg in read_args(args[1:]):
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
    what = f"`update` refuses a file over {limit // 1024} KiB (UPDATE_FILE_MAX, " \
           f"user/include/update.h)"
    short = "update's"
    if esp_mib is not None:
        other = sum((os.path.getsize(f) + 0xffff) & ~0xffff for f in esp_files)
        budget, third = esp_budget(esp_mib, os.path.getsize(kernel), other)
        if budget < limit:
            limit = max(budget, 0)
            what = (f"`update` keeps three builds on the ESP while it writes one, and a third "
                    f"of this {esp_mib} MiB ESP ({third // 1024} KiB) leaves {limit // 1024} KiB "
                    f"for the boot image beside the kernel ({os.path.getsize(kernel) // 1024} "
                    f"KiB; ESP_END_MIB in the Makefile, espwrite.c)")
            short = f"the {esp_mib} MiB ESP's third's"
    if len(image) > limit:
        if os.path.exists(out):
            os.remove(out)   # no stale image to be flashed after the error
        sys.exit(f"mkbootfs: error: {out} would be {len(image) // 1024} KiB, and {what}: "
                 f"take something out of the boot image")
    with open(out, "wb") as f:
        f.write(image)
    print(f"mkbootfs: {out}: {len(names)} files, {len(image) // 1024} KiB, "
          f"{len(image) * 100 // limit}% of {short} {limit // 1024} KiB", flush=True)
    if len(image) * 4 > limit * 3:
        print(f"mkbootfs: warning: the boot image is over 75% of what it may be "
              f"({limit // 1024} KiB: {what}): the next big port needs room made first",
              file=sys.stderr)


if __name__ == "__main__":
    main()
