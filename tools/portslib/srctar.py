"""A port's source tarball, served beside its packages
(src/<port>-<version>-r<rev>-<hash>.src.tar; docs/M13.7-PLAN.md section
12): everything needed to build the port's packages again, so a GPL or
LGPL package's complete source is always where the package is (every
port gets one: one rule for all).

It is a ustar archive in ustar.py's one form, every member under
<port>-<version>-r<rev>/:

    README                  what this is
    distfiles/<file>        each source the recipe names, as fetched (its
                            size and SHA-256 are the recipe's)
    vendored/<dir>/...      a vendored source: third_party/<dir>
    port/...                the port's directory: recipe, patches,
                            overrides, lists, build.sh, test, LICENSE
    shared/<file>           ports/config.site, jamos.cmake, jamos.meson
    jamos-tree              one line: "jamos-tree <commit> <file> <size>
                            <sha256>": the Jam OS tree the port was built
                            against (the sysroot, jamos-cc, the builder),
                            as a tarball beside it in src/; "jamos-tree
                            unknown - - -" where there is no git

The Jam OS tree's tarball is one per commit (`git archive` of it; of a
`git stash create` commit when tracked files have changes), shared by
every port built from it: src/jamos-<12 hex digits of the commit>.tar.
"""
import hashlib
import os
import subprocess

from . import fetch as fetchmod
from . import tree as treemod
from . import ustar

README = """This is the source of the Jam OS package(s) built from the port
%(port)s %(version)s, revision %(revision)d: the upstream sources as
fetched (distfiles/ or vendored/), the port's directory with its recipe,
patches and overrides (port/), the builder's shared files (shared/), and in
jamos-tree the Jam OS tree it was built against, a tarball in the same
repository. tools/ports.py in that tree builds it again:
`tools/ports.py build %(port)s` with this port's directory under ports/.
"""


class TreeInfo:
    """The Jam OS tree a repository's ports were built against."""

    def __init__(self, commit=None, file=None, size=None, sha256=None):
        self.commit, self.file, self.size, self.sha256 = commit, file, size, sha256

    def line(self):
        if self.commit is None:
            return "jamos-tree unknown - - -\n"
        return "jamos-tree %s %s %d %s\n" % (self.commit, self.file, self.size, self.sha256)


def _git(top, *args):
    p = subprocess.run(["git", "-C", top] + list(args), stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL)
    return p.stdout.decode().strip() if p.returncode == 0 else None


def tree_archive(top, repodir):
    """The tree's tarball in repodir/src (made once per commit): TreeInfo,
    with no commit where top has no git."""
    head = _git(top, "rev-parse", "HEAD")
    if not head:
        return TreeInfo()
    commit = head
    if _git(top, "status", "--porcelain", "--untracked-files=no"):
        commit = _git(top, "stash", "create") or head   # the changes as a commit; no ref made
    rel = "src/jamos-%s.tar" % commit[:12]
    path = os.path.join(repodir, rel)
    if not os.path.isfile(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            subprocess.run(["git", "-C", top, "archive", "--format=tar",
                            "--prefix=jamos-%s/" % commit[:12], commit], stdout=f, check=True)
        os.replace(tmp, path)
    with open(path, "rb") as f:
        data = f.read()
    return TreeInfo(commit, rel, len(data), hashlib.sha256(data).hexdigest())


def _files_under(d):
    """(relative path, absolute path) of every file under d, sorted; dot
    files left out."""
    out = []
    for root, dirs, files in os.walk(d):
        dirs[:] = sorted(x for x in dirs if not x.startswith("."))
        for f in sorted(files):
            if not f.startswith("."):
                p = os.path.join(root, f)
                out.append((os.path.relpath(p, d), p))
    return out


def _mode(path):
    return 0o755 if os.access(path, os.X_OK) else 0o644


def make(r, t, treeinfo, say=print):
    """The source tarball's bytes for recipe r of tree t."""
    top = "%s-%s-r%d/" % (r.name, r.version, r.revision)
    members = [(top + "README", 0o644,
                (README % {"port": r.name, "version": r.version, "revision": r.revision}).encode())]
    for s in r.sources:
        path = fetchmod.fetch(s, say)
        with open(path, "rb") as f:
            members.append((top + "distfiles/" + s.file, 0o644, f.read()))
    if r.get("vendored"):
        d = os.path.join("third_party", r.get("vendored"))
        for rel, p in _files_under(d):
            with open(p, "rb") as f:
                members.append((top + "vendored/%s/%s" % (r.get("vendored"), rel), _mode(p),
                                f.read()))
    for rel, p in _files_under(r.dir):
        with open(p, "rb") as f:
            members.append((top + "port/" + rel, _mode(p), f.read()))
    for name in sorted(treemod.SHARED):
        p = t.shared(name)
        if os.path.isfile(p):
            with open(p, "rb") as f:
                members.append((top + "shared/" + name, 0o644, f.read()))
    members.append((top + "jamos-tree", 0o644, treeinfo.line().encode()))
    members.sort(key=lambda m: m[0].encode())
    return ustar.archive(members)


def tree_named(data):
    """The tree tarball's repository path a source tarball names, or None."""
    for path, _, body in ustar.members(data):
        if path.endswith("/jamos-tree"):
            w = body.decode("ascii", "replace").split()
            return w[2] if len(w) == 5 and w[0] == "jamos-tree" and w[2] != "-" else None
    return None
