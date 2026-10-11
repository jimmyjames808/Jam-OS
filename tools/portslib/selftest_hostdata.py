"""The self-test of host builds and data ports (hostdata.py), run from
selftest.py: the lint of both, a host tool built and used by a port,
isolation (a port that did not name the tool does not see it), a data
port built, a program in a subpackage, and the compiler wrapper dropping
-pthread. Fake ports, as fixtures.py's, built with the real compiler and
sysroot.

    hosttool   place host: a small C program built natively, installed
               into its own stage (its prefix); prints a C source line
    usetool    needs libz-like nothing; `tool hosttool`: its build runs
               hosttool to make a source file, compiles it with -pthread,
               and keeps the program in a subpackage as well
    blind      the same build, without `tool`: must not find hosttool
    datapkg    style data: two `data` lines and the `files` that keep them
"""
import os
import subprocess
import tarfile
import tempfile

from . import build as buildmod
from . import fixtures
from . import hostdata
from . import licence
from . import recipe as recipemod
from . import source as sourcemod
from . import stage as stagemod
from . import tree as treemod

SOURCES = {
    "hosttool-1.0": {
        "hosttool.c": ("#include <stdio.h>\nint main(int argc, char **argv)\n{\n"
                       "    printf(\"const char *%s = \\\"made by hosttool\\\";\\n\", "
                       "argc > 1 ? argv[1] : \"none\");\n    return 0;\n}\n"),
        "LICENSE": "hosttool: MIT, for the test\n",
    },
    "usetool-1.0": {
        "main.c": ("#include <stdio.h>\nextern const char *thing;\n"
                   "int main(void) { puts(thing); return 0; }\n"),
        "LICENSE": "usetool: MIT, for the test\n",
    },
    "blind-1.0": {
        "main.c": "int main(void) { return 0; }\n",
        "LICENSE": "blind: MIT, for the test\n",
    },
    "datapkg-1.0": {
        "fonts/x.ttf": "not really a font\n",
        "fonts/y.ttf": "nor this\n",
        "a.txt": "data a\n",
        "LICENSE": "datapkg: OFL-1.1, for the test\n",
    },
}

HOSTTOOL_BUILD = """build() {
    cc -O2 -o hosttool hosttool.c
}

install() {
    mkdir -p "$destdir$prefix/bin"
    cp hosttool "$destdir$prefix/bin/hosttool"
}
"""

USETOOL_BUILD = """build() {
    command -v hosttool > /dev/null || { echo "hosttool is not on PATH"; exit 1; }
    hosttool thing > gen.c
    "$CC" -O2 -pthread -o usetool main.c gen.c
}

install() {
    mkdir -p "$destdir$prefix/bin"
    cp usetool "$destdir$prefix/bin/"
}
"""

BLIND_BUILD = """build() {
    if command -v hosttool > /dev/null; then echo "hosttool is on PATH"; exit 1; fi
    "$CC" -O2 -o blind main.c
}

install() {
    mkdir -p "$destdir$prefix/bin"
    cp blind "$destdir$prefix/bin/"
}
"""

HEAD = """name {name}
version 1.0
revision 1
summary {name}, for the self-test
licence {licence}
licence-file LICENSE
{source}
"""

RECIPES = {
    "hosttool": ("MIT", ["place host", "style custom", "program bin/hosttool"]),
    "usetool": ("MIT", ["place package", "style custom", "tool hosttool", "program bin/usetool",
                        "subpackage usetool-extra bin/usetool"]),
    "blind": ("MIT", ["place package", "style custom", "program bin/blind"]),
    "datapkg": ("OFL-1.1", ["place package", "style data", "data fonts share/fonts/datapkg",
                            "data a.txt share/datapkg/a.txt",
                            "files share/fonts/datapkg share/datapkg"]),
}
BUILDS = {"hosttool": HOSTTOOL_BUILD, "usetool": USETOOL_BUILD, "blind": BLIND_BUILD}


def write_tree(root, up, real_ports):
    os.makedirs(up, exist_ok=True)
    os.makedirs(root, exist_ok=True)
    for f in ("config.site", "jamos.cmake", "jamos.meson"):
        with open(os.path.join(real_ports, f), "rb") as src, \
                open(os.path.join(root, f), "wb") as dst:
            dst.write(src.read())
    for top, files in SOURCES.items():
        fixtures.make_tar(os.path.join(up, top + ".tar.gz"), top, files)
    for name, (lic, extra) in RECIPES.items():
        d = os.path.join(root, name)
        os.makedirs(d)
        src = fixtures.source_line(os.path.join(up, "%s-1.0.tar.gz" % name))
        with open(os.path.join(d, "recipe"), "w") as f:
            f.write(HEAD.format(name=name, licence=lic, source=src) + "\n".join(extra) + "\n")
        if name in BUILDS:
            with open(os.path.join(d, "build.sh"), "w") as f:
                f.write(BUILDS[name])


