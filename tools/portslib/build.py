"""Building ports: the order, the stamps that say what is out of date,
and one port's build from a clean tree to its checked stage
(docs/M13.7-PLAN.md section 5).

Everything a port's build makes is under build/ports/<name>/ (the
`--out` directory's):

    stamp       the inputs' hash at the last good build
    port.sha256 the hash of the port's directory (recipe, patches, files)
                that built it: `ports.py package --from` checks it
    log         every command and its output
    sysroot/    build/sysroot plus the libraries it needs (toolchain.py)
    bin/        the cross tools under the target's names (toolchain.py)
    work/       the source, unpacked afresh (source.py)
    destdir/    upstream's install (DESTDIR)
    stage/      what the recipe keeps, checked (stage.py), and stage.txt
    dropped.txt what the install made and the recipe didn't keep

A `tree kept` port's source, sysroot and tools are in its kept tree
instead (kept.py), which stays between builds.

A port is out of date when its stamp changes: the SHA-256 of the
builder's format, the recipe and every file in the port's directory, the
shared files (config.site and the toolchain files), tools/jamos-cc (and
for `toolchain clang` tools/jamos-clang and the clang's identity, and for
`rust` the Rust's), the sysroot (build/sysroot changes with musl and the
call layer), a vendored source's tree, SOURCE_DATE_EPOCH, and every needed
port's own stamp, so a fix in a library rebuilds what links it. The distfiles' hashes are in the
recipe.
"""
import fcntl
import hashlib
import os
import shutil
import subprocess
import sys
import time

from . import fetch as fetchmod
from . import hostdata
from . import kept as keptmod
from . import source as sourcemod
from . import stage as stagemod
from . import sysrootid
from . import toolchain
from . import tree as treemod

# Bump when a change to the builder changes what it makes, so every port
# is built again.
FORMAT = b"jamos-ports-build 2\n"


class BuildError(Exception):
    pass


def port_hash(r):
    """The SHA-256 of a port's directory (its recipe, patches, lists, files):
    the same on every machine that has the same recipe."""
    h = hashlib.sha256()
    tree_hash(r.dir, h)
    return h.hexdigest()


def tree_hash(path, h):
    """Feed a directory's (or a file's) names and bytes into h, in order."""
    if os.path.isfile(path):
        with open(path, "rb") as f:
            h.update(f.read())
        return
    for d, dirs, files in os.walk(path):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(d, f)
            h.update(os.path.relpath(p, path).encode() + b"\0")
            if os.path.islink(p):
                h.update(b"->" + os.readlink(p).encode())
            else:
                with open(p, "rb") as fh:
                    h.update(hashlib.sha256(fh.read()).digest())


