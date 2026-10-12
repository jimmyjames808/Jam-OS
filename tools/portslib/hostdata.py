"""Host builds and data ports (G2's GP1, docs/G2-PLAN.md section 9): two
kinds of port the plain build of M13.7 has no place for.

**Host builds**: `place host`. A tool that runs on the build machine
while another port builds, made from the same source as that port
(wayland-scanner, built from libwayland's tarball; later GLib's
glib-compile-resources and the like). It is built natively, with the
host's own compiler and tools and no sysroot, and installed into its own
stage, build/ports/<name>/stage/, which *is* its prefix (the real path:
a pkg-config file or a script it installs works in place). Nothing of it
is packaged, checked as a Jam OS program or put in a sysroot. A port that
`tool`s it gets its bin/ on PATH and its pkg-config files found by the
build machine's pkg-config, and is rebuilt when it changes.

    name        wayland-scanner
    version     1.26.0
    place       host
    host-of     wayland           # the same version and source, checked
    style       meson
    meson       -Dlibraries=false -Dscanner=true
    program     bin/wayland-scanner

and, in wayland's recipe, `tool wayland-scanner`. A host port has no
`needs` (no sysroot: it links what the build machine has, and a recipe
says which in a comment); it may `tool` another host port.

**Data ports**: `style data`. A port that is files and no build: fonts, a
protocol's XML, a theme. The source is unpacked (or the vendored
directory copied), and the recipe's `data <source path> <path in the
prefix>` lines say which file or directory goes where; `files` or
`library` lines keep them, as for any port. Nothing is compiled.

This module holds all of both: the extra lint, the host build, the data
install and the tool environment. The builder (build.py, tree.py,
toolchain.py, recipe.py) calls it at a few points, each a line or two.
"""
import os
import platform
import shutil
import subprocess

from . import source as sourcemod

# The styles a host port may use (natively: no cross file, no sysroot).
HOST_STYLES = ("configure", "configure-plain", "make", "cmake", "meson", "custom")
# What a host port may not have: it is used where it is installed.
HOST_REFUSED = ("needs", "runs", "library", "subpackage", "multicall")


# ---- lint ------------------------------------------------------------------

def _safe_rel(path):
    parts = path.split("/")
    return not path.startswith("/") and all(p not in ("", ".", "..") for p in parts)


def _covered(dest, globs):
    """Whether a files/library/subpackage line keeps dest (named exactly,
    or as a directory above it, or by a glob)."""
    import fnmatch
    for g in globs:
        g = g.rstrip("/")
        if dest == g or dest.startswith(g + "/") or fnmatch.fnmatchcase(dest, g):
            return True
    return False


def lint(r):
    """The checks of a host or data recipe that need only the recipe."""
    p, where = [], r.path
    if r.place == "host":
        if r.style not in HOST_STYLES and r.style != "":
            p.append("%s: a host port's style is one of %s (it builds natively)" %
                     (where, ", ".join(HOST_STYLES)))
        for key in HOST_REFUSED:
            if r.many.get(key):
                p.append("%s: a host port has no '%s' lines (it is used where it is "
                         "installed, and links what the build machine has)" % (where, key))
        if r.many.get("files"):
            p.append("%s: a host port has no 'files' lines: all it installs stays" % where)
    if "host-of" in r.one and r.place != "host":
        p.append("%s: host-of is for place host" % where)
    if r.style == "data":
        if not r.many.get("data"):
            p.append("%s: style data needs 'data <source path> <path in the prefix>' lines" % where)
        if r.many.get("program"):
            p.append("%s: a data port has no programs (nothing is built)" % where)
        keep = [g for key in ("files", "library") for w in r.lines(key) for g in w]
        keep += [g for w in r.lines("subpackage") for g in w[1:]]
        for w in r.lines("data"):
            if not _safe_rel(w[0]):
                p.append("%s: data source '%s' must be a path inside the source" % (where, w[0]))
            if not _safe_rel(w[1]):
                p.append("%s: data destination '%s' must be a path under the prefix" %
                         (where, w[1]))
            elif not _covered(w[1], keep):
                p.append("%s: data %s: no files, library or subpackage line keeps %s "
                         "(anything not named is dropped)" % (where, w[0], w[1]))
        for key in ("configure", "make", "cmake", "meson"):
            if r.many.get(key):
                p.append("%s: '%s' lines are for a style that builds" % (where, key))
    elif r.many.get("data"):
        p.append("%s: 'data' lines are for style data" % where)
    return p