def lint_with(root, name, text):
    """The tree's lint problems with port name's recipe replaced by text."""
    path = os.path.join(root, name, "recipe")
    with open(path) as f:
        old = f.read()
    with open(path, "w") as f:
        f.write(text)
    try:
        t, problems = treemod.load(root)
        return problems + treemod.lint(t)
    finally:
        with open(path, "w") as f:
            f.write(old)


def recipe_text(root, name):
    with open(os.path.join(root, name, "recipe")) as f:
        return f.read()


def run(tmp, real_ports, expect, refused, builder):
    """Every check; expect(cond, what) and refused(fn, words, what) are the
    self-test's, builder(tree, tmp) a Builder for a tree."""
    root, up = os.path.join(tmp, "hd-ports"), os.path.join(tmp, "hd-up")
    write_tree(root, up, real_ports)
    t, problems = treemod.load(root)
    problems += treemod.lint(t)
    expect(not problems, "the host and data fixture tree fails its lint: %s" % problems)
    test_licences(expect)
    test_lint(root, expect)
    expect(treemod.order(t, ["usetool"]) == ["hosttool", "usetool"], "the order: a tool first")
    b, _ = builder(t, os.path.join(tmp, "hd"))
    try:
        done = b.build(["usetool", "blind", "datapkg"])
    except buildmod.BuildError as e:
        expect(False, "the host and data fixture build failed: %s" % e)
        return
    expect([n for n, s in done] == ["hosttool", "usetool", "blind", "datapkg"] or
           sorted(n for n, s in done) == ["blind", "datapkg", "hosttool", "usetool"],
           "built: %s" % done)
    out = os.path.join(tmp, "hd", "out")
    test_host(out, expect)
    test_data(out, expect)
    # A change in the tool rebuilds the port that uses it, and not the one that does not.
    with open(os.path.join(root, "hosttool", "recipe"), "a") as f:
        f.write("category test\n")
    t, _ = treemod.load(root)
    b, _ = builder(t, os.path.join(tmp, "hd"))
    done = dict(b.build(["usetool", "blind"]))
    expect(done.get("hosttool") == "built" and done.get("usetool") == "built",
           "the tool's change rebuilt its user: %s" % done)
    expect(done.get("blind") == "up to date", "the tool's change rebuilt a port not using it")
    test_bad(tmp, real_ports, expect, refused, builder)


def test_licences(expect):
    for good in ("HPND-sell-variant", "MIT AND MIT-open-group AND HPND AND ISC",
                 "MIT AND LicenseRef-digital-equipment-corporation"):
        try:
            licence.parse(good)
        except ValueError as e:
            expect(False, "licence %r refused: %s" % (good, e))
    for bad in ("LicenseRef-mine", "LicenseRef-digital-equipment", "MIT-open-group+"):
        try:
            licence.parse(bad)
        except ValueError:
            continue
        expect(False, "licence %r taken" % bad)


def has(problems, *words):
    return any(all(w in p for w in words) for p in problems)


def test_lint(root, expect):
    host, use = recipe_text(root, "hosttool"), recipe_text(root, "usetool")
    data = recipe_text(root, "datapkg")
    cases = [
        ("usetool", use + "needs hosttool\n", ["needs hosttool", "a host port"],
         "a needs of a host port"),
        ("usetool", use + "tool liba\n", ["tool liba", "no such port"], "a tool of no port"),
        ("usetool", use + "tool blind\n", ["tool blind", "not a host port"],
         "a tool that is not a host port"),
        ("hosttool", host + "library lib/libx.a\n", ["no 'library' lines"],
         "a host port with a library line"),
        ("hosttool", host + "needs blind\n", ["no 'needs' lines"], "a host port that needs"),
        ("hosttool", host.replace("style custom", "style data"), ["style is one of"],
         "a host port of style data"),
        ("hosttool", host + "host-of usetool\n", ["host-of usetool", "its source lines differ"],
         "host-of another source"),
        ("hosttool", host.replace("version 1.0", "version 1.1") + "host-of blind\n",
         ["host-of blind", "version 1.0 and this 1.1"], "host-of another version"),
        ("usetool", use + "host-of hosttool\n", ["host-of is for place host"],
         "host-of on a package"),
        ("datapkg", data.replace("files share/fonts/datapkg share/datapkg\n",
                                 "files share/datapkg\n"),
         ["no files, library or subpackage line keeps share/fonts/datapkg"],
         "a data destination nothing keeps"),
        ("datapkg", data + "program bin/x\n", ["a data port has no programs"],
         "a data port with a program"),
        ("datapkg", data.replace("share/datapkg/a.txt", "../a.txt"),
         ["must be a path under the prefix"], "a data destination out of the prefix"),
        ("datapkg", data.replace("data fonts ", "data /etc "), ["must be a path inside"],
         "a data source out of the tree"),
        ("datapkg", data.replace("style data", "style custom"), ["'data' lines are for style data"],
         "data lines on another style"),
        ("datapkg", data.replace("data fonts share/fonts/datapkg\n", "")
         .replace("data a.txt share/datapkg/a.txt\n", ""), ["needs 'data"],
         "a data port with no data"),
    ]
    for name, text, words, what in cases:
        problems = lint_with(root, name, text)
        expect(has(problems, *words), "lint: %s not refused (%s)" % (what, problems))


