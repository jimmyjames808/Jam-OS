#!/usr/bin/env python3
"""Put built ports into a test image's data partition, from this side
(docs/M13.7-PLAN.md section 14, PO8; tools/ports-test.sh calls it).

    ports-preinstall.py closure <port>...
        the ports a test must have installed: the named ports and what
        they `runs`, one name per line, each after what it runs
    ports-preinstall.py install [options] <image> <port>...
        write that set into <image>'s data partition (the Makefile's
        layout: partition 2, from ESP_END_MIB); the ports must be built
        (tools/ports.py build) into --stage (default build/ports).
        --feed FILE: also write the tools/serial-feed.py lines that check
        the shell's `allow -l` lists every approval

What `install` writes, as `pkg` and init would have (section 9), for a test
image only:

    /data/pkg/<path>        every file of the package: the stage's `program`,
                            `file` and `licence` lines (a `-test` subpackage's
                            files are `file` lines too; `library` files are
                            for other builds and never go into a package)
    /data/pkg/db/<name>     the manifest, without a signature (`pkg check`
                            would refuse it: nothing signed this image)
    /data/etc/allow         one approval per `program`:
                            <sha-256>\\t/data/pkg/<path>\\t<what its list asks for>

Not written, because the pieces are not on main yet: the page-hash tables in
/data/etc/allow.d/ (M12.5's PX: until it lands the shell reads a program
whole and needs only the approval line), db/serial and db/journal (PO6).
Programs run from /data today as `run /data/pkg/bin/<name>`; /data/pkg/bin
is on no PATH yet (PO7).

After writing, every program is read back from the image and its SHA-256
compared with the stage's. Runs on the Mac's Python 3.9 and the test
host's 3.12; needs mtools (mformat's partner tools: mmd, mcopy)."""
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

from portslib import tree as treemod  # noqa: E402

ESP_END_MIB = 64   # the Makefile's: the data partition starts here
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


# ---- the stage list (tools/portslib/stage.py) --------------------------------------------

def read_stage(stage_dir, name):
    """{'port': [...], 'built-with': [...], 'files': [(kind, path, size, sha, list)]}"""
    path = os.path.join(stage_dir, name, "stage", "stage.txt")
    try:
        lines = open(path).read().splitlines()
    except OSError:
        raise Error("%s: not built (no %s): run tools/ports.py build %s" % (name, path, name))
    if not lines or lines[0] != "jamos-stage 1":
        raise Error("%s: not a jamos-stage 1 list" % path)
    out = {"port": None, "built-with": [], "files": [], "root": os.path.dirname(path)}
    for line in lines[1:]:
        w = line.split()
        if not w:
            continue
        if w[0] == "port":
            out["port"] = w[1:]
        elif w[0] == "built-with":
            out["built-with"].append(w[1:])
        elif w[0] in ("program", "file", "licence", "library"):
            out["files"].append((w[0], w[1], int(w[2]), w[3], w[4] if len(w) > 4 else None))
    if not out["port"]:
        raise Error("%s: no port line" % path)
    return out


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


def install(image, names, ports, stage_dir, offset_mib, feed=None):
    t = load_tree(ports)
    order = closure(t, names)
    part = "%s@@%dM" % (image, offset_mib)
    work = tempfile.mkdtemp(prefix="ports-preinstall.")
    try:
        pkg, etc = os.path.join(work, "pkg"), os.path.join(work, "etc")
        os.makedirs(os.path.join(pkg, "db"))
        os.makedirs(etc)
        allow, programs = [], []
        seen = {}   # path -> port: two packages must not own one file
        for name in order:
            r = t.recipes[name]
            if r.place != "package":
                raise Error("%s is a boot-image port (place %s): nothing to install" %
                            (name, r.place))
            st = read_stage(stage_dir, name)
            prefix = st["port"][4]
            if prefix != "/data/pkg":
                raise Error("%s: staged for %s, not /data/pkg" % (name, prefix))
            manifest = ["jamos-pkg 1", "name " + st["port"][0], "version " + st["port"][1],
                        "revision " + st["port"][2],
                        "# written by tools/ports-preinstall.py for a test image: no signature"]
            manifest += ["built-with " + " ".join(b) for b in st["built-with"]]
            progs = []
            for kind, rel, size, digest, lst in st["files"]:
                if kind == "library":
                    continue
                src = os.path.join(st["root"], rel)
                if not os.path.isfile(src) or sha256(src) != digest:
                    raise Error("%s: %s is not what stage.txt says (rebuild it)" % (name, rel))
                if rel in seen or rel.startswith("db/"):
                    raise Error("%s: %s is also %s's" % (name, rel, seen.get(rel, "db/")))
                seen[rel] = name
                dst = os.path.join(pkg, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(src, dst)
                mode = "755" if os.stat(src).st_mode & 0o111 else "644"
                manifest.append("file %s %s %d %s" % (rel, mode, size, digest))
                if kind == "program":
                    lines = ["posix"]
                    if lst and lst != "-":
                        lines = open(os.path.join(r.dir, lst)).read().splitlines()
                    text = wants_text(lines)
                    progs.append("program %s %s" % (rel, text))
                    allow.append("%s\t%s/%s\t%s" % (digest, prefix, rel, text))
                    programs.append((rel, digest))
            manifest += progs
            with open(os.path.join(pkg, "db", name), "w") as f:
                f.write("\n".join(manifest) + "\n")
        with open(os.path.join(etc, "allow"), "w") as f:
            f.write(ALLOW_HEAD + "".join(a + "\n" for a in allow))
        # one recursive copy each; mtools makes the directories
        mtool("mcopy", "-s", "-i", part, pkg, etc, "::/")
        pkg_dir = os.path.join(work, "back")
        os.makedirs(pkg_dir)
        for rel, digest in programs:
            back = os.path.join(pkg_dir, os.path.basename(rel))
            mtool("mcopy", "-n", "-i", part, "::/pkg/" + rel, back)
            if sha256(back) != digest:
                raise Error("%s: read back from the image, it differs" % rel)
            os.unlink(back)
        if feed:   # the serial script's proof that the shell reads the approvals
            with open(feed, "w") as f:
                f.write("# the approvals written into the image, as `allow -l` shows them\n")
                f.write("send allow -l\n")
                for line in allow:
                    digest, path, text = line.split("\t")
                    f.write("wait %s: %s (sha-256 %s\n" % (path, text, digest[:16]))
                f.write("wait {prompt}\n")
        names_done = ", ".join(order)
        print("ports-preinstall: %s: %d files, %d programs allowed (%s)" %
              (os.path.basename(image), len(seen), len(programs), names_done))
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
    i.add_argument("--offset-mib", type=int, default=ESP_END_MIB)
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
            install(args.image, args.port, args.ports, args.stage, args.offset_mib, args.feed)
    except Error as e:
        print("ports-preinstall: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
