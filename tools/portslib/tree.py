"""The ports tree (ports/): every recipe loaded, the checks across
recipes, and the order to build in (docs/M13.7-PLAN.md sections 2 and 5).

ports/ holds one directory per port and the files every port shares
(config.site, jamos.cmake, jamos.meson; keys/ for later). A port's
directory holds only what the format names:

    recipe        the recipe (recipe.py)
    patches/      NNNN-<what>.patch, applied in order (patch.py)
    OVERRIDES     whole-file overrides, as user/posix/musl/OVERRIDES,
    override/     ...and the files they name
    wants/        each program's list, when it isn't the default `posix`
    build.sh      style custom only: build() and install()
    files/        our own files a custom build uses (a Makefile, a program
                  of ours linked in, a generator: sbase's box, libc-test's
                  runner); not part of upstream, so no OVERRIDES line
    test          what to run on Jam OS to test it (tools/ports-test.sh)
    LICENSE       a note on the licence of our patches (a GPL port's)

The order: `needs` and `runs` make a graph; a cycle is refused with the
cycle printed, and a port is built only after everything it needs.
"""
import os
import re
import sys

from . import licence
from . import patch as patchmod
from . import recipe as recipemod

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import checkoverrides  # noqa: E402
import checkwants  # noqa: E402

SHARED = {"config.site", "jamos.cmake", "jamos.meson"}
RESERVED_DIRS = {"keys"}
PORT_ENTRIES = {"recipe", "patches", "OVERRIDES", "override", "wants", "build.sh", "files",
                "test", "LICENSE"}
# What `allow` refuses a program on /data (user/services/shell/sh_allow.c),
# on top of what tools/checkwants.py refuses any program outside
# user/services/ and user/tests/: a package can't ask for them.
PACKAGE_REFUSED = {"svc devmgr", "right debug"}


class CycleError(Exception):
    pass


class Tree:
    def __init__(self, root):
        self.root = root      # the ports directory
        self.recipes = {}     # name -> Recipe

    def shared(self, name):
        return os.path.join(self.root, name)


def load(root):
    """(Tree, problems) for every port under root."""
    t, problems = Tree(root), []
    if not os.path.isdir(root):
        return t, ["%s: no ports directory" % root]
    for e in sorted(os.listdir(root)):
        path = os.path.join(root, e)
        if e.startswith(".") or e in RESERVED_DIRS:
            continue
        if not os.path.isdir(path):
            if e not in SHARED:
                problems.append("%s: not a port's directory or a shared file (%s)" %
                                (path, ", ".join(sorted(SHARED))))
            continue
        r, p = recipemod.parse(os.path.join(path, "recipe"))
        problems += p
        if r.name:
            t.recipes[e] = r
    return t, problems


def vkey(v):
    """A version as a list that compares the way people read versions:
    numbers as numbers, '1.10' after '1.9'."""
    return [(0, int(x), "") if x.isdigit() else (1, 0, x) for x in re.findall(r"\d+|[A-Za-z]+", v)]


def lint(t, names=None):
    """Every recipe's own checks and the checks across recipes."""
    problems = []
    for name in sorted(names or t.recipes):
        r = t.recipes[name]
        problems += recipemod.lint(r)
        problems += _entries(r)
        problems += _deps(t, r)
        problems += _patches(r)
        problems += _overrides(r)
        problems += _lists(r)
    try:
        find_cycle(t)
    except CycleError as e:
        problems.append(str(e))
    return problems


def _entries(r):
    return ["%s: '%s' is not part of a port (%s)" % (r.dir, e, ", ".join(sorted(PORT_ENTRIES)))
            for e in sorted(os.listdir(r.dir)) if e not in PORT_ENTRIES and not e.startswith(".")]


def _provided(t):
    """Every package name a recipe makes: its own and its subpackages'."""
    out = {}
    for r in t.recipes.values():
        out[r.name] = r
        for w in r.lines("subpackage"):
            out[w[0]] = r
    return out


def _deps(t, r):
    p, provided = [], _provided(t)
    for key in ("needs", "runs"):
        for dep, least in r.deps(key):
            other = t.recipes.get(dep) if key == "needs" else provided.get(dep)
            if other is None:
                p.append("%s: %s %s: no such %s" %
                         (r.path, key, dep, "port" if key == "needs" else "package"))
            elif least and vkey(other.version) < vkey(least):
                p.append("%s: %s %s >= %s, but its recipe is %s" %
                         (r.path, key, dep, least, other.version))
            if key == "needs" and other is not None:
                p += _lgpl(r, other)
    return p