def test_host(out, expect):
    stage = os.path.join(out, "hosttool", "stage")
    tool = os.path.join(stage, "bin", "hosttool")
    expect(os.path.isfile(tool), "the host tool's stage has bin/hosttool")
    lines = stagemod.read_list(os.path.join(stage, "stage.txt"))
    expect(any(w[0] == "port" and w[4] == "host" and w[5] == os.path.abspath(stage)
               for w in lines), "the stage list names the host place and its prefix: %s" % lines)
    expect(any(w[0] == "program" and w[1] == "bin/hosttool" and w[4] == "-" for w in lines),
           "the host program in the stage list")
    run_out = subprocess.run([tool, "x"], stdout=subprocess.PIPE).stdout.decode()
    expect(run_out == 'const char *x = "made by hosttool";\n',
           "the host tool runs on this machine and says %r" % run_out)
    use = os.path.join(out, "usetool", "stage")
    ulines = stagemod.read_list(os.path.join(use, "stage.txt"))
    expect(["built-with", "hosttool", "1.0", "1"] in ulines,
           "usetool's built-with names the tool: %s" % ulines)
    expect(["sub", "usetool-extra", "bin/usetool"] in ulines,
           "a program in a subpackage: %s" % ulines)
    expect(any(w[0] == "program" and w[1] == "bin/usetool" for w in ulines),
           "...and still a program")
    with open(os.path.join(use, "bin", "usetool"), "rb") as f:
        expect(not stagemod.elf_problems(f.read()), "usetool is a static Jam OS program")
    expect(not os.path.exists(os.path.join(out, "usetool", "sysroot", "bin", "hosttool")),
           "the tool is not in the port's sysroot")


def test_data(out, expect):
    stage = os.path.join(out, "datapkg", "stage")
    lines = stagemod.read_list(os.path.join(stage, "stage.txt"))
    names = sorted(w[1] for w in lines if w[0] == "file")
    expect(names == ["share/datapkg/a.txt", "share/fonts/datapkg/x.ttf",
                     "share/fonts/datapkg/y.ttf"], "the data port's files: %s" % names)
    with open(os.path.join(stage, "share", "fonts", "datapkg", "x.ttf")) as f:
        expect(f.read() == "not really a font\n", "the data port's file bytes")
    expect(not any(w[0] == "program" for w in lines), "a data port has no program")
    expect(os.path.isfile(os.path.join(stage, "share", "licences", "datapkg", "LICENSE")),
           "the data port's licence")


def test_bad(tmp, real_ports, expect, refused, builder):
    import shutil
    root = os.path.join(tmp, "hd-bad")
    up = os.path.join(tmp, "hd-bad-up")
    write_tree(root, up, real_ports)
    # the tool's build makes no program: refused, naming it
    with open(os.path.join(root, "hosttool", "recipe"), "a") as f:
        f.write("program bin/nothere\n")
    t, _ = treemod.load(root)
    b, _ = builder(t, os.path.join(tmp, "hd-bad-build"))
    refused(lambda: b.build(["hosttool"]), ["program bin/nothere", "no executable"],
            "a host program that was not made")
    # a symlink in a data directory
    src = os.path.join(tmp, "hd-link")
    os.makedirs(os.path.join(src, "d"))
    with open(os.path.join(src, "d", "f"), "w") as f:
        f.write("x")
    os.symlink("f", os.path.join(src, "d", "l"))
    r, _ = recipemod.parse(os.path.join(root, "datapkg", "recipe"))
    refused(lambda: hostdata.install_data(r, src, os.path.join(tmp, "hd-link-out")),
            ["data fonts", "not in the source"], "a data source that is not there")
    r.many["data"] = [(1, ["d", "share/d"])]
    refused(lambda: hostdata.install_data(r, src, os.path.join(tmp, "hd-link-out2")),
            ["a symlink"], "a symlink in a data directory")
    shutil.rmtree(src)
