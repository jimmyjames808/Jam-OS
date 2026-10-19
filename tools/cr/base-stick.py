#!/usr/bin/env python3
"""The test stick for base_unittests (docs/CR-PLAN.md, track CB2;
tools/cr-test.sh's `base` case calls it), and with other lists for
content_shell (track CB4, the `shell-wl` case).

    base-stick.py [--data-mib N] [--programs a,b] [--program-dir DIR]
                  [--out-files a,b] [--source-files a,b] [--add DIR]
                  [--add-data DIR] <image> <stick> <stage> <source> <out>

<image> is the boot image (build/jamos.img). <stick> is written: a copy of
it with its data partition grown to N MiB (default 6144; the image's 64 MiB
holds no 77 MB program, and DriveInfoTest wants a volume of 5.12 GB or more:
sysfs counts 512-byte sectors, base reads the number as bytes) and formatted
again, as tools/bigprog-test.sh makes its stick, and then laid out the way
base_unittests finds its files. The stick is a sparse file: its apparent
size is the image's 256 MiB and N MiB, what is written to it (the image's
ESP, the volume's tables, the files, some 120 MB) is all that takes room
on the disk, and it must be copied sparsely (tools/cr/sparsecopy.py). <stage> is the ports builder's output for chromium
(~/cr/ports/chromium/stage), <source> the Chromium source tree and <out>
its out/jamos.

The data partition is what the image's is: a FAT volume (mformat and
mtools) or, for an image made with `make image DATA_FS=ext` (JAMOS_DATA_FS=ext
in the environment), an ext volume (build/host/jamext: mkfs at the grown
size, then one import of the whole tree, as tools/ports-preinstall.py
does). The files are the same either way.

The package format can't hold a file over 64 MiB yet (PO13), so the
programs don't go in as a package: they are put on /data directly, where
the shell approves them from a line written here, and the page table of
each is made here too (the kernel checks every page of a program running
from the pager against it: the shell's `allow` would have written both).
The files:

    /data/cr/out/jamos/base_unittests, test_child_process
                                        the programs; the test code finds
                                        the source root two directories up
                                        from the program's own (DIR_EXE)
    /data/cr/base/test/data/...         base's test files
    /data/cr/tools/metrics/...          three files the metrics tests read
    /data/cr/out/jamos/gen/base/...     two generated test files
    /data/cr/tmp/                       TMPDIR for the run (no /tmp yet)
    /data/etc/allow                     the programs' approval lines
    /data/etc/allow.d/<sha-256>         and their page tables

The lists are base_unittests' unless the options say others (cr-test.sh's
`shell-wl` case: content_shell, its pak and its fonts): --programs, the
programs (from <stage>/lib/chromium, or from --program-dir, e.g. <out>
itself, where a relink leaves them), each approved by its
ports/chromium/wants list; --out-files and --source-files, what goes under
/data/cr/out/jamos and /data/cr ("" for none); --add DIR, a tree put under
/data/cr as it is (fonts, a fonts.conf); --add-data DIR, a tree put under
/data as it is (the `fonts` case: pkg/share/fonts/..., the packages'
staged files where init's /pkg view shows them).

Runs on the build host (Python 3.12 there; mtools). Prints one line."""
import argparse
import hashlib
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import sparsecopy

TOP =os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PAGE = 4096
# From <dataprog.h> (and tools/portslib/pagetable.py, whose make() stops at
# 64 MiB): the table's 64-byte header and a SHA-256 for each page.
HEAD = struct.Struct("<8sIIQQ32s")

# The files of the source tree and of out/jamos that base_unittests reads
# (out/jamos/base_unittests.runtime_deps, less Chromium's test scripts and
# the fonts base's tests never open), as (where in <source> or <out>, the
# same under /data/cr).
FROM_SOURCE = ["base/test/data",
               "tools/metrics/actions/actions.xml",
               "tools/metrics/histograms/enums.xml",
               "tools/metrics/histograms/test_data/histograms.xml"]
FROM_OUT = ["gen/base/test/test_proto.descriptor",
            "gen/base/generated_file_for_test.txt"]
# The programs, with the ports/chromium/wants list each one runs by.
PROGRAMS = ["base_unittests", "test_child_process"]


class Error(Exception):
    pass


