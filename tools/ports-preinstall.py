#!/usr/bin/env python3
"""Put built ports into a test image's data partition, from this side
(docs/M13.7-PLAN.md section 14, PO8; tools/ports-test.sh calls it).

    ports-preinstall.py closure <port>...
        the ports a test must have installed: the named ports and what
        they `runs`, one name per line, each after what it runs
    ports-preinstall.py install [options] <image> <port>...
        write that set into <image>'s data partition (the Makefile's
        layout: MBR partition 2, wherever the image's MBR says it starts,
        or --offset-mib: a FAT volume, mtools; an ext one, `make image
        DATA_FS=ext`'s, build/host/jamext import --part 2); the ports must be built
        (tools/ports.py build) into --stage (default build/ports).
        --feed FILE: also write the tools/serial-feed.py lines that check
        the shell's `allow -l` lists every approval

What `install` writes, as `pkg` and init would have (section 9), for a test
image only. The packages are the ones `ports.py package` makes (the port's
own, and one for each `subpackage`: a `-test` subpackage's files are in
`<port>-test`; `library` files are for other builds and never go into a
package), each with a signed manifest:

    /data/pkg/<path>        every file of every package
    /data/pkg/db/<name>     the package's manifest, signed with the ports
                            vectors' test key (--key: another), its `abi` the
                            tree's stamp (tools/ports.py abi), so `pkg list`,
                            `pkg info` and `pkg check` on the image see an
                            installed package, as long as the image's
                            /boot/ports.pub is test.pub (tools/ports-test.sh
                            puts it there)
    /data/etc/allow         one approval per `program`, tagged with its package:
                            <sha-256>\\t/data/pkg/<path>\\t<what its list asks for>\\tpkg=<name> <version>-r<rev>
    /data/etc/allow.d/<sha> the program's page-hash table (the manifest's `table` line)

Not written: db/serial and db/journal (the first `pkg update` makes the
serial; nothing is in the middle of a change).

After writing, every program is read back from the image and its SHA-256
compared with the stage's. Runs on the Mac's Python 3.9 and the test
host's 3.12; needs mtools (mformat's partner tools: mmd, mcopy) for a FAT
data partition, build/host/jamext (`make build/host/jamext`) for an ext one."""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(TOP, "tools"))
sys.dont_write_bytecode = True

from portslib import package as packagemod  # noqa: E402
from portslib import pagetable  # noqa: E402
from portslib import pkgformat as fmt  # noqa: E402
from portslib import sign as signmod  # noqa: E402
from portslib import tree as treemod  # noqa: E402



def data_mib(image):
    """Where the image's data partition starts, in MiB (its MBR's entry 2:
    the Makefile's ESP_END_MIB; tools/mkimage.py --data-at)."""
    r = subprocess.run([sys.executable, os.path.join(TOP, "tools", "mkimage.py"), "--data-at",
                        image], capture_output=True, text=True)
    if r.returncode != 0:
        raise Error(r.stderr.strip() or "%s: no data partition" % image)
    return int(r.stdout.strip().rstrip("M"))

ALLOW_HEAD = ("# Programs on /data the owner allowed to run (the shell's `allow`):\n"
              "# <sha-256>\\t<path>\\t<what it asked for>\n")


class Error(Exception):
    pass


def load_tree(ports):
    t, problems = treemod.load(ports)
    if problems:
        raise Error("\n".join(problems))
    return t


def closure(t, names):
    try:
        return treemod.order(t, names, ("runs",))
    except KeyError as e:
        raise Error("no port %s" % e)
    except treemod.CycleError as e:
        raise Error(str(e))


# ---- what a list asks for, as the owner is shown it (user/lib/wants.c) ------------------

