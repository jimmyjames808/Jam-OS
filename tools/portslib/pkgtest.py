"""The packager's self-test (part of `ports.py selftest`, in `make check`):
offline, no QEMU, with the real compiler and a throwaway key.

    ustar        the one form: written, read back, by Python's tarfile
                 too; a long path split; a header not of the form refused
    pagetable    the tables against libos's and the kernel's SHA-256 on
                 the Mac (build/host/pagetable-libos, -kernel), many sizes
                 and a real staged program
    packages     fixtures.py's ports built and packaged: the files, the
                 programs and their lists, a multicall copy, a subpackage
                 with its own licences, runs and built-with, a library
                 port making no package; the same bytes twice and after a
                 forced rebuild
    repository   repo.make: the closure of runs, the index, VULNS from
                 ports/vulns, the sources (and a tree tarball from a small
                 git repository, made without a stash ref), every package
                 checked against the index; then the refusals: a package
                 changed in the repository, the index changed, an old
                 index replayed after a newer one, an index read as VULNS
                 and the other way round, a manifest read as an index,
                 another key; the serial still growing with the clock
                 turned back; files no index names removed
"""
import hashlib
import os
import random
import shutil
import subprocess
import tarfile

from . import build as buildmod
from . import fixtures
from . import package as packagemod
from . import pagetable
from . import pkgformat as fmt
from . import pkgverify
from . import repo as repomod
from . import sign as signmod
from . import srctar
from . import tree as treemod
from . import ustar

FAILS = []


def expect(cond, what):
    if not cond:
        FAILS.append(what)


def refused(fn, what):
    try:
        fn()
    except pkgverify.Refused:
        return
    FAILS.append("%s: not refused" % what)


def test_ustar(tmp):
    long = "share/" + "d" * 120 + "/" + "f" * 60
    members = [("+MANIFEST", 0o644, b"m\n"), ("bin/x", 0o755, b"\x7fELF" * 300),
               (long, 0o644, b""), ("a" * 100, 0o644, b"exactly 100")]
    data = ustar.archive(members)
    expect(ustar.members(data) == members, "ustar: members read back")
    path = os.path.join(tmp, "u.tar")
    with open(path, "wb") as f:
        f.write(data)
    with tarfile.open(path) as t:
        got = [(m.name, m.mode, t.extractfile(m).read()) for m in t.getmembers()]
    expect(got == members, "ustar: Python's tarfile reads it otherwise: %s" %
           [g[0] for g in got])
    expect(ustar.split(long) == (("share/" + "d" * 120).encode(), ("f" * 60).encode()),
           "ustar: a long path's split")
    for bad in ("x" * 101, "d/" + "x" * 101, "a" * 156 + "/b"):
        try:
            ustar.split(bad)
            FAILS.append("ustar: %r has no split but was taken" % bad)
        except ustar.UstarError:
            pass
    other = os.path.join(tmp, "other.tar")
    with tarfile.open(other, "w", format=tarfile.USTAR_FORMAT) as t:
        info = tarfile.TarInfo("bin/x")
        info.size, info.mode, info.uname = 4, 0o755, "someone"
        t.addfile(info, open(path, "rb"))
    with open(other, "rb") as f:
        try:
            ustar.members(f.read())
            FAILS.append("ustar: another writer's header (an owner's name) taken")
        except ustar.UstarError:
            pass


def test_pagetable(tmp, top, stage_program):
    tools = [os.path.join(top, "build", "host", "pagetable-" + w) for w in ("libos", "kernel")]
    missing = [t for t in tools if not os.access(t, os.X_OK)]
    if missing:
        FAILS.append("pagetable: no %s (make check builds them)" % ", ".join(missing))
        return
    rnd = random.Random(1)
    sizes = [1, 55, 56, 64, 4095, 4096, 4097, 8192, 3 * 4096 + 1, 50000, 70001]
    files = []
    for n in sizes:
        path = os.path.join(tmp, "page-%d" % n)
        with open(path, "wb") as f:
            f.write(bytes(rnd.getrandbits(8) for _ in range(n)))
        files.append(path)
    if stage_program:
        files.append(stage_program)
    for path in files:
        with open(path, "rb") as f:
            want = pagetable.make(f.read())
        for t in tools:
            got = subprocess.run([t, path], stdout=subprocess.PIPE).stdout
            expect(got == want, "pagetable: %s's table of %s (%d bytes) differs from the "
                   "packager's" % (os.path.basename(t), os.path.basename(path),
                                   os.path.getsize(path)))
    expect(len(pagetable.make(b"x" * 4097)) == 64 + 2 * 32, "pagetable: the size of 2 pages")
    class OneTooBig:   # the length alone is looked at: a real gigabyte is not made here
        def __len__(self):
            return fmt.FILE_MAX + 1

    for bad in (b"", OneTooBig()):
        try:
            pagetable.make(bad)
            FAILS.append("pagetable: a file of %d bytes taken" % len(bad))
        except ValueError:
            pass


