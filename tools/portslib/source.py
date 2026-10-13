"""A port's source tree, made afresh for every build
(docs/M13.7-PLAN.md section 5, step 3): the distfiles unpacked (or the
vendored directory or the port's own `local` one copied), the overrides
checked and copied in, the patches applied in order.

Unpacking trusts nothing in the archive: a member with an absolute path,
a '..', a path through a symlink the archive made, a link whose target
leaves the tree, or a device or FIFO stops the build. Times and the
execute bits are kept (autotools decides what to regenerate by times).
"""
import os
import shutil
import sys
import tarfile
import zipfile

from . import patch as patchmod

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import checkoverrides  # noqa: E402  (tools/checkoverrides.py)

ARCHIVES = (".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tbz2", ".tar.xz", ".txz", ".zip")


class SourceError(Exception):
    pass


def _check_name(name, where):
    if name.startswith("/") or "\\" in name:
        raise SourceError("%s: member '%s' has an absolute path" % (where, name))
    parts = [p for p in name.split("/") if p not in ("", ".")]
    if ".." in parts:
        raise SourceError("%s: member '%s' goes up with '..'" % (where, name))
    return "/".join(parts)


def _link_inside(name, target):
    """Whether a symlink at name pointing to target stays in the tree."""
    if target.startswith("/"):
        return False
    depth = len(name.split("/")) - 1
    for part in target.split("/"):
        if part == "..":
            depth -= 1
            if depth < 0:
                return False
        elif part not in ("", "."):
            depth += 1
    return True


def _unpack_tar(path, dest):
    where = os.path.basename(path)
    with tarfile.open(path, "r:*") as tar:
        members, links = [], set()
        for m in tar.getmembers():
            name = _check_name(m.name, where)
            if not name:
                continue
            if any(name == l or name.startswith(l + "/") for l in links):
                raise SourceError("%s: member '%s' goes through a symlink" % (where, name))
            if m.issym():
                if not _link_inside(name, m.linkname):
                    raise SourceError("%s: symlink '%s' points out of the tree (%s)" %
                                      (where, name, m.linkname))
                links.add(name)
            elif m.islnk():
                target = _check_name(m.linkname, where)
                if any(target == l or target.startswith(l + "/") for l in links):
                    raise SourceError("%s: hard link '%s' goes through a symlink" %
                                      (where, name))
                m.linkname = target
            elif not (m.isfile() or m.isdir()):
                raise SourceError("%s: member '%s' is a device or a FIFO" % (where, name))
            m.name = name
            m.mode = (m.mode & 0o755) | (0o700 if m.isdir() else 0o600)
            m.uid = m.gid = 0
            m.uname = m.gname = ""
            members.append(m)
        # Python 3.12 and later take a filter too: "data" refuses what the
        # checks above refuse, again.
        extra = {"filter": "data"} if hasattr(tarfile, "data_filter") else {}
        tar.extractall(dest, members=members, **extra)


def _unpack_zip(path, dest):
    where = os.path.basename(path)
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            name = _check_name(info.filename, where)
            if not name:
                continue
            if (info.external_attr >> 16) & 0o170000 == 0o120000:
                raise SourceError("%s: member '%s' is a symlink (not taken from a zip)" %
                                  (where, name))
            out = os.path.join(dest, name)
            if info.is_dir():
                os.makedirs(out, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with z.open(info) as f, open(out, "wb") as o:
                shutil.copyfileobj(f, o)
            if (info.external_attr >> 16) & 0o111:
                os.chmod(out, 0o755)


def unpack(path, dest):
    """Unpack (or copy, if it isn't an archive) one distfile into dest."""
    base = os.path.basename(path)
    if base.endswith(".zip"):
        _unpack_zip(path, dest)
    elif base.endswith(ARCHIVES):
        try:
            _unpack_tar(path, dest)
        except (tarfile.TarError, EOFError, OSError) as e:
            raise SourceError("%s: %s" % (base, e))
    else:
        shutil.copyfile(path, os.path.join(dest, base))


def top_dir(work):
    """The source's top directory: the one directory the archives made, or
    work itself when they made several things."""
    entries = [e for e in os.listdir(work) if not e.startswith(".")]
    if len(entries) == 1 and os.path.isdir(os.path.join(work, entries[0])):
        return os.path.join(work, entries[0])
    return work


def copy_vendored(name, dest):
    """third_party/<name> into dest/<name>, links kept as links."""
    src = os.path.join("third_party", name)
    if not os.path.isdir(src):
        raise SourceError("third_party/%s is not there" % name)
    shutil.copytree(src, os.path.join(dest, name), symlinks=True)
    return os.path.join(dest, name)


def patches(recipe):
    """The port's patch files, in the order they apply."""
    d = recipe.port_file("patches")
    if not os.path.isdir(d):
        return []
    return [os.path.join(d, f) for f in sorted(os.listdir(d)) if not f.startswith(".")]


def overrides_list(recipe):
    path = recipe.port_file("OVERRIDES")
    return path if os.path.isfile(path) else None


def apply_overrides(recipe, srcdir):
    """Check every override against upstream's file (or its absence), then
    copy ours over it."""
    path = overrides_list(recipe)
    if path is None:
        return []
    problems = checkoverrides.check(path, srcdir, recipe.dir)
    if problems:
        raise SourceError("\n  ".join(["the overrides don't hold:"] + problems))
    done = []
    for _, words in checkoverrides.read_list(path):
        up, _, ours = words
        target = os.path.join(srcdir, up)
        if os.path.islink(target):
            raise SourceError("override %s: upstream's is a symlink" % up)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(recipe.port_file(ours), target)
        done.append(up)
    return done


def apply_patches(recipe, srcdir, say=print):
    for p in patches(recipe):
        try:
            changed = patchmod.apply(p, srcdir)
        except patchmod.PatchError as e:
            raise SourceError(str(e))
        say("patch: %s (%s)" % (os.path.basename(p), ", ".join(changed)))


def prepare(recipe, distfiles, work, say=print):
    """A fresh source tree in work (removed first); its top directory."""
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    if recipe.get("vendored"):
        srcdir = copy_vendored(recipe.get("vendored"), work)
    elif recipe.get("local"):   # the port's own code, in its directory
        d = recipe.get("local")
        if not os.path.isdir(recipe.port_file(d)):
            raise SourceError("%s is not there" % recipe.port_file(d))
        srcdir = os.path.join(work, d)
        shutil.copytree(recipe.port_file(d), srcdir, symlinks=True)
    else:
        for path in distfiles:
            unpack(path, work)
        srcdir = top_dir(work)
    apply_overrides(recipe, srcdir)
    apply_patches(recipe, srcdir, say)
    return srcdir