def wants_text(lines):
    """The text <wants.h> makes of a list: the same words `allow` shows."""
    shown = []
    for line in lines:
        w = line.split("#")[0].split()
        if not w or w == ["posix"]:
            continue
        if w[0] == "mount" and len(w) == 3:
            shown.append("%s (%s)" % ("every mount" if w[1] == "*" else w[1],
                                      "read" if w[2] == "r" else "write"))
        elif w[:3] == ["svc", "net", "listen"] and len(w) == 3:
            shown += ["net", "accepting connections from the network"]
        elif w[0] == "svc" and len(w) == 2:
            shown.append(w[1])
        elif w[0] == "right" and len(w) == 2:
            shown.append("the kernel's " + w[1])
        else:
            raise Error("a list line this tool doesn't know: %r" % line)
    return ", ".join(shown) if shown else "nothing"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


# ---- mtools ----------------------------------------------------------------------------

def mtool(*args):
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True)
    if r.returncode:
        raise Error("%s failed:\n%s" % (" ".join(args), r.stdout.strip()))
    return r.stdout


def data_is_ext(image):
    """Is the image's data partition an ext volume (MBR type 0x83;
    tools/mkimage.py --data-fs) and not a FAT one?"""
    r = subprocess.run([sys.executable, os.path.join(TOP, "tools", "mkimage.py"), "--data-fs",
                        image], capture_output=True, text=True)
    if r.returncode != 0:
        raise Error(r.stderr.strip() or "%s: no data partition" % image)
    return r.stdout.strip() == "ext"