def lint_tree(t, r):
    """The checks across recipes: `tool` names a host port, `host-of` a
    port of the same version and source."""
    p = []
    for w in r.lines("tool"):
        other = t.recipes.get(w[0])
        if other is None:
            p.append("%s: tool %s: no such port" % (r.path, w[0]))
        elif other.place != "host":
            p.append("%s: tool %s: not a host port (place host: it runs on the build machine)" %
                     (r.path, w[0]))
    if r.place == "host" and "host-of" in r.one:
        name = r.get("host-of")
        other = t.recipes.get(name)
        if other is None:
            p.append("%s: host-of %s: no such port" % (r.path, name))
        else:
            if other.version != r.version:
                p.append("%s: host-of %s, but that is version %s and this %s (the host tool "
                         "and the port are built from one release)" %
                         (r.path, name, other.version, r.version))
            mine = [(s.kind, s.url, s.size, s.digest) for s in r.sources]
            theirs = [(s.kind, s.url, s.size, s.digest) for s in other.sources]
            if mine != theirs or other.get("vendored") != r.get("vendored"):
                p.append("%s: host-of %s, but its source lines differ (the same tarball)" %
                         (r.path, name))
    return p


# ---- what a build needs ----------------------------------------------------

def missing_tools(r):
    """The tools a host port's build needs and this machine lacks."""
    want = ["cc"]
    if r.style in ("configure", "configure-plain", "make", "cmake", "custom"):
        want.append("make")
    if r.style == "cmake":
        want.append("cmake")
    if r.style == "meson":
        want += ["meson", "ninja"]
    for w in r.lines("host"):
        want += w
    out = []
    for tool in want:
        if shutil.which(tool) is None and tool not in ("make", "gmake"):
            out.append(tool)
        elif tool in ("make", "gmake") and shutil.which("gmake") is None \
                and shutil.which("make") is None:
            out.append("GNU make")
    return out


def host_id():
    """What the host's own toolchain is, for a host port's stamp: another
    machine or compiler makes the tool again."""
    try:
        cc = subprocess.run(["cc", "--version"], stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL).stdout.decode(errors="replace")
    except OSError:
        cc = ""
    return ("host %s %s\n%s" % (platform.system(), platform.machine(),
                                cc.splitlines()[0] if cc else "")).encode()


def tool_closure(t, name):
    """The host ports name's build runs: its `tool` lines and theirs."""
    out, todo = [], [w[0] for w in t.recipes[name].lines("tool")]
    while todo:
        n = todo.pop(0)
        if n in out or n not in t.recipes:
            continue
        out.append(n)
        todo += [w[0] for w in t.recipes[n].lines("tool")]
    return out


def tool_dirs(b, r):
    """([bin directories], [pkg-config directories]) of the host ports r's
    build runs, in their stages."""
    bins, pcs = [], []
    for n in tool_closure(b.tree, r.name):
        stage = os.path.join(b.base(n), "stage")
        bins.append(os.path.join(stage, "bin"))
        pcs += [os.path.join(stage, d, "pkgconfig") for d in ("lib", "share")]
    return bins, pcs


def write_native_file(path, pcs):
    """A Meson native file for a cross build that runs host tools: the
    build machine's pkg-config looks in the tools' stages (and nowhere
    else of ours: the cross lookups are the cross file's)."""
    with open(path, "w") as f:
        f.write("# The build machine's side of a cross build (tools/portslib/hostdata.py).\n"
                "[built-in options]\npkg_config_path = [%s]\n" %
                ", ".join("'%s'" % p for p in pcs))
    return path


# ---- the host build --------------------------------------------------------

def _env(b, r, base, srcdir, prefix):
    keep = ("HOME", "USER", "LOGNAME", "SHELL", "DEVELOPER_DIR", "SDKROOT")
    env = {k: os.environ[k] for k in keep if k in os.environ}
    bins, pcs = tool_dirs(b, r)
    path = os.environ.get("PATH", "/usr/bin:/bin")
    if platform.system() == "Darwin":
        # A native build wants Apple's ar, ranlib and ld: a GNU binutils first
        # on PATH (Homebrew's, for the cross builds) writes archives the
        # linker can't read. Meson and the tools themselves stay first.
        keep = [d for d in path.split(os.pathsep) if "/binutils" not in d]
        path = os.pathsep.join(["/usr/bin"] + keep)
    env["PATH"] = os.pathsep.join(bins + [path])
    if pcs:
        env["PKG_CONFIG_PATH"] = os.pathsep.join(pcs)
    make = shutil.which("gmake") or shutil.which("make") or "make"
    env.update({
        "LANG": "C", "LC_ALL": "C", "TZ": "UTC", "TMPDIR": os.path.join(base, "tmp"),
        "SOURCE_DATE_EPOCH": str(b.epoch(r)), "CFLAGS": "-O2",
        "name": r.name, "version": r.version, "revision": str(r.revision),
        "prefix": prefix, "destdir": "", "srcdir": srcdir, "portdir": os.path.abspath(r.dir),
        "jobs": str(b.jobs), "make": make,
    })
    return env


