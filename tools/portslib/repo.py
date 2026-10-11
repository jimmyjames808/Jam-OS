"""The package repository the Mac serves (`ports.py repo`, `make ports`;
docs/M13.7-PLAN.md sections 7, 8 and 13): static files in build/repo/,
served read-only by tools/ports-server.py.

    INDEX                                        the signed index
    VULNS                                        the signed vulnerability list
    pkg/<name>-<version>-r<rev>-<hash16>.jpk     each package
    src/<port>-<version>-r<rev>-<hash16>.src.tar each port's source (srctar.py)
    src/jamos-<commit12>.tar                     the Jam OS tree they were built from

<hash16> is the first 16 hex digits of the file's own SHA-256, so a
file's name changes with its bytes: a rebuild of one version and
revision (a new sysroot, a needed library's fix) is a new file, never a
file changed under a reader, and the index names exactly the bytes it
was signed with. The index's serial is the larger of the last index's
serial plus one and the time (seconds since 1970): it grows with every
run, and still grows after build/repo is deleted and made afresh.

A run writes the packages and sources first, then VULNS, then INDEX
(each file whole, by rename), so an index never names a file that isn't
there; then it removes the files neither the new index nor the one
before it names (a client that fetched the old index a moment ago still
finds its packages).

The repository is exactly the ports named (default: every `place
package` recipe) and the packages they run, transitively. VULNS is made
from ports/vulns (vulnerability lines in VULNS's own grammar, the
owner's confirmed items; `#` starts a comment line), with the index's
serial.
"""
import hashlib
import os
import shutil
import tempfile
import time

from . import package as packagemod
from . import pkgformat as fmt
from . import pkgverify
from . import sign as signmod
from . import srctar
from . import tree as treemod


class RepoError(Exception):
    pass


def closure(t, names):
    """names and every port whose package they run, transitively."""
    try:
        return sorted(set(treemod.order(t, names, ("runs",))))
    except (treemod.CycleError, KeyError) as e:
        raise RepoError("ports: %s" % e)


def read_old(repodir):
    """(serial, the repository paths it names) of the index there now;
    (0, set()) if there is none. Not checked: only for the serial and for
    keeping what a client may still fetch."""
    try:
        with open(os.path.join(repodir, "INDEX"), "rb") as f:
            lines = f.read().decode("ascii", "replace").splitlines()
    except OSError:
        return 0, set()
    serial, paths = 0, set()
    for line in lines:
        w = line.split(" ")
        try:
            if w[0] == "serial" and len(w) == 2:
                serial = int(w[1])
            elif w[0] == "package" and len(w) == 7:
                paths.add(fmt.pkg_file(w[1], w[2], w[3], w[5]))
            elif w[0] == "src" and len(w) == 6:
                paths.add(fmt.src_file(w[1], w[2], w[3], w[5]))
        except ValueError:
            continue
    return serial, paths


def vulns_lines(path):
    """The vuln lines of ports/vulns (comments and blank lines dropped),
    each checked by the reader's grammar, sorted as VULNS must be."""
    if not os.path.isfile(path):
        return []
    out = []
    with open(path, encoding="ascii") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            try:
                e = pkgverify.vuln_line(line)
            except pkgverify.Refused as err:
                raise RepoError("%s:%d: %s" % (path, n, err))
            out.append((e[0], e[4], line))
    out.sort(key=lambda x: ("%s %s" % (x[0], x[1])).encode())
    keys = ["%s %s" % (a, b) for a, b, _ in out]
    if len(keys) != len(set(keys)):
        raise RepoError("%s: one component and id twice" % path)
    return [line for _, _, line in out]


def stamp_time(now):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now))


def signed_text(lines, first, key):
    text = ("\n".join([first] + lines + ["signature"]) + "\n").encode("ascii")
    return signmod.sign(text, first, key)