def mtool(*args):
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True)
    if r.returncode:
        raise Error("%s failed:\n%s" % (" ".join(args), r.stdout.strip()[-600:]))
    return r.stdout


def load_preinstall():
    """tools/ports-preinstall.py as a module: wants_text and ALLOW_HEAD,
    the words and the header of an approval line as the shell writes them."""
    spec = importlib.util.spec_from_file_location(
        "ports_preinstall", os.path.join(TOP, "tools", "ports-preinstall.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def page_table(path):
    """(the file's SHA-256 as hex, the page table's bytes)."""
    whole = hashlib.sha256()
    pages = []
    size = 0
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(PAGE), b""):
            whole.update(block)
            size += len(block)
            pages.append(hashlib.sha256(block + b"\0" * (PAGE - len(block))).digest())
    head = HEAD.pack(b"JAMPAGES", 1, PAGE, size, len(pages), whole.digest())
    return whole.hexdigest(), head + b"".join(pages)


JAMEXT = os.path.join(TOP, "build", "host", "jamext")


def data_fs(image):
    """"ext" or "fat": what the image's data partition is (mkimage.py --data-fs)."""
    r = subprocess.run([sys.executable, os.path.join(TOP, "tools", "mkimage.py"), "--data-fs", image],
                       capture_output=True, text=True)
    if r.returncode or r.stdout.strip() not in ("ext", "fat"):
        raise Error("%s: no data partition (%s)" % (image, (r.stderr or r.stdout).strip()))
    return r.stdout.strip()


def make_stick(image, stick, mib, fs):
    """The image grown to mib MiB of /data and its volume made again; the
    FAT volume's mtools name ("<stick>@@256M"), or for ext (made empty here
    and filled by jamext, which needs the stick, not a name) the stick."""
    at = subprocess.run([sys.executable, os.path.join(TOP, "tools", "mkimage.py"), "--data-at", image],
                        capture_output=True, text=True)
    if at.returncode:
        raise Error("%s: no data partition" % image)
    at = at.stdout.strip()   # "256M"
    start = int(at.rstrip("M"))
    # Only what comes before /data is copied (the image's own /data is made
    # again), and as a sparse file: /data is mostly never written (6 GiB for
    # DriveInfoTest), and a copy that wrote all of it would cost the PC's
    # disk that much for every boot (tools/cr/base-run.py copies the stick
    # the same way).
    sparsecopy.copy(image, stick, start << 20)
    subprocess.run([sys.executable, os.path.join(TOP, "tools", "mbr-grow.py"), stick,
                    str((start + mib) * 2048)], check=True, stdout=subprocess.DEVNULL)
    with open(stick, "r+b") as f:
        # An ext volume is made on a file that ends where /data starts (the
        # grow wiped a MiB past it): jamext then extends it and knows the
        # new blocks read zeros, so mkfs writes no zeros of its own.
        f.truncate((start if fs == "ext" else start + mib) << 20)
    if fs == "ext":
        # 2T: more than any MBR partition holds; jamext cuts it to the partition's
        # (tools/stick-data.sh's mkfs).
        mtool(JAMEXT, "-q", "--part", "2", "mkfs", "--label", "JAMOS-DATA", stick, "2T")
        return stick
    part = "%s@@%s" % (stick, at)
    mtool("mformat", "-i", part, "-T", str(mib * 2048), "-F", "-v", "JAMOS-DATA", "::")
    subprocess.run([sys.executable, os.path.join(TOP, "tools", "fat-label.py"), stick,
                    str(start), "--fix"], check=True, stdout=subprocess.DEVNULL)
    return part


def main(argv):
    ap = argparse.ArgumentParser(prog="base-stick.py", description=__doc__.split("\n\n")[0])
    ap.add_argument("--data-mib", type=int, default=6144)
    ap.add_argument("--programs", default=",".join(PROGRAMS))
    ap.add_argument("--program-dir")
    ap.add_argument("--out-files", default=",".join(FROM_OUT))
    ap.add_argument("--source-files", default=",".join(FROM_SOURCE))
    ap.add_argument("--add")
    ap.add_argument("--add-data")
    ap.add_argument("image")
    ap.add_argument("stick")
    ap.add_argument("stage")
    ap.add_argument("source")
    ap.add_argument("out")
    args = ap.parse_args(argv)
    programs, from_out, from_source = (
        [w for w in a.split(",") if w] for a in (args.programs, args.out_files, args.source_files))
    program_dir = args.program_dir or os.path.join(args.stage, "lib", "chromium")
    pre = load_preinstall()
    work = tempfile.mkdtemp(prefix="base-stick.")
    try:
        fs = data_fs(args.image)
        if fs == "ext" and not os.access(JAMEXT, os.X_OK):
            raise Error("%s's data partition is ext and there is no %s (make build/host/jamext)" %
                        (os.path.basename(args.image), os.path.relpath(JAMEXT, TOP)))
        cr = os.path.join(work, "cr")
        etc = os.path.join(work, "etc")
        os.makedirs(os.path.join(cr, "out", "jamos"))
        os.makedirs(os.path.join(etc, "allow.d"))
        # Jam OS has no /tmp yet: the tests' temporary files go here (TMPDIR).
        os.makedirs(os.path.join(cr, "tmp"))
        open(os.path.join(cr, "tmp", ".keep"), "w").close()
        allow = []
        for name in programs:
            src = os.path.join(program_dir, name)
            if not os.path.isfile(src):
                raise Error("%s: not built (tools/cr/build.sh start)" % src)
            dst = os.path.join(cr, "out", "jamos", name)
            shutil.copyfile(src, dst)
            os.chmod(dst, 0o755)
            digest, table = page_table(dst)
            with open(os.path.join(etc, "allow.d", digest), "wb") as f:
                f.write(table)
            wants = os.path.join(TOP, "ports", "chromium", "wants", name)
            lines = open(wants).read().splitlines() if os.path.isfile(wants) else ["posix"]
            allow.append("%s\t/data/cr/out/jamos/%s\t%s" % (digest, name, pre.wants_text(lines)))
        with open(os.path.join(etc, "allow"), "w") as f:
            f.write(pre.ALLOW_HEAD + "".join(a + "\n" for a in allow))
        for rel in from_source + from_out:
            top = args.out if rel in from_out else args.source
            src = os.path.join(top, rel)
            dst = os.path.join(cr, "out", "jamos", rel) if rel in from_out else os.path.join(cr, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.isdir(src):
                shutil.copytree(src, dst, symlinks=False, ignore_dangling_symlinks=True)
            elif os.path.isfile(src):
                shutil.copyfile(src, dst)
            else:
                raise Error("%s: missing" % src)
        if args.add:
            shutil.copytree(args.add, cr, dirs_exist_ok=True)
        part = make_stick(args.image, args.stick, args.data_mib, fs)
        if fs == "ext":
            # one import of the tree (jamext makes the directories; a program's
            # execute bit gives it mode 0755, a file's lack of it 0644)
            root = os.path.join(work, "root")
            os.makedirs(root)
            shutil.move(cr, os.path.join(root, "cr"))
            shutil.move(etc, os.path.join(root, "etc"))
            cr = os.path.join(root, "cr")
            if args.add_data:
                shutil.copytree(args.add_data, root, dirs_exist_ok=True)
            mtool(JAMEXT, "-q", "--part", "2", "import", args.stick, root)
        else:
            # one recursive copy each; mtools makes the directories
            tops = [cr, etc]
            if args.add_data:
                tops += [os.path.join(args.add_data, n) for n in sorted(os.listdir(args.add_data))]
            mtool("mcopy", "-s", "-m", "-i", part, *tops, "::/")
        # the programs read back, and compared with what the lines say
        for a in allow:
            digest, path, _ = a.split("\t")
            back = os.path.join(work, "back")
            if fs == "ext":
                mtool(JAMEXT, "-q", "--part", "2", "get", args.stick, path[len("/data"):], back)
            else:
                mtool("mcopy", "-n", "-i", part, "::" + path[len("/data"):], back)
            h = hashlib.sha256()
            with open(back, "rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    h.update(block)
            os.unlink(back)
            if h.hexdigest() != digest:
                raise Error("%s read back from the stick differs" % path)
        n = sum(len(names) for _, _, names in os.walk(cr))
        print("base-stick: %s: %d MiB of /data (%s), %d files under /data/cr, %d programs approved"
              % (os.path.basename(args.stick), args.data_mib, fs, n, len(allow)))
    except Error as e:
        print("base-stick: %s" % e, file=sys.stderr)
        return 1
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