TOOL_BUILD = """build() {
    "$CC" -O2 -o tool tool.c
}

install() {
    mkdir -p "$destdir$prefix/bin"
    cp tool "$destdir$prefix/bin/"
}
"""


def fixture_tree(tmp, real_ports):
    """fixtures.py's ports, and a program port `tool` that app runs; app
    gains a multicall name."""
    root, up = os.path.join(tmp, "pk"), os.path.join(tmp, "pk-up")
    fixtures.write_tree(root, up, real_ports)
    fixtures.make_tar(os.path.join(up, "tool-1.0.tar.gz"), "tool-1.0",
                      {"tool.c": "int main(void) { return 0; }\n", "LICENSE": "tool: MIT\n"})
    os.makedirs(os.path.join(root, "tool"))
    with open(os.path.join(root, "tool", "recipe"), "w") as f:
        f.write(fixtures.recipe("tool", "custom", "MIT",
                                fixtures.source_line(os.path.join(up, "tool-1.0.tar.gz")),
                                ["program bin/tool"]))
    with open(os.path.join(root, "tool", "build.sh"), "w") as f:
        f.write(TOOL_BUILD)
    with open(os.path.join(root, "app", "recipe"), "a") as f:
        f.write("runs tool\nmulticall bin/app app2\n")
    with open(os.path.join(root, "vulns"), "w") as f:
        f.write("# confirmed\nvuln libb - 1.1 - CVE-2026-1111 libb before 1.1\n"
                "vuln app 2.0 - - CVE-2026-2222 app from 2.0\n")
    return root


def test_packages(tmp, real_ports, key, pub):
    """The fixture ports packaged; returns (tree, builder) for the repo test."""
    root = fixture_tree(tmp, real_ports)
    t, problems = treemod.load(root)
    problems += treemod.lint(t)
    expect(not problems, "packages: the fixture tree: %s" % problems)
    log = []
    b = buildmod.Builder(t, os.getcwd(), os.path.join(tmp, "pk-out"), 4, (), False, log.append)
    try:
        b.build(["app", "tool"])
    except buildmod.BuildError as e:
        FAILS.append("packages: the fixture build failed: %s" % e)
        return None, None
    abi = "00112233445566ff"
    stage = os.path.join(b.base("app"), "stage")
    made = packagemod.make(t.recipes["app"], stage, os.path.join(tmp, "pk1"), abi, key)
    expect([n for n, _ in made] == ["app", "app-extra"], "packages: app makes %s" % made)
    blobs = {}
    for name, path in made:
        with open(path, "rb") as f:
            blobs[name] = f.read()
    m = pkgverify.verify_package(blobs["app"], pub, abi)
    files = [p for p, _, _, _ in m.files]
    expect(files == ["bin/app", "bin/app-a", "bin/app-b", "bin/app2", "share/app/data.txt",
                    "share/licences/app/LICENSE"],
           "packages: app's files: %s" % files)
    expect([p for p, _ in m.programs] == ["bin/app", "bin/app-a", "bin/app-b", "bin/app2"] and
           all(listed == b"posix\nmount /data r\n" for _, listed in m.programs),
           "packages: app's programs and lists: %s" % m.programs)
    expect(m.runs == [("tool", None)], "packages: app's runs: %s" % m.runs)
    expect([w[0] for w in m.built_with] == ["liba", "libb", "musl", "sysroot"],
           "packages: app's built-with: %s" % m.built_with)
    e = pkgverify.verify_package(blobs["app-extra"], pub, abi)
    expect([p for p, _, _, _ in e.files] == ["share/app/alias.txt",
                                             "share/licences/app-extra/LICENSE"]
           and not e.programs and not e.built_with, "packages: app-extra: %s" % e.files)
    expect(packagemod.plan(t.recipes["liba"], os.path.join(b.base("liba"), "stage")) == [],
           "packages: a library port made a package")
    again = packagemod.make(t.recipes["app"], stage, os.path.join(tmp, "pk2"), abi, key)
    b2 = buildmod.Builder(t, os.getcwd(), os.path.join(tmp, "pk-out"), 4, ["app"], False,
                          log.append)
    b2.build(["app"])
    forced = packagemod.make(t.recipes["app"], stage, os.path.join(tmp, "pk3"), abi, key)
    for (n, p1), (_, p2), (_, p3) in zip(made, again, forced):
        with open(p1, "rb") as f1, open(p2, "rb") as f2, open(p3, "rb") as f3:
            a, c, d = f1.read(), f2.read(), f3.read()
        expect(a == c, "packages: %s differs packaged twice" % n)
        expect(a == d, "packages: %s differs after a forced rebuild" % n)
    return t, b


