"""Distfiles: a port's sources, fetched by size and SHA-256 into a cache
(docs/M13.7-PLAN.md section 3).

Where a distfile is looked for, in order:

1. the cache, JAMOS_DISTFILES (default ~/.cache/jamos/distfiles);
2. the owner's mirror, JAMOS_DISTFILES_MIRROR (default ~/jamos-distfiles;
   `none`: no mirror), a directory of every distfile ever fetched, kept
   and backed up because upstream URLs rot;
3. the recipe's URL;
4. each base URL in JAMOS_MIRROR_URL (spaces between), the mirror served
   over HTTP (the PC's builds fetch the Mac's mirror over the LAN).

Every candidate is checked, size first and then SHA-256, before anything
opens it; one that fails is deleted and the next place is tried, and a
download is cut off past the recipe's size. A file that passes is put in
the cache and copied into the mirror. --offline (or JAMOS_OFFLINE=1)
skips 3 and 4 except file:// URLs.

`source git <url> <commit>`: the commit is checked out of a bare clone in
the cache and archived (`git archive`) as a tar; its SHA-256 is recorded
beside it (<file>.sha256, in the cache and the mirror), and a later
archive of the same commit that hashes differently stops the build.
"""
import hashlib
import os
import shutil
import subprocess
import urllib.request


class FetchError(Exception):
    pass


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def cache_dir():
    return os.path.expanduser(os.environ.get("JAMOS_DISTFILES", "~/.cache/jamos/distfiles"))


def mirror_dir():
    m = os.environ.get("JAMOS_DISTFILES_MIRROR", "~/jamos-distfiles")
    return None if m == "none" else os.path.expanduser(m)


def offline():
    return os.environ.get("JAMOS_OFFLINE") == "1"


def _bad(path, src):
    """Why path isn't src's file (None: it is)."""
    size = os.path.getsize(path)
    if size != src.size:
        return "%d bytes, the recipe says %d" % (size, src.size)
    got = sha256_file(path)
    if got != src.digest:
        return "sha256 %s, the recipe says %s" % (got, src.digest)
    return None


def _download(url, dest, limit):
    """url into dest, at most limit bytes (more is an error)."""
    if shutil.which("curl"):
        cmd = ["curl", "-fsSL", "--connect-timeout", "20", "--retry", "2",
               "--max-filesize", str(limit), "-o", dest, url]
        r = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
        if r.returncode != 0:
            raise FetchError("curl: %s" % (r.stdout.decode(errors="replace").strip() or
                                           "exit %d" % r.returncode))
        return
    try:
        with urllib.request.urlopen(url, timeout=60) as resp, open(dest, "wb") as out:
            data = resp.read(limit + 1)
            if len(data) > limit:
                raise FetchError("more than %d bytes" % limit)
            out.write(data)
    except OSError as e:
        raise FetchError(str(e))


def _try_file(path, src, cache_path):
    """Take path (a local copy) into the cache if it is src's file."""
    if not os.path.isfile(path):
        return None
    why = _bad(path, src)
    if why:
        return "%s: %s" % (path, why)
    if os.path.abspath(path) != os.path.abspath(cache_path):
        tmp = cache_path + ".part"
        shutil.copyfile(path, tmp)
        os.replace(tmp, cache_path)
    return ""


def _try_url(url, src, cache_path):
    tmp = cache_path + ".part"
    try:
        _download(url, tmp, src.size)
    except FetchError as e:
        if os.path.exists(tmp):
            os.remove(tmp)
        return "%s: %s" % (url, e)
    why = _bad(tmp, src)
    if why:
        os.remove(tmp)
        return "%s gave a different file: %s" % (url, why)
    os.replace(tmp, cache_path)
    return ""


def _fill_mirror(path, name, extra=None):
    """Copy a checked distfile into the mirror if it isn't there yet."""
    m = mirror_dir()
    if m is None:
        return
    os.makedirs(m, exist_ok=True)
    for src, dst in [(path, os.path.join(m, name))] + (extra or []):
        if not os.path.exists(dst):
            shutil.copyfile(src, dst + ".part")
            os.replace(dst + ".part", dst)