def _lgpl(r, lib):
    """A static link of an LGPL library leaves the duty to let the user
    relink: refused unless the recipe says how it is met."""
    try:
        bound = licence.binds_lgpl(lib.get("licence", ""))
    except ValueError:
        return []   # the library's own lint names its licence
    if bound and not r.get("lgpl-relink"):
        return ["%s: needs %s, an LGPL library linked statically: say how its relink "
                "duty is met (lgpl-relink <how>) or wait for dynamic linking" % (r.path, lib.name)]
    return []


def _patches(r):
    p, touched = [], set()
    d = r.port_file("patches")
    if os.path.isdir(d):
        for f in sorted(os.listdir(d)):
            path = os.path.join(d, f)
            if not patchmod.NAME.match(f):
                p.append("%s: patch names are NNNN-<what>.patch" % path)
                continue
            with open(path, "rb") as fh:
                data = fh.read()
            p += ["%s: %s" % (path, why) for why in patchmod.header_problems(data)]
            try:
                touched.update(fp.path for fp in patchmod.parse(data, path))
            except patchmod.PatchError as e:
                p.append(str(e))
        numbers = [f[:4] for f in os.listdir(d) if patchmod.NAME.match(f)]
        if len(numbers) != len(set(numbers)):
            p.append("%s: two patches share a number" % d)
    ovr = r.port_file("OVERRIDES")
    if os.path.isfile(ovr):
        for _, words in checkoverrides.read_list(ovr):
            if words and words[0] in touched:
                p.append("%s: %s is both patched and overridden (one or the other)" %
                         (ovr, words[0]))
    return p


def _overrides(r):
    path = r.port_file("OVERRIDES")
    if not os.path.isfile(path):
        if os.path.isdir(r.port_file("override")):
            return ["%s: override/ without an OVERRIDES list" % r.dir]
        return []
    if r.get("vendored"):
        return checkoverrides.check(path, os.path.join("third_party", r.get("vendored")), r.dir)
    # A fetched source's hashes are checked when it is unpacked; here the
    # list's shape and our files.
    p = []
    for n, w in checkoverrides.read_list(path):
        if len(w) != 3 or not (checkoverrides.HEX64.match(w[1]) or w[1] == checkoverrides.NEW):
            p.append("%s:%d: want '<upstream file> <sha256 or -> <our file>'" % (path, n))
        elif not os.path.isfile(os.path.join(r.dir, w[2])):
            p.append("%s:%d: our file %s is missing" % (path, n, w[2]))
    return p


def list_problems(text, place, src):
    """What a program's list may not ask for in its place: the build's
    rules for any program outside user/services/ and user/tests/, and for
    a package also what `allow` refuses on /data."""
    problems, shown = checkwants.check_text(text, False, checkwants.services(), False, src)
    if "posix" not in shown:
        problems.append("the list has no 'posix' (a ported program is a POSIX one)")
    if place == "package":
        problems += ["'%s': `allow` refuses it, so a package can't ask for it" % w
                     for w in shown if w in PACKAGE_REFUSED]
    return problems


def _lists(r):
    p = []
    for path, wants in r.programs():
        if wants is None:
            continue
        try:
            with open(r.port_file(wants), "rb") as f:
                text = f.read()
        except OSError:
            continue   # recipe.lint names it
        src = os.path.relpath(r.dir)
        p += ["%s: program %s (%s): %s" % (r.path, path, wants, why)
              for why in list_problems(text, r.place, src)]
    return p


def find_cycle(t, kinds=("needs", "runs")):
    """CycleError naming the cycle, if the graph has one."""
    provided = _provided(t)
    state = {}

    def visit(name, path):
        state[name] = 1
        r = t.recipes[name]
        for key in kinds:
            for dep, _ in r.deps(key):
                other = provided.get(dep)
                if other is None:
                    continue
                if state.get(other.name) == 1:
                    cyc = path[path.index(other.name):] + [other.name]
                    raise CycleError("ports: a cycle: %s" % " -> ".join(cyc))
                if other.name not in state:
                    visit(other.name, path + [other.name])
        state[name] = 2

    for name in sorted(t.recipes):
        if name not in state:
            visit(name, [name])


def order(t, names, kinds=("needs",)):
    """names and everything they need (or run), each after what it needs,
    ties broken by name; CycleError on a cycle, KeyError on an unknown
    port."""
    find_cycle(t)
    provided = _provided(t)
    out, done = [], set()

    def visit(name):
        if name in done:
            return
        done.add(name)
        r = t.recipes[name]
        for key in kinds:
            for dep, _ in sorted(r.deps(key)):
                if dep in provided:
                    visit(provided[dep].name)
        out.append(name)

    for name in names:
        if name not in t.recipes:
            raise KeyError(name)
        visit(name)
    return out


def needs_closure(t, name):
    """Every port name's build sees: what it needs, and what they need (a
    static library's own libraries must be linked too), not itself."""
    return [n for n in order(t, [name]) if n != name]