class Run:
    """One `ports.py repo` run."""

    def __init__(self, t, builder, top, repodir, key, pub, say=print):
        self.t, self.builder, self.top = t, builder, top
        self.repodir, self.key, self.pub, self.say = repodir, key, pub, say
        self.abi = fmt.abi_stamp(top)
        self.files = {}      # repository path -> bytes, to write
        self.old_paths = set()   # what the index before this run names

    def packages(self, names):
        """Build and package names: [(Manifest, package's repository path)]."""
        self.builder.build(names)
        staging = os.path.join(self.repodir, ".staging")
        shutil.rmtree(staging, ignore_errors=True)
        out = []
        for n in names:
            r = self.t.recipes[n]
            stage = os.path.join(self.builder.base(n), "stage")
            for name, path in packagemod.make(r, stage, staging, self.abi, self.key):
                with open(path, "rb") as f:
                    data = f.read()
                m = pkgverify.verify_package(data, self.pub, self.abi)
                rel = fmt.pkg_file(m.name, m.version, m.revision, hashlib.sha256(data).hexdigest())
                self.files[rel] = data
                out.append((m, n, rel))
                self.say("%s: %s, %d bytes" % (name, rel, len(data)))
        shutil.rmtree(staging, ignore_errors=True)
        return out

    def sources(self, ports, treeinfo):
        """Each port's source tarball: port -> (version, revision, size, sha256)."""
        out = {}
        for n in ports:
            r = self.t.recipes[n]
            data = srctar.make(r, self.t, treeinfo, self.say)
            sha = hashlib.sha256(data).hexdigest()
            self.files[fmt.src_file(n, r.version, r.revision, sha)] = data
            out[n] = (r.version, r.revision, len(data), sha)
        return out

    def index_lines(self, serial, made, pkgs, srcs):
        lines = ["serial %d" % serial, "made %s" % made, "abi %s" % self.abi]
        for m, port, rel in sorted(pkgs, key=lambda x: x[0].name.encode()):
            data = self.files[rel]
            lines.append("package %s %s %d %d %s %s" % (m.name, m.version, m.revision, len(data),
                                                        hashlib.sha256(data).hexdigest(), port))
            lines += ["runs %s %s%s" % (m.name, d, " " + v if v else "") for d, v in m.runs]
        for port in sorted(srcs, key=str.encode):
            v, rev, size, sha = srcs[port]
            lines.append("src %s %s %d %d %s" % (port, v, rev, size, sha))
        return lines


def make(t, builder, top, repodir, key, names=None, tree_source=True, now=None, say=print):
    """Make (or remake) the repository in repodir; the new INDEX's serial."""
    names = names or sorted(n for n, r in t.recipes.items() if r.place == "package")
    for n in names:
        if n not in t.recipes or t.recipes[n].place != "package":
            raise RepoError("%s: not a package port" % n)
    names = closure(t, names)
    vulns = vulns_lines(os.path.join(t.root, "vulns"))
    os.makedirs(repodir, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ports-repo.") as tmp:
        pub = os.path.join(tmp, "ports.pub")
        try:
            signmod.public(key, pub)
        except signmod.SignError as e:
            raise RepoError(str(e))
        run = Run(t, builder, top, repodir, key, pub, say)
        pkgs = run.packages(names)
        treeinfo = srctar.tree_archive(top, repodir) if tree_source else srctar.TreeInfo()
        srcs = run.sources(sorted({port for _, port, _ in pkgs}), treeinfo)
        serial = publish(run, pkgs, srcs, vulns, now)
    keep = set(run.files) | run.old_paths
    if treeinfo.file:
        keep.add(treeinfo.file)
    collect(repodir, keep, say)
    say("repo: %s: serial %d, %d packages, %d sources" % (repodir, serial, len(pkgs), len(srcs)))
    return serial


def publish(run, pkgs, srcs, vulns, now):
    """Write the run's files, then VULNS, then INDEX (each whole, by
    rename); the serial."""
    old_serial, run.old_paths = read_old(run.repodir)
    now = time.time() if now is None else now
    serial = max(old_serial + 1, int(now))
    for rel, data in sorted(run.files.items()):
        path = os.path.join(run.repodir, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if not os.path.isfile(path):   # its name is its hash: there already, it's these bytes
            packagemod.write_atomic(path, data)
    made = stamp_time(now)
    vtext = signed_text(["serial %d" % serial, "made %s" % made] + vulns, fmt.VULNS_FIRST, run.key)
    index = signed_text(run.index_lines(serial, made, pkgs, srcs), fmt.INDEX_FIRST, run.key)
    pkgverify.verify_vulns(vtext, run.pub)
    pkgverify.verify_index(index, run.pub)
    packagemod.write_atomic(os.path.join(run.repodir, "VULNS"), vtext)
    packagemod.write_atomic(os.path.join(run.repodir, "INDEX"), index)
    return serial


def collect(repodir, keep, say=print):
    """Remove pkg/ and src/ files keep doesn't name (nor a tree tarball a
    kept source tarball names)."""
    for rel in list(keep):
        if rel.endswith(".src.tar") and os.path.isfile(os.path.join(repodir, rel)):
            with open(os.path.join(repodir, rel), "rb") as f:
                try:
                    named = srctar.tree_named(f.read())
                except Exception:   # noqa: BLE001  (an unreadable old file keeps nothing)
                    named = None
            if named:
                keep.add(named)
    for d in ("pkg", "src"):
        full = os.path.join(repodir, d)
        if not os.path.isdir(full):
            continue
        for f in sorted(os.listdir(full)):
            rel = "%s/%s" % (d, f)
            if rel not in keep:
                os.remove(os.path.join(full, f))
                say("repo: removed %s (no index names it)" % rel)
