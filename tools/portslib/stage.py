"""After a port's install: keep only what the recipe names, and check it
(docs/M13.7-PLAN.md section 5, steps 6 to 8, and section 10).

The upstream install goes into build/ports/<name>/destdir; the stage,
build/ports/<name>/stage, gets only the files a `program`, `library`,
`files` or `subpackage` line names (paths under the prefix; globs, a
directory meaning everything in it) and the licence texts, in
share/licences/<name>/. Everything else is dropped and listed (man
pages, .la files, an executable nobody gave a list). A symlink the
install made becomes a copy of its target, recorded as one (FAT32 has no
links); one that leaves the install is refused.

Then the checks: each program is a static x86-64 ELF executable whose
list (its ELF note) is the one its recipe chose, and the list is allowed
in its place (tree.list_problems); each file that will be packaged passes
the place's name rules (FAT32's for both places today: a boot-image name
FAT can't hold is no portable name either). Times are the build's
SOURCE_DATE_EPOCH and modes 0755 or 0644, so two builds stage the same
bytes.

stage.txt lists the result for the packager and for the ports that need
this one (the format is the builder's own, `jamos-stage 1`):

    port <name> <version> <revision> <place> <prefix>
    built-with <what> <version> [<revision>]   musl, the sysroot (by
                                  its stamp), each port it needs
    program <path> <size> <sha256> <list file or ->
    library <path> <size> <sha256>
    file <path> <size> <sha256>
    licence <path> <size> <sha256>
    sub <subpackage> <path>       (the path is also a `file` line)
    copy <path> <its target>      (a symlink made a copy)
    multicall <program> <name>    the program is also installed as <name>,
                                  beside it (the recipe's `multicall`)
"""
import glob
import hashlib
import os
import shutil
import struct
import subprocess
import sys

from . import toolchain
from . import tree as treemod

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import checkwants  # noqa: E402

FAT_BAD = set('"*:<>?\\|') | {chr(c) for c in range(0x20)} | {"\x7f"}
FAT_FILE_MAX = 0xFFFFFFFF
PATH_MAX = 255   # the package format's limit, bytes, under the prefix


class StageError(Exception):
    pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def fat_problems(entries):
    """What FAT32 (and the package format) can't hold among entries, a
    list of (path under the prefix, size): one line each, the file named."""
    out, seen = [], {}
    for path, size in entries:
        try:
            raw = path.encode("utf-8")
        except UnicodeEncodeError:
            out.append("%s: the name is not UTF-8" % path)
            continue
        if len(raw) > PATH_MAX:
            out.append("%s: longer than %d bytes" % (path, PATH_MAX))
        parts = path.split("/")
        for part in parts:
            bad = sorted(set(part) & FAT_BAD)
            if bad:
                out.append("%s: FAT32 can't hold %s in a name" %
                           (path, " ".join(repr(c) for c in bad)))
            if part.endswith((".", " ")):
                out.append("%s: FAT32 drops a name's trailing dot or space" % path)
            if len(part.encode("utf-16-le")) // 2 > 255:
                out.append("%s: a name longer than FAT32's 255" % path)
        for i in range(1, len(parts) + 1):
            p = "/".join(parts[:i])
            other = seen.setdefault(p.casefold(), p)
            if other != p:
                out.append("%s: differs from %s only in case (FAT32 is case-blind: "
                           "a case-sensitive filesystem's for later)" % (p, other))
                break
        if size > FAT_FILE_MAX:
            out.append("%s: over FAT32's 4 GiB" % path)
    return sorted(set(out))


# One name rule per place; a later filesystem gets its own.
PLACE_RULES = {"package": fat_problems, "boot": fat_problems}


