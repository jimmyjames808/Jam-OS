"""A kept tree (docs/CR-PLAN.md sections 14 and 15, track CB1): for a port
too big to unpack and build afresh every time (Chromium: 10 GB of source,
hours of compiling), the recipe line `tree kept` keeps its unpacked,
patched source, its build's output and the tools its build commands name
between builds, in a directory of their own:

    <kept>/src/          the source, unpacked once; the build's output goes
                         inside it (build.sh's choice: Chromium's out/)
    <kept>/sysroot/      the port's sysroot (toolchain.make_sysroot's)
    <kept>/bin/          its tools (toolchain.make_bin's)
    <kept>/pristine/     upstream's copy of every file a patch or override
                         touches (none: upstream has no such file)
    <kept>/source.json   what src/ was made from: the distfiles' hashes,
                         each patch's and override's hash, and the paths
                         they touched

The sysroot and the tools are made afresh as for any port, then copied in
only where their bytes changed, so their paths in the build's commands stay
the same and their times change only with their contents: an incremental
build (ninja, make) sees as changed only what is. The same rule makes the
source: the distfiles are unpacked once; when only the patches or
overrides change, the files any old or new one touches are taken as
upstream has them (pristine/, or src/ for a file no old patch touched),
the new patches applied to those copies, and only the files whose bytes
differ written into src/. A change to the args or to build.sh changes
nothing in src/; a changed patch rewrites the files it touches. Other
distfiles (a new version) unpack afresh into an empty src/, its output
gone with it: a clean build, as `ports.py build --clean` asks on purpose.

<kept> is build/ports/<name>/kept, or `ports.py build --kept DIR`
(JAMOS_KEPT): outside the checkout, so every checkout's build shares one
(tools/cr/build.sh gives ~/cr/<version>). One build at a time uses it
(the builder's lock on the output directory, and tools/cr/build.sh's own).
The build gets its path as $kept.
"""
import hashlib
import json
import os
import shutil

from . import patch as patchmod
from . import source as sourcemod

STATE = "source.json"


class KeptError(Exception):
    pass


def kept_dir(base, override=None):
    """Where a port's kept tree is: override (--kept, JAMOS_KEPT) or
    <base>/kept."""
    d = override or os.environ.get("JAMOS_KEPT")
    return os.path.abspath(d) if d else os.path.join(base, "kept")


def _same(a, b):
    """Whether two paths hold the same thing: a link to the same target,
    or a file with the same mode bits and bytes."""
    if os.path.islink(a) or os.path.islink(b):
        return os.path.islink(a) and os.path.islink(b) and os.readlink(a) == os.readlink(b)
    if not os.path.isfile(b):
        return False
    sa, sb = os.stat(a), os.stat(b)
    if sa.st_size != sb.st_size or (sa.st_mode & 0o777) != (sb.st_mode & 0o777):
        return False
    with open(a, "rb") as fa, open(b, "rb") as fb:
        while True:
            x, y = fa.read(1 << 20), fb.read(1 << 20)
            if x != y:
                return False
            if not x:
                return True


def _remove(path):
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path)
    elif os.path.lexists(path):
        os.remove(path)


def _put(src, dst):
    """dst made a copy of src (a file or a link), replaced in one step. The
    copy's time is now, never src's: a header older than the objects built
    from its last version would not be seen as changed."""
    tmp = dst + ".kept-new"
    _remove(tmp)
    if os.path.islink(src):
        os.symlink(os.readlink(src), tmp)
    else:
        shutil.copyfile(src, tmp)
        shutil.copymode(src, tmp)
    if os.path.isdir(dst) and not os.path.islink(dst):
        shutil.rmtree(dst)
    os.replace(tmp, dst)


def sync(src, dst):
    """Make the tree dst hold what src holds, writing only the files whose
    bytes, mode or link target differ and removing what src lacks.
    Returns how many entries it wrote or removed."""
    changed = 0
    os.makedirs(dst, exist_ok=True)
    for d, dirs, files in os.walk(src):
        rel = os.path.relpath(d, src)
        out = os.path.normpath(os.path.join(dst, rel))
        keep = set()
        for name in sorted(dirs):
            s, t = os.path.join(d, name), os.path.join(out, name)
            keep.add(name)
            if os.path.islink(s):   # os.walk lists a link to a directory here
                dirs.remove(name)
                if not _same(s, t):
                    _put(s, t)
                    changed += 1
            elif not os.path.isdir(t) or os.path.islink(t):
                _remove(t)
                os.makedirs(t)
                changed += 1
        for name in files:
            s, t = os.path.join(d, name), os.path.join(out, name)
            keep.add(name)
            if not _same(s, t):
                _put(s, t)
                changed += 1
        for name in os.listdir(out):
            if name not in keep:
                _remove(os.path.join(out, name))
                changed += 1
    return changed