def _run_style(b, r, srcdir, work, env, prefix):
    make, jobs = env["make"], "-j%d" % b.jobs
    margs = r.args("make")
    if r.style in ("configure", "configure-plain"):
        b.run(["./configure", "--prefix=" + prefix] + r.args("configure"), srcdir, env)
        b.run([make, jobs] + margs, srcdir, env)
        b.run([make, "install"] + margs, srcdir, env)
    elif r.style == "make":
        b.run([make, jobs, "PREFIX=" + prefix] + margs, srcdir, env)
        b.run([make, "install", "PREFIX=" + prefix] + margs, srcdir, env)
    elif r.style == "cmake":
        bdir = os.path.join(work, ".build")
        b.run(["cmake", "-S", srcdir, "-B", bdir, "-G", "Unix Makefiles",
               "-DCMAKE_MAKE_PROGRAM=" + make, "-DCMAKE_INSTALL_PREFIX=" + prefix,
               "-DCMAKE_BUILD_TYPE=Release"] + r.args("cmake"), srcdir, env)
        b.run(["cmake", "--build", bdir, "--parallel", str(b.jobs)], srcdir, env)
        b.run(["cmake", "--install", bdir], srcdir, env)
    elif r.style == "meson":
        bdir = os.path.join(work, ".build")
        b.run(["meson", "setup", bdir, srcdir, "--prefix=" + prefix, "--libdir=lib",
               "--buildtype=release", "-Dwrap_mode=nodownload"] + r.args("meson"), srcdir, env)
        b.run(["meson", "compile", "-C", bdir, "-j", str(b.jobs)], srcdir, env)
        b.run(["meson", "install", "-C", bdir], srcdir, env)
    else:
        sh = os.path.abspath(r.port_file("build.sh"))
        for fn in ("build", "install"):
            b.run(["/bin/sh", "-ec", '. "$0"; %s' % fn, sh], srcdir, env)


def build_host(b, r, base, distfiles, say=print):
    """Build host port r (its distfiles fetched) into base/stage, which is
    its prefix. Programs the recipe names must be there and executable;
    the stage list says so (the format of stage.py, place host)."""
    from . import stage as stagemod   # not at the top: stage imports tree
    work, stage = os.path.join(base, "work"), os.path.join(base, "stage")
    prefix = os.path.abspath(stage)
    r.prefix_override = prefix
    srcdir = sourcemod.prepare(r, distfiles, work, b._log_say)
    for w in r.lines("licence-file"):
        for f in w:
            if not os.path.isfile(os.path.join(srcdir, f)):
                raise stagemod.StageError("licence-file %s is not in the source" % f)
    os.makedirs(stage)
    env = _env(b, r, base, srcdir, prefix)
    _run_style(b, r, srcdir, work, env, prefix)
    lines = []
    for rel, _ in r.programs():
        path = os.path.join(stage, rel)
        if not os.path.isfile(path) or not os.access(path, os.X_OK):
            raise stagemod.StageError("program %s: the install made no executable file" % rel)
        lines.append("program %s %d %s -" % (rel, os.path.getsize(path), stagemod.sha256(path)))
        say("%s: %s, %d KB (for this machine: not a Jam OS program)" %
            (r.name, rel, os.path.getsize(path) // 1024))
    stagemod.write_list(os.path.join(stage, "stage.txt"), r, lines)


# ---- the data install ------------------------------------------------------

def install_data(r, srcdir, destdir):
    """A data port's `data` lines: each source file or directory copied to
    its path under the prefix, in destdir. A symlink in a directory is
    refused (FAT32 has none: the data is made of plain files)."""
    root = os.path.join(destdir, r.prefix.lstrip("/"))
    for src_rel, dest_rel in r.lines("data"):
        src = os.path.join(srcdir, src_rel)
        dest = os.path.join(root, dest_rel)
        if os.path.islink(src) or not os.path.exists(src):
            raise sourcemod.SourceError("data %s: not in the source (or a symlink)" % src_rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if os.path.isdir(src):
            for d, dirs, files in os.walk(src):
                for n in dirs + files:
                    if os.path.islink(os.path.join(d, n)):
                        raise sourcemod.SourceError("data %s: %s is a symlink" %
                                                    (src_rel, os.path.relpath(os.path.join(d, n), src)))
            shutil.copytree(src, dest)
        else:
            shutil.copyfile(src, dest)
