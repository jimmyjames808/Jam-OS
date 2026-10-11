"""Packages from a built port's stage (docs/M13.7-PLAN.md section 6; the
format in full is the plan's "PO3 notes"): `ports.py package`.

A port's stage (build/ports/<name>/stage/, its list stage.txt) gives one
package, `<name>`: its programs, its `files` and its licence texts, and
one more package per `subpackage` line, `<name>-<what>`: the files the
line named and the licence texts again (each package carries its own, in
share/licences/<package>/). Libraries are never packaged (static linking:
they are build-time only before M15), so a port that is only a library
makes no package. A `multicall` name of a program is a copy of it beside
it (FAT32 has no links): its own file, program and table lines.

A package is a ustar archive (ustar.py's one canonical form) of:

    +MANIFEST                 the signed manifest
    <path>...                 the files, in the manifest's order
    +TABLES/<program path>... each program's page-hash table (pagetable.py)

Its bytes depend only on the stage, the recipe and the ABI stamp:
members sorted, every time 0, no owner, and an Ed25519 signature (the
same key and bytes always give the same signature).
"""
import hashlib
import os
import sys

from . import pagetable
from . import pkgformat as fmt
from . import sign as signmod
from . import stage as stagemod
from . import ustar

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import checkwants  # noqa: E402


class PackageError(Exception):
    pass


class Package:
    """One package to write: what its manifest says and its files' bytes."""

    def __init__(self, name, r):
        self.name = name          # the package's name (the port's, or a subpackage's)
        self.port = r.name        # the port it comes from
        self.version = r.version
        self.revision = r.revision
        self.licence = r.get("licence")
        self.summary = r.get("summary")
        self.runs = []            # [(name, minimum version or None)]
        self.built_with = []      # [[what, version, revision?]]
        self.files = {}           # path -> (mode, bytes)
        self.lists = {}           # program path -> its list (bytes)

    def add(self, path, mode, data):
        if path in self.files:
            raise PackageError("%s: %s twice" % (self.name, path))
        self.files[path] = (mode, data)


def _read_stage(stage):
    """The stage list's lines, each file's bytes checked against its line."""
    lines = stagemod.read_list(os.path.join(stage, "stage.txt"))
    data = {}
    for w in lines:
        if w[0] in ("program", "file", "licence", "library"):
            with open(os.path.join(stage, w[1]), "rb") as f:
                b = f.read()
            if len(b) != int(w[2]) or hashlib.sha256(b).hexdigest() != w[3]:
                raise PackageError("%s in the stage isn't the file its stage.txt line names: "
                                   "build the port again" % w[1])
            data[w[1]] = b
    return lines, data


def _list_of(path, data):
    """A program's list: the text of its one JamOS note."""
    try:
        texts = checkwants.notes(data)
    except (ValueError, IndexError) as e:
        raise PackageError("program %s: %s" % (path, e))
    if len(texts) != 1:
        raise PackageError("program %s: %d lists in it (want one)" % (path, len(texts)))
    return texts[0]


def plan(r, stage):
    """The packages a port's stage makes: [Package], the main one first
    (when it has anything but licences), then the subpackages."""
    if r.place != "package":
        raise PackageError("%s: a %s port is not packaged (it goes into the boot image)" %
                           (r.name, r.place))
    lines, data = _read_stage(stage)
    if lines[0][:4] != ["port", r.name, r.version, str(r.revision)]:
        raise PackageError("%s: the stage is of %s, not of the recipe" %
                           (r.name, " ".join(lines[0][1:4])))
    sub_of = {w[2]: w[1] for w in lines if w[0] == "sub"}
    main = Package(r.name, r)
    subs = {w[0]: Package(w[0], r) for w in r.lines("subpackage")}
    licences = [w[1] for w in lines if w[0] == "licence"]
    for w in lines:
        if w[0] == "program":
            main.add(w[1], 0o755, data[w[1]])
            main.lists[w[1]] = _list_of(w[1], data[w[1]])
        elif w[0] == "file":
            (subs[sub_of[w[1]]] if w[1] in sub_of else main).add(w[1], 0o644, data[w[1]])
    _multicall(r, main)
    main.runs = sorted(r.deps("runs"), key=lambda d: d[0].encode())
    if main.lists:
        main.built_with = sorted(w[1:] for w in lines if w[0] == "built-with")
    out = [main] if main.files else []
    for name in sorted(subs):
        p = subs[name]
        p.summary = "%s (%s)" % (r.get("summary"), name)
        out.append(p)
    for p in out:
        for lic in licences:
            p.add("share/licences/%s/%s" % (p.name, os.path.basename(lic)), 0o644, data[lic])
    return out


def _multicall(r, main):
    """Each multicall name as a copy of its program, beside it."""
    for w in r.lines("multicall"):
        if w[0] not in main.lists:
            raise PackageError("multicall %s: not a staged program" % w[0])
        mode, data = main.files[w[0]]
        for n in w[1:]:
            path = os.path.join(os.path.dirname(w[0]), n)
            main.add(path, mode, data)
            main.lists[path] = main.lists[w[0]]