def elf_problems(data):
    """Why data isn't a static x86-64 ELF executable (empty: it is)."""
    if data[:4] != b"\x7fELF":
        return ["not an ELF file"]
    if data[4] != 2 or data[5] != 1:
        return ["not a 64-bit little-endian ELF file"]
    etype, machine = struct.unpack_from("<HH", data, 16)
    out = []
    if machine != 62:
        out.append("not x86-64 (machine %d)" % machine)
    if etype != 2:
        out.append("not an executable (ELF type %d: a static link makes type 2)" % etype)
    phoff, = struct.unpack_from("<Q", data, 32)
    phentsize, phnum = struct.unpack_from("<HH", data, 54)
    for i in range(phnum):
        ptype, = struct.unpack_from("<I", data, phoff + i * phentsize)
        if ptype in (2, 3):
            out.append("dynamically linked (it has a %s)" % ("PT_DYNAMIC" if ptype == 2
                                                             else "PT_INTERP"))
    return out


def program_problems(path, want_text, place, src):
    """A staged program checked: static, its list the recipe's, the list
    allowed in its place."""
    with open(path, "rb") as f:
        data = f.read()
    out = elf_problems(data)
    if out:
        return out
    try:
        texts = checkwants.notes(data)
    except (ValueError, struct.error) as e:
        return ["its note: %s" % e]
    if len(texts) != 1:
        return ["%d lists in it (want one: was it linked by the port's cc?)" % len(texts)]
    if texts[0] != want_text:
        return ["its list is %r, the recipe chose %r (was it linked under another name?)" %
                (texts[0].decode(errors="replace"), want_text.decode(errors="replace"))]
    return treemod.list_problems(texts[0], place, src)


def _matches(root, pattern):
    """Files under root matching pattern (relative paths; a directory
    match means every file under it)."""
    out = []
    for m in sorted(glob.glob(os.path.join(root, pattern), recursive=True)):
        if os.path.isdir(m) and not os.path.islink(m):
            for d, dirs, files in os.walk(m):
                dirs.sort()
                out += [os.path.relpath(os.path.join(d, f), root) for f in sorted(files)]
        else:
            out.append(os.path.relpath(m, root))
    return out


def _wanted(r, root):
    """[(kind, path under the prefix, extra)] the recipe names; StageError
    for a line that matches nothing."""
    want = []
    for path, wants in r.programs():
        if not os.path.lexists(os.path.join(root, path)):
            raise StageError("program %s: the install made no such file" % path)
        want.append(("program", path, wants))
    # A subpackage's files before `files`: a file both name is the
    # subpackage's (the narrower claim).
    groups = [("library", "library", None, w) for w in r.lines("library")]
    groups += [("subpackage " + w[0], "file", w[0], w[1:]) for w in r.lines("subpackage")]
    groups += [("files", "file", None, w) for w in r.lines("files")]
    for key, kind, sub, globs in groups:
        for g in globs:
            found = _matches(root, g)
            if not found:
                raise StageError("%s %s: the install made nothing that matches" % (key, g))
            want += [(kind, p, sub) for p in found]
    return want


def _source(root, rel):
    """The file to copy for rel: itself, or a symlink's target inside root."""
    path = os.path.join(root, rel)
    if not os.path.islink(path):
        return path, None
    real = os.path.realpath(path)
    real_root = os.path.realpath(root)
    if not real.startswith(real_root + os.sep) or not os.path.isfile(real):
        raise StageError("%s: a symlink to %s, outside the install" % (rel, os.readlink(path)))
    return real, os.path.relpath(real, real_root)