def fetch(src, say=print):
    """The path of src's checked file in the cache; FetchError naming
    every place tried and why each failed."""
    if src.kind == "git":
        return _fetch_git(src, say)
    cache = cache_dir()
    os.makedirs(cache, exist_ok=True)
    cache_path = os.path.join(cache, src.file)
    tried = []
    places = [("file", cache_path)]
    if mirror_dir():
        places.append(("file", os.path.join(mirror_dir(), src.file)))
    urls = [src.url] + [b.rstrip("/") + "/" + src.file
                        for b in os.environ.get("JAMOS_MIRROR_URL", "").split()]
    places += [("url", u) for u in urls if not offline() or u.startswith("file://")]
    for kind, where in places:
        if kind == "url":
            say("fetch: %s" % where)
            why = _try_url(where, src, cache_path)
        else:
            why = _try_file(where, src, cache_path)
            if why and where == cache_path:
                os.remove(cache_path)   # a bad copy in the cache is never kept
        if why == "":
            _fill_mirror(cache_path, src.file)
            return cache_path
        if why:
            tried.append(why)
    if offline():
        tried.append("offline: the network was not tried")
    raise FetchError("%s: not found with its size and hash:\n  %s" %
                     (src.file, "\n  ".join(tried) or "nowhere to look"))


def _git(args, cwd=None):
    r = subprocess.run(["git"] + args, cwd=cwd, stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        raise FetchError("git %s: %s" % (" ".join(args[:2]), r.stdout.decode(errors="replace")))
    return r.stdout


def _recorded(name):
    """The SHA-256 recorded for a git archive before, from the cache or the
    mirror (None: never made)."""
    for d in (cache_dir(), mirror_dir()):
        if d and os.path.isfile(os.path.join(d, name + ".sha256")):
            with open(os.path.join(d, name + ".sha256")) as f:
                return f.read().split()[0]
    return None


def _fetch_git(src, say):
    cache = cache_dir()
    os.makedirs(cache, exist_ok=True)
    cache_path = os.path.join(cache, src.file)
    want = _recorded(src.file)
    m = mirror_dir()
    for path in [cache_path] + ([os.path.join(m, src.file)] if m else []):
        if want and os.path.isfile(path) and sha256_file(path) == want:
            if path != cache_path:
                shutil.copyfile(path, cache_path)
            return cache_path
    if not shutil.which("git"):
        raise FetchError("%s: needs git on this machine" % src.file)
    if offline() and not src.url.startswith("file://"):
        raise FetchError("%s: offline, and not in the cache or the mirror" % src.file)
    bare = os.path.join(cache, "git", src.file[:-4] + ".git")
    if not os.path.isdir(bare):
        say("fetch: git clone %s" % src.url)
        _git(["clone", "--quiet", "--bare", src.url, bare])
    try:
        _git(["cat-file", "-e", src.digest + "^{commit}"], cwd=bare)
    except FetchError:
        _git(["fetch", "--quiet", "origin", src.digest], cwd=bare)
    prefix = src.file[:-4] + "/" if src.file.endswith(".tar") else src.file + "/"
    tmp = cache_path + ".part"
    _git(["archive", "--format=tar", "--prefix=" + prefix, "-o", tmp, src.digest], cwd=bare)
    got = sha256_file(tmp)
    if want and got != want:
        os.remove(tmp)
        raise FetchError("%s: git archive of %s hashes %s, but %s was recorded before: "
                         "the same commit must give the same bytes" %
                         (src.file, src.digest, got, want))
    os.replace(tmp, cache_path)
    with open(cache_path + ".sha256", "w") as f:
        f.write(got + "\n")
    _fill_mirror(cache_path, src.file, [(cache_path + ".sha256",
                                         os.path.join(m, src.file + ".sha256"))] if m else None)
    return cache_path
