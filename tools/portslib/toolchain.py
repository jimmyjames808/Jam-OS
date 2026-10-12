"""What a port's build runs with: the host's tools found and checked, the
port's own sysroot, the cross tools under the target's names, and the
build's environment (docs/M13.7-PLAN.md section 5, steps 4 and 5).

The sysroot a port sees (build/ports/<name>/sysroot) is build/sysroot
(musl, the call layer, libjam) plus the staged `library` files of exactly
the ports it needs, and theirs (a static library's own libraries must be
linked too), nothing else. jamos-cc is given it as JAMOS_SYSROOT, and
pkg-config is pointed into it, so a library the port didn't declare is
not found even when the Mac has it. A library port's pkg-config files
are rewritten as they go in (their `prefix=` emptied), so `-I` and `-L`
come out under the sysroot.

The tools go in build/ports/<name>/bin, first on PATH, under the names
autoconf, CMake and Meson look for with --host=x86_64-linux-musl: a `gcc`
that is tools/jamos-cc with this sysroot, its debug paths made relative
and, on a link, the program's list chosen by the output's name (a
`program` line's); an `ar` and a `ranlib` that write zero times and ids
(the same bytes every build); a `g++` that refuses (no C++ for Jam OS
yet: a port that wants it must fail, not find the Mac's); the cross
binutils; a pkg-config with the sysroot.
"""
import os
import platform
import re
import shutil
import subprocess

HOST = "x86_64-linux-musl"
BINUTILS = ("as", "ld", "nm", "objcopy", "objdump", "readelf", "strip", "strings", "size",
            "addr2line")


class ToolError(Exception):
    pass


def cross():
    return os.environ.get("CROSS", "x86_64-elf-")


def build_triplet():
    """The machine the build runs on, as config.guess would say it."""
    m = platform.machine()
    m = {"arm64": "aarch64", "amd64": "x86_64"}.get(m, m)
    if platform.system() == "Darwin":
        return "%s-apple-darwin%s" % (m, platform.release().split(".")[0])
    return "%s-pc-linux-gnu" % m


def gnu_make():
    """(command, (major, minor)) of the host's GNU make, gmake first."""
    for cmd in ("gmake", "make"):
        path = shutil.which(cmd)
        if not path:
            continue
        out = subprocess.run([path, "--version"], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL).stdout.decode(errors="replace")
        m = re.match(r"GNU Make (\d+)\.(\d+)", out)
        if m:
            return path, (int(m.group(1)), int(m.group(2)))
    return None, None


def pkgconf():
    return shutil.which("pkgconf") or shutil.which("pkg-config")


def missing_tools(r):
    """The host tools this recipe's build needs and this machine lacks."""
    want = []
    if r.style in ("configure", "configure-plain", "make", "cmake", "custom"):
        want.append("make")
    if r.style == "cmake":
        want.append("cmake")
    if r.style == "meson":
        want += ["meson", "ninja"]
    for w in r.lines("host"):
        want += w
    out = []
    make, ver = gnu_make()
    for tool in want:
        if tool == "make" and not make:
            out.append("GNU make")
        elif tool == "gmake" and (not make or ver < (4, 0)):
            out.append("GNU make 4 (gmake)")
        elif tool in ("pkgconf", "pkg-config") and not pkgconf():
            out.append("pkgconf")
        elif tool not in ("make", "gmake", "pkgconf", "pkg-config") and not shutil.which(tool):
            out.append(tool)
    for tool in ("gcc", "ar", "ranlib", "strip"):
        if not shutil.which(cross() + tool):
            out.append(cross() + tool)
    return out


def _script(path, text):
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, 0o755)


def _q(s):
    """s quoted for sh."""
    return "'" + s.replace("'", "'\\''") + "'"


def cc_wrapper(r, top, sysroot, work):
    """The port's C compiler: jamos-cc with its sysroot and lists."""
    cases = []
    for path, wants in r.programs():
        if wants is not None:
            cases.append("    %s) wants=%s ;;" % (_q(os.path.basename(path)),
                                                  _q("--jamos-wants=" + os.path.abspath(
                                                      r.port_file(wants)))))
    jamos_cc = os.path.join(top, "tools", "jamos-cc")
    return """#!/bin/sh
# The C compiler of port %(name)s (tools/portslib/toolchain.py): jamos-cc
# against the port's own sysroot, debug paths made relative, and on a
# link the program's list chosen by the output's name.
JAMOS_SYSROOT=%(sysroot)s; export JAMOS_SYSROOT
CROSS=%(cross)s; export CROSS
# -pthread (Meson's threads dependency adds it): the threads are in musl's
# libc, and the cross gcc has no such option.
n=$#
while [ $n -gt 0 ]; do
    a=$1; shift; n=$((n - 1))
    case $a in -pthread) ;; *) set -- "$@" "$a" ;; esac
done
out=a.out
prev=
link=1
for a in "$@"; do
    case $a in
    -c|-S|-E|-M|-MM) link=0 ;;
    -o?*) out=${a#-o} ;;
    esac
    [ "$prev" = -o ] && out=$a
    prev=$a
done
wants=
if [ $link = 1 ]; then
    case ${out##*/} in
%(cases)s
    *) ;;
    esac
fi
map=-ffile-prefix-map=%(work)s=/usr/src/%(name)s
if [ -n "$wants" ]; then
    exec %(cc)s "$map" "$wants" "$@"
fi
exec %(cc)s "$map" "$@"
""" % {"name": r.name, "sysroot": _q(sysroot), "cross": _q(cross()), "cases": "\n".join(cases),
       "work": _q(work), "cc": _q(jamos_cc)}