def keep(r, destdir, srcdir, stage, epoch, say=print):
    """Make the stage from the install; returns the stage list's lines."""
    root = os.path.join(destdir, r.prefix.lstrip("/"))
    if os.path.exists(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)
    lines, kept, programs = [], set(), []
    strip = shutil.which(toolchain.cross() + "strip")
    for kind, rel, extra in _wanted(r, root):
        if rel in kept:
            continue
        kept.add(rel)
        src, target = _source(root, rel)
        dst = os.path.join(stage, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if kind == "program":
            subprocess.run([strip, "--strip-debug", "-o", dst, src], check=True)
            programs.append((rel, extra))
        else:
            shutil.copyfile(src, dst)
        os.chmod(dst, 0o755 if kind == "program" else 0o644)
        if target is not None:
            lines.append("copy %s %s" % (rel, target))
        lines.append(_line(kind, rel, dst, extra if kind == "program" else None))
        if kind == "file" and extra:
            lines.append("sub %s %s" % (extra, rel))
    lines += _multicall(r, stage)
    lines += _licences(r, srcdir, stage)
    _times(stage, epoch)
    _dropped(r, destdir, root, kept, os.path.dirname(stage), say)
    return programs, lines


def _multicall(r, stage):
    """`multicall <program> <name>` lines: the names the program is also
    installed under, beside it (a copy; FAT32 has no links). The stage
    holds the program once; the boot image's list and the package make
    the names."""
    out = []
    for w in r.lines("multicall"):
        if not os.path.isfile(os.path.join(stage, w[0])):
            raise StageError("multicall %s: no such program in the stage" % w[0])
        out += ["multicall %s %s" % (w[0], n) for n in w[1:]]
    return out


def _line(kind, rel, path, wants):
    s = "%s %s %d %s" % (kind, rel, os.path.getsize(path), sha256(path))
    return s + (" %s" % (wants or "-") if kind == "program" else "")


def _licences(r, srcdir, stage):
    out = []
    for w in r.lines("licence-file"):
        for f in w:
            src = os.path.join(srcdir, f)
            if not os.path.isfile(src):
                raise StageError("licence-file %s is not in the source" % f)
            rel = "share/licences/%s/%s" % (r.name, os.path.basename(f))
            dst = os.path.join(stage, rel)
            if os.path.exists(dst):
                raise StageError("two licence files named %s" % os.path.basename(f))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
            os.chmod(dst, 0o644)
            out.append(_line("licence", rel, dst, None))
    return out


def _times(stage, epoch):
    for d, _, files in os.walk(stage):
        for f in files:
            os.utime(os.path.join(d, f), (epoch, epoch))


def _dropped(r, destdir, root, kept, base, say):
    """List what the install made and the recipe didn't keep."""
    dropped = []
    for d, dirs, files in os.walk(destdir):
        dirs.sort()
        for f in sorted(files):
            path = os.path.join(d, f)
            rel = os.path.relpath(path, root)
            if rel in kept:
                continue
            what = os.path.relpath(path, destdir)
            if not os.path.islink(path):
                with open(path, "rb") as fh:
                    if fh.read(4) == b"\x7fELF":
                        what += "  (an executable with no program line)"
            dropped.append(what)
    with open(os.path.join(base, "dropped.txt"), "w") as f:
        f.write("".join(d + "\n" for d in dropped))
    if dropped:
        shown = ", ".join(dropped[:4]) + (", ..." if len(dropped) > 4 else "")
        say("%s: dropped %d file%s the recipe doesn't name (%s; all in dropped.txt)" %
            (r.name, len(dropped), "" if len(dropped) == 1 else "s", shown))


def check(r, stage, programs, lines):
    """Every problem with the stage: programs, then the place's names."""
    problems = []
    src = os.path.relpath(r.dir)
    for rel, wants in programs:
        want = b"posix\n"
        if wants is not None:
            with open(r.port_file(wants), "rb") as f:
                want = f.read()
        problems += ["program %s: %s" % (rel, p) for p in
                     program_problems(os.path.join(stage, rel), want, r.place, src)]
    packaged = [(w.split()[1], int(w.split()[2])) for w in lines
                if w.split()[0] in ("program", "file", "licence")]
    sizes = dict(packaged)
    for w in lines:
        if w.split()[0] == "multicall":
            _, prog, name = w.split()
            packaged.append((os.path.join(os.path.dirname(prog), name), sizes[prog]))
    problems += PLACE_RULES[r.place](packaged)
    return problems


def write_list(path, r, lines):
    head = ["jamos-stage 1", "port %s %s %d %s %s" % (r.name, r.version, r.revision, r.place,
                                                       r.prefix)]
    with open(path, "w") as f:
        f.write("\n".join(head + lines) + "\n")


def read_list(path):
    """A stage list's lines as word lists (after the header)."""
    with open(path) as f:
        lines = [l.split() for l in f.read().splitlines()]
    if not lines or lines[0] != ["jamos-stage", "1"]:
        raise StageError("%s: not a stage list" % path)
    return lines[1:]