def install(image, names, ports, stage_dir, offset_mib, key=None, feed=None):
    t = load_tree(ports)
    order = closure(t, names)
    abi = fmt.abi_stamp(TOP)
    part = "%s@@%dM" % (image, offset_mib)
    ext = data_is_ext(image)
    jamext = os.path.join(TOP, "build", "host", "jamext")
    if ext and not os.access(jamext, os.X_OK):
        raise Error("%s's data partition is ext and there is no %s (make build/host/jamext)" %
                    (os.path.basename(image), os.path.relpath(jamext, TOP)))
    work = tempfile.mkdtemp(prefix="ports-preinstall.")
    try:
        if not key:   # the ports vectors' key: its public half is ports/vectors/test.pub
            key, _pub = signmod.test_key(os.path.join(work, "keys"), "ports vectors")
        root = os.path.join(work, "root")   # what goes into /data: pkg and etc
        pkg, etc = os.path.join(root, "pkg"), os.path.join(root, "etc")
        tables = os.path.join(etc, "allow.d")
        os.makedirs(os.path.join(pkg, "db"))
        os.makedirs(tables)
        allow, programs, packages = [], [], []
        seen = {}   # path -> package: two packages must not own one file
        for name in order:
            r = t.recipes[name]
            if r.place != "package":
                raise Error("%s is a boot-image port (place %s): nothing to install" %
                            (name, r.place))
            stage = os.path.join(stage_dir, name, "stage")
            if not os.path.isfile(os.path.join(stage, "stage.txt")):
                raise Error("%s: not built (no %s/stage.txt): run tools/ports.py build %s" %
                            (name, stage, name))
            try:
                pkgs = packagemod.plan(r, stage)
                problems = ["%s: %s" % (p.name, x) for p in pkgs for x in packagemod.check(p)]
                if problems:
                    raise Error("the format can't hold it:\n  " + "\n  ".join(problems))
                texts = [signmod.sign(packagemod.manifest(p, abi), fmt.PKG_FIRST, key)
                         for p in pkgs]
            except (packagemod.PackageError, signmod.SignError) as e:
                raise Error("%s: %s" % (name, e))
            for p, signed in zip(pkgs, texts):
                tag = "pkg=%s %s-r%d" % (p.name, p.version, p.revision)
                for rel in sorted(p.files):
                    if rel in seen or rel.startswith("db/"):
                        raise Error("%s: %s is also %s's" % (p.name, rel, seen.get(rel, "db/")))
                    seen[rel] = p.name
                    data = p.files[rel][1]
                    dst = os.path.join(pkg, rel)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    with open(dst, "wb") as f:
                        f.write(data)
                    os.chmod(dst, 0o755 if p.files[rel][0] & 0o111 else 0o644)   # jamext import's 0755
                    if rel in p.lists:
                        digest = hashlib.sha256(data).hexdigest()
                        text = wants_text(p.lists[rel].decode().splitlines())
                        allow.append("%s\t/data/pkg/%s\t%s\t%s" % (digest, rel, text, tag))
                        programs.append((rel, digest))
                        with open(os.path.join(tables, digest), "wb") as f:
                            f.write(pagetable.make(data))
                with open(os.path.join(pkg, "db", p.name), "wb") as f:
                    f.write(signed)
                packages.append("%s %s-r%d" % (p.name, p.version, p.revision))
        with open(os.path.join(etc, "allow"), "w") as f:
            f.write(ALLOW_HEAD + "".join(a + "\n" for a in allow))
        if ext:   # one import of the tree (jamext makes the directories, the volume stays clean)
            mtool(jamext, "-q", "--part", "2", "import", image, root)
        else:     # one recursive copy each; mtools makes the directories
            mtool("mcopy", "-s", "-i", part, pkg, etc, "::/")
        pkg_dir = os.path.join(work, "back")
        os.makedirs(pkg_dir)
        for rel, digest in programs:
            back = os.path.join(pkg_dir, os.path.basename(rel))
            if ext:
                mtool(jamext, "-q", "--part", "2", "get", image, "/pkg/" + rel, back)
            else:
                mtool("mcopy", "-n", "-i", part, "::/pkg/" + rel, back)
            if sha256(back) != digest:
                raise Error("%s: read back from the image, it differs" % rel)
            os.unlink(back)
        if feed:   # the serial script's proof that the shell reads the approvals
            with open(feed, "w") as f:
                f.write("# the approvals written into the image, as `allow -l` shows them\n")
                f.write("send allow -l\n")
                for line in allow:
                    digest, path, text, tag = line.split("\t")   # shown: `<text> [package <tag>]`
                    f.write("wait %s: %s [package %s] (sha-256 %s\n" %
                            (path, text, tag[len("pkg="):], digest[:16]))
                f.write("wait {prompt}\n")
        print("ports-preinstall: %s: %d files, %d programs allowed (%s)" %
              (os.path.basename(image), len(seen), len(programs), ", ".join(packages)))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main(argv):
    ap = argparse.ArgumentParser(prog="ports-preinstall.py", description=__doc__.split("\n\n")[0])
    ap.add_argument("--ports", default=os.path.join(TOP, "ports"))
    ap.add_argument("--stage", default=os.path.join(TOP, "build", "ports"),
                    help="the builder's output directory (build/ports)")
    sub = ap.add_subparsers(dest="command")
    sub.add_parser("closure").add_argument("port", nargs="+")
    i = sub.add_parser("install")
    i.add_argument("--offset-mib", type=int, help="default: from the image's MBR")
    i.add_argument("--key", metavar="FILE",
                   help="the secret key that signs the manifests (default: the ports vectors' "
                        "test key, whose public half tools/ports-test.sh puts in the image "
                        "as /boot/ports.pub)")
    i.add_argument("--feed", metavar="FILE",
                   help="write the serial script lines that check `allow -l` shows the approvals")
    i.add_argument("image")
    i.add_argument("port", nargs="+")
    args = ap.parse_args(argv)
    if not args.command:
        ap.print_help()
        return 2
    try:
        if args.command == "closure":
            print("\n".join(closure(load_tree(args.ports), args.port)))
        else:
            if not os.path.isfile(args.image):
                raise Error("%s: no such image" % args.image)
            offset = args.offset_mib if args.offset_mib is not None else data_mib(args.image)
            install(args.image, args.port, args.ports, args.stage, offset, args.key, args.feed)
    except Error as e:
        print("ports-preinstall: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
