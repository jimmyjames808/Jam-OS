"""ports.py's package, repository and format commands (docs/M13.7-PLAN.md
sections 6 to 8; the formats are the plan's "PO3 notes"):

    ports.py package [--key FILE | --test-key | --unsigned] [-f] [-v] [-j N]
                     [--pkgdir DIR] <port>...
        build the ports, then their packages, into build/ports/<name>/pkg/
        (or DIR), each checked by the reference verifier; prints each
        package's path, size and SHA-256
    ports.py package --from DIR [--key ...] [--pkgdir DIR] <port>
        the package of one port built elsewhere (Chromium, on the PC:
        docs/CR-PLAN.md section 15): DIR is that build's
        build/ports/<name>/ (its stage/, stamp and port.sha256, fetched
        back), packaged here, where the key is, without building; refused
        unless DIR was built from this very port directory (its
        port.sha256) and its stage is whole
    ports.py repo [--key FILE | --test-key] [--repo DIR] [--no-tree] [-j N] [<port>...]
        the repository in build/repo (or DIR): every package port (or the
        ones named, and what they run), the index, VULNS, the sources
    ports.py verify [--pub FILE] [--abi STAMP] [--last SERIAL] <file>...
        the reference verifier on a .jpk, an INDEX or a VULNS
    ports.py vectors [--write]
        the formats' test vectors in ports/vectors: made afresh and
        compared with the committed ones, each checked as CASES says
        (make check); --write writes them
    ports.py pkgfuzz DIR
        mutated texts and archives from the vectors, with the reference
        reader's answers, in DIR, for the C readers (tools/pkgcheck.c;
        make check)
    ports.py abi
        the ABI stamp of this tree

--key defaults to JAMOS_PORTS_KEY or ~/.config/jamos/ports.key, the
ports key (`build/host/jamos-sign keygen --ports` makes it once, outside
the repository); --pub to the ports.pub beside it. --test-key signs with
the self-test's throwaway key (sign.test_key: its seed is public, so two
machines make the same bytes; no image carries its public half): for
comparing builds, never for packages to install.
"""
import hashlib
import os
import sys
import tempfile

from . import build as buildmod
from . import package as packagemod
from . import pkgformat as fmt
from . import pkgverify
from . import repo as repomod
from . import sign as signmod
from . import tree as treemod


def add_parsers(sub):
    p = sub.add_parser("package")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--key")
    g.add_argument("--test-key", action="store_true")
    g.add_argument("--unsigned", action="store_true")
    p.add_argument("--pkgdir")
    p.add_argument("--from", dest="from_dir", metavar="DIR")
    p.add_argument("-f", "--force", action="store_true")
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("-j", "--jobs", type=int)
    p.add_argument("port", nargs="+")
    p = sub.add_parser("repo")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--key")
    g.add_argument("--test-key", action="store_true")
    p.add_argument("--repo")
    p.add_argument("--no-tree", action="store_true")
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("-j", "--jobs", type=int)
    p.add_argument("port", nargs="*")
    p = sub.add_parser("verify")
    p.add_argument("--pub")
    p.add_argument("--abi")
    p.add_argument("--last", type=int)
    p.add_argument("file", nargs="+")
    sub.add_parser("vectors").add_argument("--write", action="store_true")
    sub.add_parser("pkgfuzz").add_argument("dir")
    sub.add_parser("abi")


def commands(top):
    def cmd_package(args):
        return package(top, args)

    def cmd_repo(args):
        return repo(top, args)

    def cmd_vectors(args):
        from . import vectors
        return vectors.main(top, args.ports, args.write)

    def cmd_pkgfuzz(args):
        from . import pkgfuzz
        return pkgfuzz.main(top, args.ports, os.path.abspath(args.dir))

    return {"package": cmd_package, "repo": cmd_repo, "verify": verify, "vectors": cmd_vectors,
            "pkgfuzz": cmd_pkgfuzz, "abi": lambda args: print(fmt.abi_stamp(top)) or 0}


def _tree(args):
    t, problems = treemod.load(args.ports)
    names = getattr(args, "port", None) or []
    problems += ["no port %s" % n for n in names if n not in t.recipes]
    if not problems:
        problems = treemod.lint(t, sorted(t.recipes))
    for p in problems:
        print("ports: " + p)
    return None if problems else t