def make_bin(r, top, base, sysroot, work):
    """build/ports/<name>/bin: the tools under the target's names."""
    b = os.path.join(base, "bin")
    os.makedirs(b)
    pre = os.path.join(b, HOST + "-")
    _script(pre + "gcc", cc_wrapper(r, top, sysroot, work))
    os.symlink(HOST + "-gcc", pre + "cc")
    refuse = ("#!/bin/sh\necho \"%s: no C++ compiler for Jam OS yet\" >&2\nexit 1\n" % r.name)
    _script(pre + "g++", refuse)
    os.symlink(HOST + "-g++", pre + "c++")
    ar = shutil.which(cross() + "ar")
    _script(pre + "ar", """#!/bin/sh
# ar writing zero times and ids (D) whenever it adds members, so a
# library's bytes are the same every build.
case $1 in
--*|@*|*[!a-zA-Z-]*) ;;
*[rq]*) k=$1; shift; case $k in *D*) ;; *) k=${k}D ;; esac
    exec %s "$k" "$@" ;;
esac
exec %s "$@"
""" % (_q(ar), _q(ar)))
    _script(pre + "ranlib", "#!/bin/sh\nexec %s -D \"$@\"\n" % _q(shutil.which(cross() + "ranlib")))
    for tool in BINUTILS:
        path = shutil.which(cross() + tool)
        if path:
            os.symlink(path, pre + tool)
    pc = pkgconf()
    libdirs = "%s/lib/pkgconfig:%s/share/pkgconfig" % (sysroot, sysroot)
    if pc:
        body = ("PKG_CONFIG_SYSROOT_DIR=%s PKG_CONFIG_LIBDIR=%s PKG_CONFIG_PATH= exec %s \"$@\"\n"
                % (_q(sysroot), _q(libdirs), _q(pc)))
    else:
        body = "echo 'pkg-config: none on this machine (brew or apt: pkgconf)' >&2\nexit 1\n"
    _script(pre + "pkg-config", "#!/bin/sh\n" + body)
    return b


def _pc_rewrite(data, prefix):
    """A pkg-config file's paths moved under the sysroot: its prefix empty."""
    text = data.decode("utf-8", "replace")
    text = re.sub(r"(?m)^prefix=%s/?$" % re.escape(prefix), "prefix=", text)
    return text.replace(prefix + "/", "/").encode()


def make_sysroot(sysroot, base_sysroot, libs):
    """sysroot: base_sysroot's files plus libs, a list of (port, its stage
    directory, its prefix, [library paths]); ToolError on two that give
    the same file."""
    if not os.path.isfile(os.path.join(base_sysroot, "lib", "libc.a")):
        raise ToolError("no sysroot at %s: run make first" % base_sysroot)
    shutil.copytree(base_sysroot, sysroot, symlinks=True)
    owner = {}
    for port, stage, prefix, paths in libs:
        for rel in paths:
            dst = os.path.join(sysroot, rel)
            if os.path.lexists(dst):
                raise ToolError("%s's %s is already in the sysroot (%s)" %
                                (port, rel, owner.get(rel, "build/sysroot")))
            owner[rel] = port
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(os.path.join(stage, rel), "rb") as f:
                data = f.read()
            if rel.endswith(".pc"):
                data = _pc_rewrite(data, prefix)
            with open(dst, "wb") as f:
                f.write(data)


def environment(r, bindir, sysroot, paths):
    """The build's environment: made from nothing but PATH, HOME and a few
    of the host's, so the Mac's CFLAGS, CPATH, PKG_CONFIG_PATH or a make's
    MAKEFLAGS never reach a port."""
    env = {k: os.environ[k] for k in ("HOME", "USER", "LOGNAME", "SHELL", "DEVELOPER_DIR",
                                      "SDKROOT") if k in os.environ}
    pre = os.path.join(bindir, HOST + "-")
    make, _ = gnu_make()
    env.update({
        "PATH": os.pathsep.join([bindir] + list(paths.get("tool_bins", ())) +
                                [os.environ.get("PATH", "/usr/bin:/bin")]),
        "LANG": "C", "LC_ALL": "C", "TZ": "UTC",
        "TMPDIR": paths["tmp"],
        "SOURCE_DATE_EPOCH": str(paths["epoch"]),
        "CC": pre + "gcc", "CPP": pre + "gcc -E", "CXX": pre + "g++",
        "AR": pre + "ar", "RANLIB": pre + "ranlib", "LD": pre + "ld", "AS": pre + "as",
        "NM": pre + "nm", "STRIP": pre + "strip", "OBJCOPY": pre + "objcopy",
        "OBJDUMP": pre + "objdump", "READELF": pre + "readelf",
        "CFLAGS": "-O2",
        "CC_FOR_BUILD": "cc", "BUILD_CC": "cc", "HOSTCC": "cc",
        "PKG_CONFIG": pre + "pkg-config",
        "CONFIG_SITE": paths["config_site"],
        "JAMOS_SYSROOT": sysroot, "CROSS": cross(),
        # build.sh's variables (and anyone's)
        "name": r.name, "version": r.version, "revision": str(r.revision),
        "prefix": r.prefix, "destdir": paths["destdir"], "srcdir": paths["srcdir"],
        "portdir": os.path.abspath(r.dir), "jobs": str(paths["jobs"]),
        "make": make or "make",
    })
    return env