def git_repo(tmp):
    """A small git repository: (its directory, its environment)."""
    d = os.path.join(tmp, "gitrepo")
    os.makedirs(d)
    when = "2026-01-01T00:00:00Z"
    env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", HOME=tmp, GIT_AUTHOR_DATE=when,
               GIT_COMMITTER_DATE=when)
    with open(os.path.join(d, "f.c"), "w") as f:
        f.write("int f;\n")
    for cmd in (["init", "-q"], ["add", "f.c"],
                ["-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "t"]):
        subprocess.run(["git"] + cmd, cwd=d, env=env, check=True, stdout=subprocess.DEVNULL)
    return d, env


def test_tree(tmp):
    if not shutil.which("git"):
        print("ports selftest: no git here: the tree tarball test is skipped")
        return
    d, env = git_repo(tmp)
    repodir = os.path.join(tmp, "tree-repo")
    clean = srctar.tree_archive(d, repodir)
    with tarfile.open(os.path.join(repodir, clean.file)) as t:
        names = t.getnames()
    expect(any(n.endswith("/f.c") for n in names), "tree: the archive's names: %s" % names)
    with open(os.path.join(d, "f.c"), "w") as f:
        f.write("int f, g;\n")
    dirty = srctar.tree_archive(d, repodir)
    expect(dirty.commit != clean.commit and dirty.file != clean.file,
           "tree: a changed tree gave the same commit")
    refs = subprocess.run(["git", "stash", "list"], cwd=d, env=env, stdout=subprocess.PIPE)
    expect(refs.stdout == b"", "tree: a stash entry was made")
    plain = os.path.join(tmp, "plain")
    os.makedirs(plain)
    expect(srctar.tree_archive(plain, repodir).line() == "jamos-tree unknown - - -\n",
           "tree: a directory that isn't a repository")


def check_repo(repodir, pub, last=None):
    """The repository read as pkg would: (Index, Vulns, {name: Manifest})."""
    with open(os.path.join(repodir, "INDEX"), "rb") as f:
        x = pkgverify.verify_index(f.read(), pub, last)
    with open(os.path.join(repodir, "VULNS"), "rb") as f:
        v = pkgverify.verify_vulns(f.read(), pub)
    ms = {}
    for name, p in x.packages.items():
        with open(os.path.join(repodir, fmt.pkg_file(name, p["version"], p["revision"],
                                                     p["sha256"])), "rb") as f:
            data = f.read()
        pkgverify.check_package_in_index(x, name, data)
        ms[name] = pkgverify.verify_package(data, pub, x.abi)
    for port, s in x.sources.items():
        with open(os.path.join(repodir, fmt.src_file(port, s["version"], s["revision"],
                                                     s["sha256"])), "rb") as f:
            data = f.read()
        expect(hashlib.sha256(data).hexdigest() == s["sha256"], "repo: %s's source" % port)
    return x, v, ms


def test_repo(tmp, t, b, key, pub, key2):
    repodir = os.path.join(tmp, "repo")
    quiet = lambda s: None  # noqa: E731
    top = os.getcwd()
    s1 = repomod.make(t, b, top, repodir, key, ["app"], False, now=2000000000, say=quiet)
    x, v, ms = check_repo(repodir, pub)
    expect(s1 == 2000000000 and sorted(x.packages) == ["app", "app-extra", "tool"],
           "repo: serial %d, packages %s" % (s1, sorted(x.packages)))
    expect(sorted(x.sources) == ["app", "tool"], "repo: sources %s" % sorted(x.sources))
    expect(x.packages["app"]["runs"] == [("tool", None)], "repo: app's runs in the index")
    ids = sorted(i for _, i, _ in pkgverify.affected(v, ms["app"]))
    expect(ids == ["CVE-2026-1111"], "repo: audit of app: %s (libb 1.0 inside it)" % ids)
    src = [n for n in os.listdir(os.path.join(repodir, "src")) if n.startswith("app-")]
    with open(os.path.join(repodir, "src", src[0]), "rb") as f:
        names = [p for p, _, _ in ustar.members(f.read())]
    for want in ("app-1.0-r1/distfiles/app-1.0.tar.gz", "app-1.0-r1/port/recipe",
                 "app-1.0-r1/port/build.sh", "app-1.0-r1/shared/config.site",
                 "app-1.0-r1/jamos-tree"):
        expect(want in names, "repo: app's source tarball lacks %s" % want)
    with open(os.path.join(repodir, "INDEX"), "rb") as f:
        old = f.read()
    _refusals(repodir, pub, key2, old, x)
    # The clock turned back: the serial still grows. A change to app's
    # data makes new files; the old ones stay one run, then go.
    first_app = fmt.pkg_file("app", "1.0", 1, x.packages["app"]["sha256"])
    with open(os.path.join(t.root, "app", "recipe"), "a") as f:
        f.write("category test\n")
    b.force = {"app"}
    s2 = repomod.make(t, b, top, repodir, key, ["app"], False, now=1000, say=quiet)
    expect(s2 == s1 + 1, "repo: serial %d after %d with the clock back" % (s2, s1))
    x2, _, _ = check_repo(repodir, pub, (s1, x.sha256))
    refused(lambda: pkgverify.verify_index(old, pub, (x2.serial, x2.sha256)),
            "repo: the old index replayed after the new one")
    expect(os.path.isfile(os.path.join(repodir, first_app)), "repo: the old index's package "
           "removed while that index is the last one")
    s3 = repomod.make(t, b, top, repodir, key, ["app"], False, now=3000000000, say=quiet)
    expect(s3 == 3000000000, "repo: serial %d with the clock forward" % s3)
    check_repo(repodir, pub, (s2, x2.sha256))
    same = x2.packages["app"]["sha256"] == x.packages["app"]["sha256"]
    expect(same or not os.path.isfile(os.path.join(repodir, first_app)),
           "repo: a package no index names was kept")


def _refusals(repodir, pub, key2, index, x):
    name = fmt.pkg_file("tool", x.packages["tool"]["version"], x.packages["tool"]["revision"],
                        x.packages["tool"]["sha256"])
    with open(os.path.join(repodir, name), "rb") as f:
        data = bytearray(f.read())
    data[len(data) // 2] ^= 0x20
    refused(lambda: pkgverify.check_package_in_index(x, "tool", bytes(data)),
            "repo: a package changed in the repository (the index)")
    refused(lambda: pkgverify.verify_package(bytes(data), pub),
            "repo: a package changed in the repository (its manifest)")
    refused(lambda: pkgverify.verify_index(index.replace(b"\npackage app ", b"\npackage app  "),
                                           pub), "repo: the index changed")
    with open(os.path.join(repodir, "VULNS"), "rb") as f:
        vulns = f.read()
    refused(lambda: pkgverify.verify_index(vulns, pub), "repo: VULNS read as an index")
    refused(lambda: pkgverify.verify_vulns(index, pub), "repo: the index read as VULNS")
    relabelled = fmt.INDEX_FIRST.encode() + vulns[len(fmt.VULNS_FIRST):]
    refused(lambda: pkgverify.verify_index(relabelled, pub),
            "repo: VULNS relabelled as an index (the signature over another first line)")
    manifest = ustar.members(bytes(data))[0][2]
    refused(lambda: pkgverify.verify_index(manifest, pub), "repo: a manifest read as an index")
    other = signmod.sign(index[:index.rindex(b"signature ")] + b"signature\n", fmt.INDEX_FIRST,
                         key2)
    refused(lambda: pkgverify.verify_index(other, pub), "repo: an index signed by another key")
    expect(pkgverify.verify_index(index, pub, (x.serial - 1, "")) is not None,
           "repo: the index after an older serial")


def run(tmp, real_ports, top):
    """Every test above; the failures."""
    del FAILS[:]
    tmp = os.path.join(tmp, "pkgtest")
    os.makedirs(tmp)
    key, pub = signmod.test_key(os.path.join(tmp, "keys"))
    key2, _ = signmod.test_key(os.path.join(tmp, "keys2"), "ports self-test, another")
    test_ustar(tmp)
    try:
        t, b = test_packages(tmp, real_ports, key, pub)
        program = os.path.join(b.base("app"), "stage", "bin", "app") if b else None
        test_pagetable(tmp, top, program)
        if t:
            test_repo(tmp, t, b, key, pub, key2)
        test_tree(tmp)
    except (pkgverify.Refused, packagemod.PackageError, repomod.RepoError,
            signmod.SignError, buildmod.BuildError) as e:
        FAILS.append("packages: %s: %s" % (type(e).__name__, e))
    return list(FAILS)