def _key(args, d):
    """The secret key file the command signs with (None: unsigned)."""
    if getattr(args, "unsigned", False):
        return None
    if args.test_key:
        return signmod.test_key(d)[0]
    return args.key or signmod.default_key()


def from_stage(r, d):
    """The stage of r built elsewhere, in d (that build's build/ports/<name>/):
    PackageError unless it was built from this very port directory and its
    stage list is there (each file's bytes are checked against it as the
    package is made)."""
    try:
        with open(os.path.join(d, "port.sha256")) as f:
            theirs = f.read().strip()
    except OSError:
        raise packagemod.PackageError("%s: no port.sha256 in %s (not a port's build directory)"
                                      % (r.name, d))
    if theirs != buildmod.port_hash(r):
        raise packagemod.PackageError(
            "%s: %s was built from another recipe (its port directory hashed %s, this one %s)"
            % (r.name, d, theirs[:16], buildmod.port_hash(r)[:16]))
    stage = os.path.join(d, "stage")
    if not os.path.isfile(os.path.join(stage, "stage.txt")):
        raise packagemod.PackageError("%s: no stage/stage.txt in %s" % (r.name, d))
    return stage


def package(top, args):
    t = _tree(args)
    if t is None:
        return 1
    b = buildmod.Builder(t, top, args.out, args.jobs, args.port if args.force else (),
                         args.verbose)
    if args.from_dir and len(args.port) != 1:
        print("ports: package --from takes one port")
        return 2
    try:
        stages = {}
        if args.from_dir:
            n = args.port[0]
            stages[n] = from_stage(t.recipes[n], os.path.abspath(args.from_dir))
        else:
            b.build(args.port)
        with tempfile.TemporaryDirectory() as d:
            key = _key(args, d)
            pub = pkgverify.UNCHECKED
            if key:
                pub = os.path.join(d, "ports.pub")
                signmod.public(key, pub)
            for n in args.port:
                out = args.pkgdir or os.path.join(b.base(n), "pkg")
                stage = stages.get(n) or os.path.join(b.base(n), "stage")
                for name, path in packagemod.make(t.recipes[n], stage,
                                                  out, fmt.abi_stamp(top), key):
                    with open(path, "rb") as f:
                        data = f.read()
                    pkgverify.verify_package(data, pub, fmt.abi_stamp(top))
                    print("%s %d %s" % (os.path.relpath(path), len(data),
                                        hashlib.sha256(data).hexdigest()))
    except (buildmod.BuildError, packagemod.PackageError, signmod.SignError,
            pkgverify.Refused) as e:
        print("ports: %s" % e)
        return 1
    return 0


def repo(top, args):
    t = _tree(args)
    if t is None:
        return 1
    b = buildmod.Builder(t, top, args.out, args.jobs, (), args.verbose)
    repodir = args.repo or os.path.join(top, "build", "repo")
    try:
        with tempfile.TemporaryDirectory() as d:
            repomod.make(t, b, top, repodir, _key(args, d), args.port or None, not args.no_tree)
    except (buildmod.BuildError, packagemod.PackageError, signmod.SignError,
            repomod.RepoError, pkgverify.Refused) as e:
        print("ports: %s" % e)
        return 1
    return 0


def verify(args):
    pub = args.pub or os.path.join(os.path.dirname(signmod.default_key()), "ports.pub")
    bad = 0
    for path in args.file:
        with open(path, "rb") as f:
            data = f.read()
        try:
            if data.startswith(fmt.INDEX_FIRST.encode() + b"\n"):
                x = pkgverify.verify_index(data, pub, (args.last, "") if args.last else None)
                what = "an index, serial %d, %d packages" % (x.serial, len(x.packages))
            elif data.startswith(fmt.VULNS_FIRST.encode() + b"\n"):
                v = pkgverify.verify_vulns(data, pub, (args.last, "") if args.last else None)
                what = "a vulnerability list, serial %d, %d entries" % (v.serial, len(v.entries))
            else:
                m = pkgverify.verify_package(data, pub, args.abi)
                what = "the package %s %s r%d, %d files, %d programs" % (
                    m.name, m.version, m.revision, len(m.files), len(m.programs))
            print("%s: good: %s" % (path, what))
        except pkgverify.Refused as e:
            print("%s: REFUSED: %s" % (path, e))
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit("run tools/ports.py")