class Builder:
    """One run of the builder over a ports tree."""

    def __init__(self, tree, top, out, jobs=None, force=(), verbose=False, say=print,
                 kept=None, clean=()):
        self.tree = tree                  # tree.Tree
        self.top = top                    # the repository's top directory
        self.out = out                    # build/ports, or --out
        self.jobs = jobs or os.cpu_count() or 4
        self.force = set(force) | set(clean)   # ports to build even when up to date
        self.kept = kept                  # a kept tree's directory (--kept), or None
        self.clean = set(clean)           # kept trees to remove first (--clean)
        self.verbose = verbose
        self.say = say
        self.sysroot = os.environ.get("JAMOS_BASE_SYSROOT",
                                      os.path.join(top, "build", "sysroot"))
        self.stamps = {}                  # name -> stamp, this run
        self._sysroot_stamp = None

    def base(self, name):
        return os.path.join(self.out, name)

    def sysroot_stamp(self):
        if self._sysroot_stamp is None:
            if not os.path.isfile(os.path.join(self.sysroot, "lib", "libc.a")):
                raise BuildError("no sysroot at %s: run make first" % self.sysroot)
            h = hashlib.sha256()
            tree_hash(self.sysroot, h)
            self._sysroot_stamp = h.hexdigest()
        return self._sysroot_stamp

    def stamp(self, r):
        h = hashlib.sha256(FORMAT)
        tree_hash(r.dir, h)
        for f in sorted(treemod.SHARED):
            if os.path.isfile(self.tree.shared(f)):
                h.update(f.encode() + b"\0")
                tree_hash(self.tree.shared(f), h)
        if r.place == "host":   # built natively: the sysroot is not an input
            h.update(hostdata.host_id())
        else:
            tree_hash(os.path.join(self.top, "tools", "jamos-cc"), h)
            h.update(self.sysroot_stamp().encode())
            h.update(toolchain.cross().encode())
            if r.toolchain == "clang":   # the drivers and the clang itself
                h.update(b"toolchain clang\0")
                tree_hash(os.path.join(self.top, "tools", "jamos-clang"), h)
                h.update(toolchain.clang_id(self.top).encode())
            if r.rust:   # and the Rust
                h.update(b"toolchain rust\0")
                h.update(toolchain.rust_id(self.top).encode())
        h.update(b"epoch %d\n" % self.epoch(r))
        if r.get("vendored"):
            tree_hash(os.path.join("third_party", r.get("vendored")), h)
        for dep in treemod.needs_closure(self.tree, r.name):
            h.update(("%s %s\n" % (dep, self.stamps[dep])).encode())
        return h.hexdigest()

    def up_to_date(self, r, stamp):
        path = os.path.join(self.base(r.name), "stamp")
        if r.name in self.force or not os.path.isfile(path):
            return False
        with open(path) as f:
            if f.read().strip() != stamp:
                return False
        # A stage built for another prefix is out of date though no input of
        # the stamp changed (the prefix is the builder's, recipe.py's PREFIX:
        # /data/pkg until PO14 made it /pkg).
        try:
            head = stagemod.read_list(os.path.join(self.base(r.name), "stage", "stage.txt"))[0]
        except (OSError, IndexError, stagemod.StageError):
            return False
        return head[:1] == ["port"] and (r.place != "package" or head[-1] == r.prefix)

    def build(self, names):
        """Build names and what they need, in order. Returns [(name, "built"
        or "up to date")]; BuildError at the first port that fails. One run
        at a time in an output directory (a lock): two would build in the
        same trees."""
        os.makedirs(self.out, exist_ok=True)
        with open(os.path.join(self.out, ".lock"), "w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            return self._build(names)

    def _build(self, names):
        try:
            todo = treemod.order(self.tree, names)
        except treemod.CycleError as e:
            raise BuildError(str(e))
        except KeyError as e:
            raise BuildError("no port %s" % e)
        done = []
        for name in todo:
            r = self.tree.recipes[name]
            self.stamps[name] = self.stamp(r)
            if self.up_to_date(r, self.stamps[name]):
                self.say("%s: up to date" % name)
                done.append((name, "up to date"))
                continue
            self.build_one(r)
            done.append((name, "built"))
        return done

    def build_one(self, r):
        missing = (hostdata.missing_tools(r) if r.place == "host" else
                   toolchain.missing_tools(r, self.top))
        if missing:
            raise BuildError("%s: this machine lacks %s" % (r.name, ", ".join(missing)))
        self.say("%s: building %s-%s r%d (%s, %s)" % (r.name, r.name, r.version, r.revision,
                                                     r.style, r.place))
        try:
            distfiles = [fetchmod.fetch(s, self.say) for s in r.sources]
        except fetchmod.FetchError as e:
            raise BuildError("%s: %s" % (r.name, e))
        base = self.base(r.name)
        stamp_path = os.path.join(base, "stamp")
        if os.path.exists(stamp_path):
            os.remove(stamp_path)
        for d in ("sysroot", "bin", "work", "destdir", "stage", "tmp"):
            shutil.rmtree(os.path.join(base, d), ignore_errors=True)
        os.makedirs(os.path.join(base, "tmp"))
        self.log = os.path.join(base, "log")
        with open(self.log, "w") as f:
            f.write("%s %s-r%d, stamp %s\n" % (r.name, r.version, r.revision,
                                               self.stamps[r.name]))
        start = time.time()
        try:
            self._steps(r, base, distfiles)
        except (sourcemod.SourceError, stagemod.StageError, toolchain.ToolError) as e:
            raise BuildError("%s: %s" % (r.name, e))
        with open(stamp_path, "w") as f:
            f.write(self.stamps[r.name] + "\n")
        with open(os.path.join(base, "port.sha256"), "w") as f:
            f.write(port_hash(r) + "\n")
        self.say("%s: built in %.1f s" % (r.name, time.time() - start))

    def _steps(self, r, base, distfiles):
        if r.place == "host":
            return hostdata.build_host(self, r, base, distfiles, self.say)
        sysroot, work = os.path.join(base, "sysroot"), os.path.join(base, "work")
        toolchain.make_sysroot(sysroot, self.sysroot, self._libs(r))
        kept = None
        if r.kept:   # the sysroot and the tools copied into the kept tree where they changed
            kept = keptmod.kept_dir(base, self.kept)
            if r.name in self.clean:
                keptmod.clean(kept, self._log_say)
            n = keptmod.sync(sysroot, os.path.join(kept, "sysroot"))
            self._log_say("kept: %d sysroot files changed" % n)
            sysroot, work = os.path.join(kept, "sysroot"), os.path.join(kept, "src")
        bindir = toolchain.make_bin(r, self.top, base, sysroot, work)
        if kept:
            keptmod.sync(bindir, os.path.join(kept, "bin"))
            bindir = os.path.join(kept, "bin")
            try:
                srcdir = keptmod.prepare(r, distfiles, kept, self._log_say)
            except keptmod.KeptError as e:
                raise sourcemod.SourceError(str(e))
        else:
            srcdir = sourcemod.prepare(r, distfiles, work, self._log_say)
        tool_bins, tool_pcs = hostdata.tool_dirs(self, r)
        paths = {"tmp": os.path.join(base, "tmp"), "epoch": self.epoch(r), "kept": kept,
                 "tool_bins": tool_bins, "tool_pcs": tool_pcs,
                 "destdir": os.path.join(base, "destdir"), "srcdir": srcdir,
                 "jobs": self.jobs, "rust": toolchain.rust_dir(self.top) if r.rust else None,
                 "clang": toolchain.clang_dir(self.top) if r.toolchain == "clang" else None,
                 "config_site": os.path.abspath(self.tree.shared("config.site"))}
        os.makedirs(paths["destdir"])
        env = toolchain.environment(r, bindir, sysroot, paths)
        self.run_style(r, srcdir, work, env, paths)
        stage = os.path.join(base, "stage")
        programs, lines = stagemod.keep(r, paths["destdir"], srcdir, stage, paths["epoch"],
                                        self.say)
        problems = stagemod.check(r, stage, programs, lines)
        if problems:
            raise stagemod.StageError("the stage fails its checks:\n  " + "\n  ".join(problems))
        stagemod.write_list(os.path.join(stage, "stage.txt"), r, self.built_with(r) + lines)
        for rel, _ in programs:
            self.say("%s: %s, %d KB" % (r.name, rel,
                                       os.path.getsize(os.path.join(stage, rel)) // 1024))

    def built_with(self, r):
        """What is inside r's programs besides its own code: musl, the
        sysroot (the call layer and libjam, by an identity the same on
        every build host: sysrootid.py), every port it needs (for an audit
        of a library's CVE in its users)."""
        out = []
        try:
            with open(os.path.join(self.top, "third_party", "musl", "VERSION")) as f:
                out.append("built-with musl %s" % f.read().strip())
        except OSError:
            pass
        out.append("built-with sysroot %s" %
                   sysrootid.identity(self.sysroot, self.sysroot_stamp())[:16])
        for dep in treemod.needs_closure(self.tree, r.name):
            d = self.tree.recipes[dep]
            out.append("built-with %s %s %d" % (dep, d.version, d.revision))
        return out

    def _libs(self, r):
        """The library files of every port r needs, for its sysroot."""
        out = []
        for dep in treemod.needs_closure(self.tree, r.name):
            d = self.tree.recipes[dep]
            if d.place == "host":   # a tool, not a library: nothing for the sysroot
                continue
            stage = os.path.join(self.base(dep), "stage")
            lines = stagemod.read_list(os.path.join(stage, "stage.txt"))
            out.append((dep, stage, d.prefix, [w[1] for w in lines if w[0] == "library"]))
        return out

    def epoch(self, r):
        """SOURCE_DATE_EPOCH: the recipe's last commit, or (a copy without
        git, as on the test host) the newest file in the port's directory."""
        try:
            out = subprocess.run(["git", "log", "-1", "--format=%ct", "--", r.dir],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                 cwd=self.top).stdout.strip()
            if out:
                return int(out)
        except (OSError, ValueError):
            pass
        newest = 0
        for d, _, files in os.walk(r.dir):
            for f in files:
                newest = max(newest, int(os.path.getmtime(os.path.join(d, f))))
        return newest

    def _log_say(self, text):
        with open(self.log, "a") as f:
            f.write(text + "\n")
        if self.verbose:
            self.say(text)

    def run(self, cmd, cwd, env, extra_env=None):
        """One command of the build, its output in the log (and on the
        terminal with -v); BuildError with the log's end if it fails."""
        if extra_env:
            env = dict(env, **extra_env)
        self._log_say("$ (cd %s && %s)" % (cwd, " ".join(cmd)))
        with open(self.log, "a") as logf:
            p = subprocess.Popen(cmd, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            for line in p.stdout:
                logf.write(line.decode("utf-8", "replace"))
                if self.verbose:
                    sys.stdout.write(line.decode("utf-8", "replace"))
            rc = p.wait()
        if rc != 0:
            with open(self.log, errors="replace") as f:
                tail = f.read().splitlines()[-30:]
            raise sourcemod.SourceError("'%s' failed (exit %d); the log's end (%s):\n    %s" %
                                        (" ".join(cmd[:3]), rc, self.log, "\n    ".join(tail)))

    def run_style(self, r, srcdir, work, env, paths):
        """The build style's commands (section 5, step 5)."""
        make = env["make"]
        jobs = "-j%d" % self.jobs
        destdir = paths["destdir"]
        margs = r.args("make")
        if r.style == "data":
            return hostdata.install_data(r, srcdir, destdir)
        if r.style in ("configure", "configure-plain"):
            if r.style == "configure":
                args = ["--host=" + toolchain.HOST, "--build=" + toolchain.build_triplet(),
                        "--prefix=" + r.prefix, "--disable-shared", "--enable-static"]
                extra = None
            else:
                args, extra = ["--prefix=" + r.prefix], {"CHOST": toolchain.HOST}
            self.run(["./configure"] + args + r.args("configure"), srcdir, env, extra)
            self.run([make, jobs] + margs, srcdir, env, extra)
            self.run([make, "install", "DESTDIR=" + destdir] + margs, srcdir, env, extra)
        elif r.style == "make":
            tools = ["CC=" + env["CC"], "AR=" + env["AR"], "RANLIB=" + env["RANLIB"],
                     "PREFIX=" + r.prefix]
            self.run([make, jobs] + tools + margs, srcdir, env)
            self.run([make, "install", "DESTDIR=" + destdir] + tools + margs, srcdir, env)
        elif r.style == "cmake":
            bdir = os.path.join(work, ".build")
            self.run(["cmake", "-S", srcdir, "-B", bdir, "-G", "Unix Makefiles",
                      "-DCMAKE_MAKE_PROGRAM=" + make,
                      "-DCMAKE_TOOLCHAIN_FILE=" + os.path.abspath(self.tree.shared("jamos.cmake")),
                      "-DCMAKE_INSTALL_PREFIX=" + r.prefix, "-DCMAKE_BUILD_TYPE=Release",
                      "-DBUILD_SHARED_LIBS=OFF"] + r.args("cmake"), srcdir, env)
            self.run(["cmake", "--build", bdir, "--parallel", str(self.jobs)], srcdir, env)
            self.run(["cmake", "--install", bdir], srcdir, env, {"DESTDIR": destdir})
        elif r.style == "meson":
            bdir = os.path.join(work, ".build")
            native = []
            if paths["tool_pcs"]:   # the host tools' pkg-config files, for the build machine
                native = ["--native-file", hostdata.write_native_file(
                    os.path.join(work, "native.ini"), paths["tool_pcs"])]
            self.run(["meson", "setup", bdir, srcdir, "--cross-file",
                      os.path.abspath(self.tree.shared("jamos.meson")), "--prefix=" + r.prefix,
                      "--libdir=lib", "--default-library=static", "--buildtype=release"] +
                     native + r.args("meson"), srcdir, env)
            self.run(["meson", "compile", "-C", bdir, "-j", str(self.jobs)], srcdir, env)
            self.run(["meson", "install", "-C", bdir, "--destdir", destdir], srcdir, env)
        else:
            sh = os.path.abspath(r.port_file("build.sh"))
            for fn in ("build", "install"):
                self.run(["/bin/sh", "-ec", '. "$0"; %s' % fn, sh], srcdir, env)
