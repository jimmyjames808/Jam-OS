"""The builder's self-test (`ports.py selftest`, in `make check`): offline,
no QEMU, a few seconds. docs/M13.7-PLAN.md section 14 lists what it must
show; each check below is one of them.

The pieces alone: licence expressions, recipe and tree lint (a missing
licence, an unknown one, a bad list, a cycle, a patch with no header),
the patch applier (fuzz refused, offsets and new files taken), hostile
archives, the FAT rules, the override check's new-file form, fetching (a
bad hash, a bad size, the mirror).

Then real builds of fixtures.py's fake ports with the real compiler and
sysroot: the order, a library's patch seen by its user, the transitive
link, a program's list in its note, what is dropped, a symlink made a
copy, isolation (a header of a port not declared is not found), a no-op
build that builds nothing, a needed port's change rebuilding its users,
and two builds giving the same bytes.

Then the packages, the repository and their formats (pkgtest.py).

Then a kept tree (`tree kept`, kept.py): a recipe's change leaves the
source and the build's output alone, a new patch rewrites only the file it
touches and dropping it brings the file back, --clean and a new distfile
start afresh; and `package --from` packages a build from elsewhere, refused
when the recipe that built it is not this one.
"""
import argparse
import contextlib
import io
import os
import re
import shutil
import subprocess
import tarfile
import tempfile

from . import boot as bootmod
from . import build as buildmod
from . import fetch as fetchmod
from . import fixtures
from . import licence
from . import patch as patchmod
from . import pkgcmd
from . import pkgtest
from . import recipe as recipemod
from . import selftest_hostdata
from . import source as sourcemod
from . import stage as stagemod
from . import tree as treemod

FAILS = []


def expect(cond, what):
    if not cond:
        FAILS.append(what)


def refused(fn, words, what):
    """fn() must raise, and its message must hold every word in words."""
    try:
        fn()
    except Exception as e:   # noqa: BLE001  (any refusal; the words say which)
        msg = str(e)
        expect(all(w in msg for w in words), "%s: refused, but said %r" % (what, msg))
        return
    FAILS.append("%s: not refused" % what)


def test_licences():
    for good in ("MIT", "Zlib", "MIT OR Apache-2.0", "(MIT AND BSD-2-Clause) OR GPL-2.0-only",
                 "GPL-3.0-or-later WITH GCC-exception-3.1",
                 "GPL-2.0-only WITH Linux-syscall-note"):
        try:
            licence.parse(good)
        except ValueError as e:
            FAILS.append("licence %r refused: %s" % (good, e))
    for bad in ("", "mit", "Public-Domain", "LicenseRef-mine", "GPL-2.0+", "MIT AND",
                "MIT WITH Nothing", "(MIT", "MIT BSD-2-Clause", "AGPL-3.0-only"):
        refused(lambda b=bad: licence.parse(b), [], "licence %r" % bad)
    expect(licence.binds_lgpl("LGPL-2.1-only"), "LGPL alone binds")
    expect(not licence.binds_lgpl("LGPL-2.1-only OR MIT"), "LGPL OR MIT doesn't bind")
    expect(licence.copyleft("GPL-2.0-only AND MIT"), "GPL AND MIT is copyleft")