def check(p):
    """What the format can't hold in p (the reader's rules, said early)."""
    problems = []
    if not p.licence or not fmt.LICENCE.match(p.licence) or len(p.licence) > fmt.TEXT_MAX:
        problems.append("the licence %r is no licence line" % p.licence)
    if not p.summary or not fmt.text_ok(p.summary):
        problems.append("the summary must be 1..%d bytes of printable ASCII" % fmt.TEXT_MAX)
    for path in sorted(p.files):
        why = fmt.path_problem(path)
        if why:
            problems.append("%s: %s" % (path, why))
        else:
            for member in [path] + ["+TABLES/" + path] * (path in p.lists):
                try:
                    ustar.split(member)
                except ustar.UstarError as e:
                    problems.append(str(e))
    why = fmt.clash_problem(list(p.files))
    if why:
        problems.append(why)
    sizes = [len(d) for _, d in p.files.values()]
    if not 0 < len(p.files) <= fmt.FILES_MAX or len(p.lists) > fmt.PROGRAMS_MAX:
        problems.append("%d files, %d programs (1..%d, at most %d)" %
                        (len(p.files), len(p.lists), fmt.FILES_MAX, fmt.PROGRAMS_MAX))
    if max(sizes, default=0) > fmt.FILE_MAX or sum(sizes) > fmt.FILES_TOTAL_MAX:
        problems.append("a file over %d bytes, or all over %d" %
                        (fmt.FILE_MAX, fmt.FILES_TOTAL_MAX))
    if len(p.runs) > fmt.DEPS_MAX or len(p.built_with) > fmt.DEPS_MAX:
        problems.append("more than %d runs or built-with lines" % fmt.DEPS_MAX)
    for path, text in p.lists.items():
        try:
            fmt.encode_list(text)
        except ValueError as e:
            problems.append("program %s: %s" % (path, e))
        if len(p.files[path][1]) == 0:
            problems.append("program %s: empty" % path)
    return problems


def manifest(p, abi):
    """p's manifest, its last line a bare "signature" (bytes)."""
    out = [fmt.PKG_FIRST, "name " + p.name, "version " + p.version,
           "revision %d" % p.revision, "abi " + abi, "licence " + p.licence,
           "summary " + p.summary]
    out += ["runs %s%s" % (n, " " + v if v else "") for n, v in p.runs]
    out += ["built-with " + " ".join(w) for w in p.built_with]
    paths = sorted(p.files, key=lambda s: s.encode())
    for path in paths:
        mode, data = p.files[path]
        out.append("file %s %o %d %s" % (path, mode, len(data), hashlib.sha256(data).hexdigest()))
    progs = [x for x in paths if x in p.lists]
    out += ["program %s %s" % (x, fmt.encode_list(p.lists[x])) for x in progs]
    for x in progs:
        t = pagetable.make(p.files[x][1])
        out.append("table %s %d %s" % (x, len(t), hashlib.sha256(t).hexdigest()))
    out.append("signature")
    return ("\n".join(out) + "\n").encode("ascii")


def archive(p, signed):
    """The .jpk's bytes: the signed manifest, the files, the tables."""
    paths = sorted(p.files, key=lambda s: s.encode())
    members = [("+MANIFEST", 0o644, signed)]
    members += [(x, p.files[x][0], p.files[x][1]) for x in paths]
    members += [("+TABLES/" + x, 0o644, pagetable.make(p.files[x][1]))
                for x in paths if x in p.lists]
    return ustar.archive(members)


def make(r, stage, outdir, abi, key=None):
    """Write the port's packages into outdir as <name>-<version>-r<rev>.jpk;
    [(name, path)]. key: the secret key file to sign with, or None for
    an unsigned package (a bare "signature" line: every reader refuses
    it, but its bytes can be compared)."""
    pkgs = plan(r, stage)
    problems = ["%s: %s" % (p.name, x) for p in pkgs for x in check(p)]
    if problems:
        raise PackageError("the format can't hold it:\n  " + "\n  ".join(problems))
    os.makedirs(outdir, exist_ok=True)
    out = []
    for p in pkgs:
        text = manifest(p, abi)
        if len(text) + 129 > fmt.MANIFEST_MAX:
            raise PackageError("%s: its manifest is over %d bytes" % (p.name, fmt.MANIFEST_MAX))
        if key:
            try:
                text = signmod.sign(text, fmt.PKG_FIRST, key)
            except signmod.SignError as e:
                raise PackageError(str(e))
        data = archive(p, text)
        path = os.path.join(outdir, "%s-%s-r%d.jpk" % (p.name, p.version, p.revision))
        write_atomic(path, data)
        out.append((p.name, path))
    return out


def write_atomic(path, data):
    """path's bytes become data all at once (a reader sees the old file or
    the new one, never a part)."""
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