def _digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def _inputs(recipe, distfiles):
    """What the source is made from, as source.json records it."""
    over = sourcemod.overrides_list(recipe)
    touched = set()
    for p in sourcemod.patches(recipe):
        try:
            touched.update(patchmod.targets(p))
        except patchmod.PatchError as e:
            raise KeptError(str(e))
    overrides = {}
    if over:
        for _, words in sourcemod.checkoverrides.read_list(over):
            touched.add(words[0])
            overrides[words[0]] = _digest(recipe.port_file(words[2]))
    return {
        "sources": [os.path.basename(d) + " " + _digest(d) for d in distfiles],
        "patches": [[os.path.basename(p), _digest(p)] for p in sourcemod.patches(recipe)],
        "overrides": overrides,
        "overrides-list": _digest(over) if over else None,
        "touched": sorted(touched),
    }


def _read_state(kept):
    try:
        with open(os.path.join(kept, STATE)) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def _write_state(kept, state):
    tmp = os.path.join(kept, STATE + ".new")
    with open(tmp, "w") as f:
        json.dump(state, f, indent=1, sort_keys=True)
        f.write("\n")
    os.replace(tmp, os.path.join(kept, STATE))


def _keep_pristine(top, pristine, paths):
    """Upstream's copy of each of paths that top has (before any patch
    touched it) into pristine/; a path upstream lacks gets no copy."""
    for p in paths:
        have, keep = os.path.join(top, p), os.path.join(pristine, p)
        if os.path.isfile(have) and not os.path.lexists(keep):
            os.makedirs(os.path.dirname(keep), exist_ok=True)
            _put(have, keep)


def _fresh(distfiles, kept, sources, say):
    """src/ unpacked afresh (its old self, and the output in it, removed),
    as upstream has it: the state of a tree with no patches, which the
    patches are then applied to as to any (a patch that doesn't apply then
    costs no second unpacking)."""
    src, new = os.path.join(kept, "src"), os.path.join(kept, "src.new")
    for d in (src, new, os.path.join(kept, "pristine")):
        if os.path.exists(d):
            say("kept: removing %s" % d)
            shutil.rmtree(d)
    os.makedirs(new)
    for path in distfiles:
        say("kept: unpacking %s" % os.path.basename(path))
        sourcemod.unpack(path, new)
    os.replace(new, src)
    state = {"sources": sources, "patches": [], "overrides": {}, "overrides-list": None,
             "touched": []}
    _write_state(kept, state)
    return state


def _update(recipe, kept, old, new, say):
    """Only the patches or overrides changed: the files they touch (old and
    new) as upstream has them, the new ones applied, and only the files whose
    bytes changed written into src/. Upstream's copy of a file an old patch
    touched is in pristine/ (none there: upstream has no such file); a file
    no old patch touched is still upstream's in src/."""
    src = os.path.join(kept, "src")
    top = sourcemod.top_dir(src)
    pristine = os.path.join(kept, "pristine")
    paths = sorted(set(old["touched"]) | set(new["touched"]))
    _keep_pristine(top, pristine, sorted(set(new["touched"]) - set(old["touched"])))
    stop = os.path.join(kept, "patch-scratch")
    _remove(stop)
    os.makedirs(stop)
    for p in paths:
        if os.path.isfile(os.path.join(pristine, p)):
            os.makedirs(os.path.dirname(os.path.join(stop, p)), exist_ok=True)
            _put(os.path.join(pristine, p), os.path.join(stop, p))
    sourcemod.apply_overrides(recipe, stop)
    sourcemod.apply_patches(recipe, stop, say)
    # Every patch applied: src/ changes from here (a build stopped half way
    # through finds no source.json and unpacks afresh).
    os.remove(os.path.join(kept, STATE))
    written = 0
    for p in paths:
        fresh, have = os.path.join(stop, p), os.path.join(top, p)
        if os.path.isfile(fresh):
            if not _same(fresh, have):
                os.makedirs(os.path.dirname(have), exist_ok=True)
                _put(fresh, have)
                written += 1
        elif os.path.lexists(have):
            os.remove(have)   # a file only an old patch made
            written += 1
    shutil.rmtree(stop)
    say("kept: the patches changed: %d of the %d files they touch rewritten" %
        (written, len(paths)))
    return top


def prepare(recipe, distfiles, kept, say=print):
    """The kept source tree, made or brought up to date; its top directory.
    A patch that doesn't apply leaves src/ as it was."""
    os.makedirs(kept, exist_ok=True)
    new = _inputs(recipe, distfiles)
    old = _read_state(kept)
    src = os.path.join(kept, "src")
    if old is None or not os.path.isdir(src) or old.get("sources") != new["sources"]:
        if os.path.exists(os.path.join(kept, STATE)):
            os.remove(os.path.join(kept, STATE))
        old = _fresh(distfiles, kept, new["sources"], say)
    if all(old.get(k) == new[k] for k in ("patches", "overrides", "overrides-list")):
        say("kept: the source is as it was (%s)" % src)
    else:
        _update(recipe, kept, old, new, say)
        _write_state(kept, new)
    return sourcemod.top_dir(src)


def clean(kept, say=print):
    """`ports.py build --clean`: the kept tree removed, so the next build
    unpacks and builds everything afresh."""
    if os.path.exists(kept):
        say("kept: removing %s (--clean)" % kept)
        shutil.rmtree(kept)