def lint_of(root, name, text, extra_files=None):
    """The tree's lint problems with port name's recipe replaced by text."""
    with open(os.path.join(root, name, "recipe"), "w") as f:
        f.write(text)
    for rel, data in (extra_files or {}).items():
        path = os.path.join(root, name, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(data)
    t, problems = treemod.load(root)
    return problems + treemod.lint(t)


def test_lint(tmp, real_ports):
    root, up = os.path.join(tmp, "lint"), os.path.join(tmp, "lint-up")
    fixtures.write_tree(root, up, real_ports)
    t, problems = treemod.load(root)
    problems += treemod.lint(t)
    expect(not problems, "the good fixture tree fails its lint: %s" % problems)
    with open(os.path.join(root, "liba", "recipe")) as f:
        good = f.read()
    cases = [
        (good.replace("licence MIT\n", ""), ["no 'licence' line"], "a recipe without a licence"),
        (good.replace("licence MIT", "licence Mine-1.0"), ["not a licence the builder accepts"],
         "an unknown licence"),
        (good.replace("licence-file LICENSE\n", ""), ["no 'licence-file' line"],
         "a recipe without its licence file"),
        (good + "colour blue\n", ["not a recipe key"], "an unknown key"),
        (good + "needs nothere\n", ["no such port"], "a needs of no recipe"),
        (good + "needs libb\n", ["a cycle", "liba -> libb", " -> "], "a cycle"),
        (good + "configure --x\n", ["for style"], "arguments for another style"),
        (re.sub(r"(\.tar\.gz) \d+", r"\1 x", good), ["want 'source"], "a source with no size"),
        (re.sub(r" [0-9a-f]{64}\n", "\n", good), ["'source' takes 3 to 4 words"],
         "a source with no hash"),
        (good + "program bin/a wants/nope\n", ["not in the port's directory"],
         "a list that isn't there"),
        (good + "configure-x 1\n", ["not a recipe key"], "a misspelt key"),
        (good + "toolchain gcc\n", ["toolchain must be one of"], "an unknown toolchain"),
        (good + "toolchain clang\n", ["needs llvm-runtimes"],
         "a clang port without the runtimes"),
        (good + "toolchain rust\n", ["toolchain takes", "clang rust"], "Rust without clang"),
        (good + "toolchain clang rust\n", ["needs llvm-runtimes"],
         "a Rust port without the runtimes"),
        (good + "image-base 0x400000\n", ["image-base takes"], "an image base below 4 GiB"),
        (good + "image-base 0x100000800\n", ["image-base takes"], "an image base not on a page"),
        (good + "image-base 4294967296\n", ["image-base takes"], "an image base not in hex"),
        (good + "image-base 0x400000000000\n", ["image-base takes"], "an image base too high"),
        (good + "local files\n", ["'local' line", "more than one"], "a source and a local one"),
        (re.sub(r"(?m)^source .*\n", "", good) + "local nothere\n", ["local:", "not there"],
         "a local source that isn't there"),
        (re.sub(r"(?m)^source .*\n", "", good), ["'source' lines", "none"], "no source at all"),
    ]
    for text, words, what in cases:
        problems = lint_of(root, "liba", text)
        expect(any(all(w in p for w in words) for p in problems),
               "lint: %s not refused (%s)" % (what, problems))
    lint_of(root, "liba", good)
    session = good + "program bin/a\n"
    problems = lint_of(root, "liba", session, {"wants/a": "posix\nsvc session\n"})
    expect(any("svc session" in p for p in problems), "a package's list with svc session taken")
    problems = lint_of(root, "liba", session, {"wants/a": "mount /data r\n"})
    expect(any("no 'posix'" in p for p in problems), "a list without posix taken")
    os.remove(os.path.join(root, "liba", "wants", "a"))
    # A wants/<name> no program line reaches is never used (cc_wrapper takes
    # a list only for a program the recipe names): refused; named by a
    # program of that name, or by another program line's second word, taken.
    orphan = {"wants/orphan": "posix\nmount /data r\n"}
    problems = lint_of(root, "liba", good, orphan)
    expect(any("wants/orphan" in p and "no 'program' line" in p for p in problems),
           "a wants file no program line names taken: %s" % problems)
    problems = lint_of(root, "liba", good + "program bin/orphan\n", orphan)
    expect(not problems, "a wants file named by its program refused: %s" % problems)
    problems = lint_of(root, "liba", good + "program bin/other wants/orphan\n", orphan)
    expect(not problems, "a wants file named by a program line's list refused: %s" % problems)
    os.remove(os.path.join(root, "liba", "wants", "orphan"))
    for text, what in ((session + "multicall bin/a x x\n", "a multicall name twice"),
                       (session + "multicall bin/a a\n", "a multicall name that is the program")):
        problems = lint_of(root, "liba", text)
        expect(any("given twice" in p for p in problems), "%s taken" % what)
    problems = lint_of(root, "liba", session + "multicall bin/a x y\n",
                       {"files/Makefile": "all:\n"})
    expect(not problems, "a port with files/ and multicall refused: %s" % problems)
    shutil.rmtree(os.path.join(root, "liba", "files"))
    # A port of Jam OS's own code: its source is files/ (`local`), copied
    # into the work tree as a vendored one is.
    local = re.sub(r"(?m)^source .*\n", "", good) + "local files\n"
    problems = lint_of(root, "liba", local, {"files/a.c": "int a;\n"})
    expect(not problems, "a port with a local source refused: %s" % problems)
    own = os.path.join(tmp, "local-port", "own")   # (no patches: liba's are upstream's)
    os.makedirs(os.path.join(own, "files"))
    with open(os.path.join(own, "recipe"), "w") as f:
        f.write("name own\nlocal files\n")
    with open(os.path.join(own, "files", "a.c"), "w") as f:
        f.write("int a;\n")
    r, _ = recipemod.parse(os.path.join(own, "recipe"))
    srcdir = sourcemod.prepare(r, [], os.path.join(tmp, "local-work"), say=lambda s: None)
    expect(os.path.isfile(os.path.join(srcdir, "a.c")), "a local source not copied: %s" % srcdir)
    shutil.rmtree(os.path.join(root, "liba", "files"))
    lint_of(root, "liba", good)
    problems = lint_of(root, "liba", good, {"patches/0002-x.patch": "--- a/a\n+++ b/a\n"})
    expect(any("Description" in p for p in problems), "a patch with no header taken")
    os.remove(os.path.join(root, "liba", "patches", "0002-x.patch"))


def test_patches(tmp):
    d = os.path.join(tmp, "patch")
    os.makedirs(d)
    src = "one\ntwo\nthree\nfour\nfive\n"

    def run(diff, text=src):
        with open(os.path.join(d, "f.txt"), "w") as f:
            f.write(text)
        with open(os.path.join(tmp, "p.patch"), "w") as f:
            f.write("Description: t\nForwarded: no\n" + diff)
        patchmod.apply(os.path.join(tmp, "p.patch"), d)
        with open(os.path.join(d, "f.txt")) as f:
            return f.read()
    good = "--- a/f.txt\n+++ b/f.txt\n@@ -2,3 +2,3 @@\n two\n-three\n+THREE\n four\n"
    expect(run(good) == src.replace("three", "THREE"), "a clean patch")
    expect(run(good, "zero\n" + src) == "zero\n" + src.replace("three", "THREE"),
           "a patch at an offset (exact context)")
    fuzzy = good.replace(" four\n", " fourr\n")
    refused(lambda: run(fuzzy), ["doesn't apply exactly"], "a patch that needs fuzz")
    refused(lambda: run(good.replace("a/f.txt", "a/../x")), ["not a plain path"],
            "a patch with '..'")
    new = "--- /dev/null\n+++ b/sub/new.c\n@@ -0,0 +1,2 @@\n+int x;\n+int y;\n"
    run(new)
    with open(os.path.join(d, "sub", "new.c")) as f:
        expect(f.read() == "int x;\nint y;\n", "a patch that adds a file")
    nonl = "--- a/f.txt\n+++ b/f.txt\n@@ -5 +5 @@\n-five\n+FIVE\n\\ No newline at end of file\n"
    expect(run(nonl) == src.replace("five\n", "FIVE"), "a patch that drops the last newline")
    refused(lambda: run("--- a/f.txt\n+++ b/f.txt\nold mode 100644\n"), ["override"],
            "a mode change")


def test_archives(tmp):
    def tar_with(members):
        path = os.path.join(tmp, "evil.tar")
        with tarfile.open(path, "w") as tar:
            for name, kind, target in members:
                info = tarfile.TarInfo(name)
                info.type = kind
                info.linkname = target
                tar.addfile(info, io.BytesIO(b"") if kind == tarfile.REGTYPE else None)
        dest = os.path.join(tmp, "evil-out")
        shutil.rmtree(dest, ignore_errors=True)
        os.makedirs(dest)
        sourcemod.unpack(path, dest)
    reg, sym, lnk = tarfile.REGTYPE, tarfile.SYMTYPE, tarfile.LNKTYPE
    refused(lambda: tar_with([("../up", reg, "")]), ["'..'"], "a member with '..'")
    refused(lambda: tar_with([("/abs", reg, "")]), ["absolute"], "an absolute member")
    refused(lambda: tar_with([("d/l", sym, "../../etc")]), ["out of the tree"],
            "a symlink out of the tree")
    refused(lambda: tar_with([("l", sym, "d"), ("l/x", reg, "")]), ["through a symlink"],
            "a member through a symlink")
    refused(lambda: tar_with([("dev", tarfile.CHRTYPE, "")]), ["device"], "a device")
    tar_with([("d/f", reg, ""), ("d/l", sym, "f"), ("d/h", lnk, "d/f")])


def test_fat():
    bad = ["a:b", "x/what?", "trail.", "trail ", "tab\tname", "pipe|x", "q\"uote",
           "%s" % ("n" * 256)]
    for name in bad:
        expect(stagemod.fat_problems([(name, 1)]), "FAT: %r taken" % name)
    expect(stagemod.fat_problems([("share/A/x", 1), ("share/a/y", 1)]), "FAT: case in a directory")
    expect(stagemod.fat_problems([("Makefile", 1), ("makefile", 1)]), "FAT: names apart by case")
    expect(stagemod.fat_problems([("big", 1 << 32)]), "FAT: a file over 4 GiB")
    expect(not stagemod.fat_problems([("bin/vim", 9), ("share/vim/vim91/x.vim", 1),
                                      ("share/terminfo/78/xterm", 2)]), "FAT: good names refused")


def test_fetch(tmp, up):
    path = os.path.join(up, "liba-1.0.tar.gz")
    size, digest = os.path.getsize(path), fetchmod.sha256_file(path)
    url = "file://" + path
    quiet = lambda s: None  # noqa: E731
    got = fetchmod.fetch(recipemod.Source("url", url, size, digest, "liba-1.0.tar.gz"), quiet)
    expect(fetchmod.sha256_file(got) == digest, "fetch: the cached file")
    expect(os.path.isfile(os.path.join(tmp, "mirror", "liba-1.0.tar.gz")), "fetch: mirror filled")
    bad = recipemod.Source("url", url, size, "0" * 64, "bad-hash.tar.gz")
    refused(lambda: fetchmod.fetch(bad, quiet), ["sha256"], "a distfile with another hash")
    expect(not os.path.exists(os.path.join(tmp, "cache", "bad-hash.tar.gz")),
           "a distfile that failed its hash was kept")
    short = recipemod.Source("url", url, size + 1, digest, "bad-size.tar.gz")
    refused(lambda: fetchmod.fetch(short, quiet), ["bytes"], "a distfile of another size")
    shutil.copyfile(path, os.path.join(tmp, "mirror", "gone.tar.gz"))
    gone = recipemod.Source("url", "file:///nonexistent/gone.tar.gz", size, digest, "gone.tar.gz")
    expect(fetchmod.fetch(gone, quiet), "fetch: a vanished upstream from the mirror")


def builder(tree, tmp, force=()):
    log = []
    b = buildmod.Builder(tree, os.getcwd(), os.path.join(tmp, "out"), 4, force, False,
                         log.append)
    return b, log


def archive_zeroed(path):
    """Whether every member of an ar archive has time, owner and group 0
    (what `ar D` writes, so the bytes don't depend on the machine)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"!<arch>\n":
        return False
    at = 8
    while at + 60 <= len(data):
        head = data[at:at + 60]
        if int(head[16:28]) or int(head[28:34]) or int(head[34:40]):
            return False
        at += 60 + int(head[48:58])
        at += at & 1
    return True


def stage_bytes(tmp, name):
    with open(os.path.join(tmp, "out", name, "stage", "stage.txt")) as f:
        return f.read()


def test_builds(tmp, real_ports):
    root, up = os.path.join(tmp, "ports"), os.path.join(tmp, "up")
    fixtures.write_tree(root, up, real_ports)
    t, problems = treemod.load(root)
    expect(not problems, "the fixture tree doesn't load: %s" % problems)
    expect(treemod.order(t, ["app"]) == ["liba", "libb", "app"], "the order app needs")
    b, _ = builder(t, tmp)
    try:
        done = b.build(["app", "libx", "bootapp"])
    except buildmod.BuildError as e:
        FAILS.append("the fixture build failed: %s" % e)
        return
    expect([n for n, s in done if s == "built"] == ["liba", "libb", "app", "libx", "bootapp"],
           "built in order: %s" % done)
    lines = stagemod.read_list(os.path.join(tmp, "out", "app", "stage", "stage.txt"))
    kinds = {(w[0], w[1]) for w in lines}
    expect(("program", "bin/app") in kinds, "app's program staged")
    expect(not any(w[1] == "bin/helper" for w in lines), "an unnamed executable kept")
    expect(["copy", "share/app/alias.txt", "share/app/data.txt"] in lines, "a symlink as a copy")
    expect(["sub", "app-extra", "share/app/alias.txt"] in lines, "a subpackage's file")
    expect(["multicall", "bin/app", "app-a"] in lines and ["multicall", "bin/app", "app-b"] in lines,
           "multicall lines in the stage list")
    expect(not os.path.exists(os.path.join(tmp, "out", "app", "stage", "bin", "app-a")),
           "a multicall name made a file in the stage (the packager makes it)")
    expect(["built-with", "liba", "1.0", "1"] in lines and ["built-with", "libb", "1.0", "1"]
           in lines, "app's built-with lines")
    app = os.path.join(tmp, "out", "app", "stage", "bin", "app")
    expect(not stagemod.program_problems(app, b"posix\nmount /data r\n", "package", "x"),
           "app's list as its recipe chose")
    expect(stagemod.program_problems(app, b"posix\n", "package", "x"), "another list taken")
    expect(stagemod.program_problems(os.path.join(tmp, "out", "app", "stage", "share", "app",
                                                  "data.txt"), b"posix\n", "package", "x"),
           "a text file taken as a program")
    with open(os.path.join(tmp, "out", "app", "dropped.txt")) as f:
        expect("helper  (an executable with no program line)" in f.read(), "the drop named")
    expect(archive_zeroed(os.path.join(tmp, "out", "liba", "stage", "lib", "liba.a")),
           "liba.a's members carry a time or an owner (ar rc without D)")
    sysroot = os.path.join(tmp, "out", "app", "sysroot", "include")
    expect(os.path.isfile(os.path.join(sysroot, "a.h")) and
           not os.path.exists(os.path.join(sysroot, "x.h")), "app's sysroot: a.h, no x.h")
    entries = {n: (f, h) for n, f, h in bootmod.bootlist(os.path.join(tmp, "out"), ["bootapp"])}
    expect(sorted(entries) == ["share/licences/bootapp/LICENSE", "usr/bin/boxy", "usr/bin/tool1",
                               "usr/bin/tool2"], "the boot list's names: %s" % sorted(entries))
    expect(len({entries[n] for n in ("usr/bin/boxy", "usr/bin/tool1", "usr/bin/tool2")}) == 1,
           "a multicall name is the program's own file and hash")
    expect(bootmod.boot_ports(t) == ["bootapp"], "boot_ports: %s" % bootmod.boot_ports(t))
    refused(lambda: bootmod.bootlist(os.path.join(tmp, "out"), ["bootapp", "bootapp"]),
            ["usr/bin/boxy", "also"], "one port twice in the boot list")
    refused(lambda: b.build(["sneaky"]), ["x.h"], "a header of a port not declared")
    before = stage_bytes(tmp, "app")
    b, _ = builder(t, tmp)
    done = b.build(["app"])
    expect(all(s == "up to date" for _, s in done), "a no-op build built: %s" % done)
    # A stage built for another prefix is out of date though no input of the stamp
    # changed (PO14 moved a package's prefix from /data/pkg to /pkg); a port that
    # keeps its prefix (recipe.KEEP_PREFIX: Chromium) is not.
    listing = os.path.join(tmp, "out", "app", "stage", "stage.txt")
    with open(listing) as f:
        text = f.read()
    expect(" package /pkg\n" in text, "app's stage list names its prefix: %s" % text.split("\n")[1])
    with open(listing, "w") as f:
        f.write(text.replace(" package /pkg\n", " package /data/pkg\n", 1))
    b, _ = builder(t, tmp)
    done = b.build(["app"])
    expect([s for n, s in done if n == "app"] == ["built"] and
           all(s == "up to date" for n, s in done if n != "app"),
           "a stage of another prefix wasn't built again, alone: %s" % done)
    recipemod.KEEP_PREFIX["app"] = "/data/pkg"
    try:
        with open(listing, "w") as f:
            f.write(text.replace(" package /pkg\n", " package /data/pkg\n", 1))
        b, _ = builder(t, tmp)
        expect(all(s == "up to date" for _, s in b.build(["app"])),
               "a port kept on /data/pkg was built again")
    finally:
        del recipemod.KEEP_PREFIX["app"]
    b, _ = builder(t, tmp, ["app"])
    b.build(["app"])
    expect(stage_bytes(tmp, "app") == before, "two builds of app differ")
    with open(os.path.join(root, "liba", "recipe"), "a") as f:
        f.write("category test\n")
    t, _ = treemod.load(root)
    b, _ = builder(t, tmp)
    done = b.build(["app"])
    expect([s for _, s in done] == ["built"] * 3, "liba's change rebuilt %s" % done)


def bad_build(tmp, real_ports, case, port, spoil, words, what):
    """Build port in a fresh fixture tree spoiled by spoil(root): it must
    be refused, saying words."""
    root = os.path.join(tmp, "bad-" + case)
    fixtures.write_tree(root, os.path.join(tmp, "bad-up-" + case), real_ports)
    spoil(root)
    t, _ = treemod.load(root)
    b, _ = builder(t, root + "-out")
    refused(lambda: b.build([port]), words, what)


def rewrite(path, old, new):
    with open(path) as f:
        text = f.read()
    with open(path, "w") as f:
        f.write(text.replace(old, new))


def test_bad_builds(tmp, real_ports):
    bad_build(tmp, real_ports, "patch", "liba",
              lambda root: rewrite(os.path.join(root, "liba", "patches", "0001-mark.patch"),
                                   " int a(void);", " int a(int);"),
              ["doesn't apply exactly"], "a patch that doesn't apply")
    bad_build(tmp, real_ports, "licence", "libx",
              lambda root: rewrite(os.path.join(root, "libx", "recipe"),
                                   "licence-file LICENSE", "licence-file NOTICE"),
              ["NOTICE", "not in the source"], "a licence file that isn't there")
    bad_build(tmp, real_ports, "override", "libx",
              lambda root: fixtures.overrides(root, "0" * 64),
              ["x.c changed"], "an override of an upstream file that changed")
    bad_build(tmp, real_ports, "newfile", "libx",
              lambda root: rewrite(os.path.join(root, "libx", "OVERRIDES"),
                                   "extra.h - ", "x.h - "),
              ["now has x.h"], "a new file upstream now has")
    bad_build(tmp, real_ports, "list", "app",
              lambda root: rewrite(os.path.join(root, "app", "build.sh"),
                                   "-o app app.c -lb -la", "-o app.tmp app.c -lb -la\n    mv app.tmp app"),
              ["program bin/app", "the recipe chose"], "a program linked with another list")


KEPT_BUILD = """# big's build in a kept tree: its output in out/, inside the source.
plan() {
    mkdir -p out
    echo plan >> out/plans
}

build() {
    mkdir -p out
    echo run >> out/runs
    "$CC" -O2 -c x.c -o out/x.o
    "$CC" -O2 -c y.c -o out/y.o
    "$CC" -O2 -o out/big out/x.o out/y.o
}

install() {
    mkdir -p "$destdir$prefix/bin"
    cp out/big "$destdir$prefix/bin/"
}
"""

KEPT_PATCH = """Description: y returns 3, so the test can tell
Forwarded: not-needed (a test)
--- a/y.c
+++ b/y.c
@@ -1 +1 @@
-int y(void) { return 2; }
+int y(void) { return 3; }
"""


def test_kept(tmp, real_ports):
    """A kept tree's rules (kept.py), then package --from (pkgcmd.py)."""
    root, up = os.path.join(tmp, "kept-ports"), os.path.join(tmp, "kept-up")
    fixtures.write_tree(root, up, real_ports)
    files = {"x.c": "int y(void);\nint main(void) { return y(); }\n",
             "y.c": "int y(void) { return 2; }\n", "LICENSE": "big: MIT, for the test\n"}
    tarball = os.path.join(up, "big-1.0.tar.gz")
    d = os.path.join(root, "big")
    os.makedirs(d)

    def write_recipe(extra=()):
        fixtures.make_tar(tarball, "big-1.0", files)
        with open(os.path.join(d, "recipe"), "w") as f:
            f.write(fixtures.recipe("big", "custom", "MIT", fixtures.source_line(tarball),
                                    ["tree kept", "program bin/big"] + list(extra)))
    write_recipe()
    with open(os.path.join(d, "build.sh"), "w") as f:
        f.write(KEPT_BUILD)
    kdir = os.path.join(tmp, "kept-tree")
    src = os.path.join(kdir, "src", "big-1.0")

    def build(clean=False, plan=False):
        t, problems = treemod.load(root)
        problems += treemod.lint(t, ["big"])
        expect(not problems, "the kept fixture doesn't load: %s" % problems)
        b = buildmod.Builder(t, os.getcwd(), os.path.join(tmp, "kept-out"), 4, ["big"] if plan else (), False,
                             lambda s: None, kept=kdir, clean=["big"] if clean else (), plan=plan)
        try:
            b.build(["big"])
        except buildmod.BuildError as e:
            FAILS.append("kept: the build failed: %s" % e)
            return False
        return True

    def runs():
        try:
            with open(os.path.join(src, "out", "runs")) as f:
                return len(f.read().split())
        except OSError:
            return 0

    def plans():
        try:
            with open(os.path.join(src, "out", "plans")) as f:
                return len(f.read().split())
        except OSError:
            return 0

    def mtime(*parts):
        return os.stat(os.path.join(*parts)).st_mtime_ns

    def text(name):
        with open(os.path.join(src, name)) as f:
            return f.read()

    if not build():
        return
    expect(runs() == 1, "kept: the first build ran %d times" % runs())
    x0, y0 = mtime(src, "x.c"), mtime(src, "y.c")
    libc0 = mtime(kdir, "sysroot", "lib", "libc.a")
    cc0 = mtime(kdir, "bin", "x86_64-linux-musl-gcc")
    write_recipe(["category test"])
    build()
    expect(runs() == 2, "kept: a recipe's change lost the build's output (runs: %d)" % runs())
    expect((mtime(src, "x.c"), mtime(src, "y.c")) == (x0, y0),
           "kept: a recipe's change touched the source")
    expect(mtime(kdir, "sysroot", "lib", "libc.a") == libc0 and
           mtime(kdir, "bin", "x86_64-linux-musl-gcc") == cc0,
           "kept: an unchanged sysroot file or tool was written again")
    os.makedirs(os.path.join(d, "patches"))
    with open(os.path.join(d, "patches", "0001-three.patch"), "w") as f:
        f.write(KEPT_PATCH)
    build()
    expect("return 3" in text("y.c"), "kept: a new patch was not applied")
    expect(mtime(src, "x.c") == x0, "kept: a patch to y.c rewrote x.c")
    expect(runs() == 3, "kept: a patch's change lost the build's output")
    os.remove(os.path.join(d, "patches", "0001-three.patch"))
    build()
    expect(text("y.c") == files["y.c"], "kept: a dropped patch's file is not upstream's again")
    expect(mtime(src, "x.c") == x0 and runs() == 4, "kept: dropping a patch touched more")
    with open(os.path.join(d, "patches", "0001-bad.patch"), "w") as f:
        f.write(KEPT_PATCH.replace("return 2;", "return 5;"))
    nfails = len(FAILS)
    build()
    expect(len(FAILS) == nfails + 1 and "doesn't apply" in FAILS[-1],
           "kept: a patch that doesn't apply was taken")
    del FAILS[nfails:]
    os.remove(os.path.join(d, "patches", "0001-bad.patch"))
    build()
    expect(runs() == 5 and mtime(src, "x.c") == x0,
           "kept: a patch that didn't apply cost the tree (runs: %d)" % runs())
    build(clean=True)
    expect(runs() == 1, "kept: --clean kept the build's output (runs: %d)" % runs())
    # --plan: the kept tree brought up to date (the new patch applied) and the recipe's
    # plan() run, nothing built or staged: the last build's stamp and stage stay.
    stamp = os.path.join(tmp, "kept-out", "big", "stamp")
    stage = os.path.join(tmp, "kept-out", "big", "stage")
    with open(stamp) as f:
        stamp0 = f.read()
    stage0 = sorted(os.listdir(stage))
    with open(os.path.join(d, "patches", "0001-three.patch"), "w") as f:
        f.write(KEPT_PATCH)
    expect(plans() == 0, "kept: plan() ran in a build")
    build(plan=True)
    expect(plans() == 1 and runs() == 1, "kept: --plan ran plan() %d times and build() %d times" % (plans(), runs()))
    expect("return 3" in text("y.c"), "kept: --plan did not bring the tree up to date")
    with open(stamp) as f:
        stamp1 = f.read()
    expect(stamp1 == stamp0 and sorted(os.listdir(stage)) == stage0, "kept: --plan touched the stamp or the stage")
    expect(os.path.exists(os.path.join(tmp, "kept-out", "big", "plan.log")), "kept: no plan.log")
    os.remove(os.path.join(d, "patches", "0001-three.patch"))
    build()
    expect(runs() == 2 and text("y.c") == files["y.c"], "kept: the build after a plan (runs: %d)" % runs())
    files["x.c"] = files["x.c"].replace("return y();", "return y() + 1;")
    write_recipe(["category test"])
    build()
    expect(runs() == 1 and "y() + 1" in text("x.c"),
           "kept: a new distfile was not unpacked afresh (runs: %d)" % runs())

    # package --from: the build directory copied elsewhere and packaged
    # there without building; a build of another recipe refused.
    there = os.path.join(tmp, "kept-fetched", "big")
    shutil.copytree(os.path.join(tmp, "kept-out", "big"), there)
    args = argparse.Namespace(ports=root, out=os.path.join(tmp, "kept-out-mac"), jobs=None,
                              force=False, verbose=False, port=["big"], from_dir=there,
                              pkgdir=os.path.join(tmp, "kept-pkg"), key=None, test_key=True,
                              unsigned=False)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = pkgcmd.package(os.getcwd(), args)
    expect(rc == 0 and ".jpk" in out.getvalue(), "package --from: %s" % out.getvalue().strip())
    expect(not os.path.exists(os.path.join(tmp, "kept-out-mac", "big")),
           "package --from built the port")
    write_recipe(["category test", "homepage https://example.org/"])
    t, _ = treemod.load(root)
    refused(lambda: pkgcmd.from_stage(t.recipes["big"], there), ["another recipe"],
            "package --from a build of another recipe")
    write_recipe(["category test"])
    os.remove(os.path.join(there, "stage", "stage.txt"))
    t, _ = treemod.load(root)
    refused(lambda: pkgcmd.from_stage(t.recipes["big"], there), ["stage.txt"],
            "package --from a build without its stage")


def test_git(tmp):
    """A git source: archived by commit, the same bytes from the cache the
    second time, and a different recorded hash refused."""
    if not shutil.which("git"):
        print("ports selftest: no git here: the git source test is skipped")
        return
    repo = os.path.join(tmp, "repo")
    os.makedirs(repo)
    with open(os.path.join(repo, "f.c"), "w") as f:
        f.write("int f;\n")
    env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", HOME=tmp)
    for cmd in (["init", "-q"], ["add", "f.c"],
                ["-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "t"]):
        subprocess.run(["git"] + cmd, cwd=repo, env=env, check=True, stdout=subprocess.DEVNULL)
    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo, env=env, check=True,
                            stdout=subprocess.PIPE).stdout.decode().strip()
    src = recipemod.Source("git", "file://" + repo, None, commit, "t-git.tar")
    quiet = lambda s: None  # noqa: E731
    first = fetchmod.fetch(src, quiet)
    with tarfile.open(first) as tar:
        expect("t-git/f.c" in tar.getnames(), "git: the archive's names")
    expect(fetchmod.fetch(src, quiet) == first, "git: the second fetch")
    for d in (fetchmod.cache_dir(), fetchmod.mirror_dir()):
        with open(os.path.join(d, "t-git.tar.sha256"), "w") as f:
            f.write("0" * 64 + "\n")
    os.remove(first)
    refused(lambda: fetchmod.fetch(src, quiet), ["the same commit must give the same bytes"],
            "git: an archive unlike the one recorded")


def main(top):
    os.chdir(top)
    real_ports = os.path.join(top, "ports")
    saved = {k: os.environ.get(k) for k in ("JAMOS_DISTFILES", "JAMOS_DISTFILES_MIRROR",
                                            "JAMOS_MIRROR_URL", "JAMOS_OFFLINE")}
    os.environ["JAMOS_OFFLINE"] = "1"
    os.environ.pop("JAMOS_MIRROR_URL", None)
    try:
        with tempfile.TemporaryDirectory(prefix="ports-selftest.") as tmp:
            os.environ["JAMOS_DISTFILES"] = os.path.join(tmp, "cache")
            os.environ["JAMOS_DISTFILES_MIRROR"] = os.path.join(tmp, "mirror")
            test_licences()
            test_patches(tmp)
            test_archives(tmp)
            test_fat()
            test_lint(tmp, real_ports)
            test_builds(tmp, real_ports)
            FAILS.extend(pkgtest.run(tmp, real_ports, top))
            test_fetch(tmp, os.path.join(tmp, "up"))
            test_bad_builds(tmp, real_ports)
            test_kept(tmp, real_ports)
            test_git(tmp)
            selftest_hostdata.run(tmp, real_ports, expect, refused,
                                  lambda t, d: builder(t, d))
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    for f in FAILS:
        print("ports selftest: " + f)
    print("ports selftest: %s" % ("FAIL" if FAILS else "PASS"))
    return 1 if FAILS else 0
