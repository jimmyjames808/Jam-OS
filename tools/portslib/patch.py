"""A port's patches (ports/<name>/patches/NNNN-<what>.patch): their
headers, and applying them with no fuzz (docs/M13.7-PLAN.md section 4).

Each patch is a unified diff, applied as `patch -p1 --fuzz=0` would: the
first part of every path is dropped, and every hunk's context and removed
lines must be in the file exactly as written (it may sit at another line
than the hunk says, nearest first, but not one byte may differ). A patch
that no longer applies exactly stops the build: a new upstream version is
read, not guessed at. The builder applies patches itself rather than
running the host's `patch`, so the Mac (BSD patch) and the PC (GNU patch)
give the same result, and so no path in a patch can reach outside the
source tree (no absolute path, no '..', no write through a symlink).

Every patch starts with a header before its first diff line (Debian's
DEP-3, the two fields Jam OS needs):

    Description: what the patch changes and why
    Forwarded: where it was sent upstream, or not-needed <why>

New and deleted files (/dev/null on one side) are taken; renames, mode
changes and binary diffs are refused (an override does those).
"""
import os
import re

NAME = re.compile(r"^[0-9]{4}-[A-Za-z0-9][A-Za-z0-9._+-]*\.patch$")
HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")
REFUSED = ("rename from", "rename to", "copy from", "copy to", "old mode", "new mode",
           "GIT binary patch", "Binary files")


class PatchError(Exception):
    pass


class FilePatch:
    """The hunks of one file: old and new paths (None for /dev/null)."""

    def __init__(self, old, new):
        self.old, self.new = old, new
        self.hunks = []   # [(old start, [(kind, bytes line, has newline)])]

    @property
    def path(self):
        return self.new if self.new is not None else self.old


def header_problems(data):
    """What is wrong with a patch's header (empty: nothing)."""
    head = []
    for line in data.split(b"\n"):
        if line.startswith((b"--- ", b"diff ", b"Index: ")):
            break
        head.append(line.decode("utf-8", "replace"))
    fields = {}
    for line in head:
        m = re.match(r"^([A-Za-z-]+):\s*(.*)$", line)
        if m:
            fields[m.group(1).lower()] = m.group(2).strip()
    out = []
    if not fields.get("description"):
        out.append("no 'Description:' line saying what it changes and why")
    fwd = fields.get("forwarded")
    if not fwd:
        out.append("no 'Forwarded:' line (where it went upstream, or not-needed <why>)")
    elif fwd.split()[0].lower() == "not-needed" and len(fwd.split()) < 2:
        out.append("'Forwarded: not-needed' without the reason")
    return out


def _strip(path, where):
    """A header path with its first part dropped (-p1)."""
    path = path.split("\t")[0].strip()
    if path == "/dev/null":
        return None
    if path.startswith('"'):
        raise PatchError("%s: quoted file names are not taken" % where)
    parts = path.split("/")
    if len(parts) < 2:
        raise PatchError("%s: '%s' has no directory to drop (-p1)" % (where, path))
    rest = parts[1:]
    if any(p in ("", ".", "..") for p in rest):
        raise PatchError("%s: '%s' is not a plain path inside the tree" % (where, path))
    return "/".join(rest)


def parse(data, where):
    """The patch's files, each with its hunks; PatchError on anything
    malformed or refused."""
    lines = data.split(b"\n")
    files, i = [], 0
    while i < len(lines):
        line = lines[i]
        text = line.decode("utf-8", "replace")
        if text.startswith(REFUSED):
            raise PatchError("%s: '%s': renames, modes and binary diffs need an override" %
                             (where, text.strip()))
        if text.startswith("--- ") and i + 1 < len(lines) and lines[i + 1].startswith(b"+++ "):
            fp = FilePatch(_strip(text[4:], where),
                           _strip(lines[i + 1].decode("utf-8", "replace")[4:], where))
            if fp.old is None and fp.new is None:
                raise PatchError("%s: /dev/null on both sides" % where)
            files.append(fp)
            i += 2
            continue
        if text.startswith("@@"):
            if not files:
                raise PatchError("%s:%d: a hunk before any file" % (where, i + 1))
            i = _hunk(lines, i, files[-1], where)
            continue
        i += 1
    if not files:
        raise PatchError("%s: no file in the patch" % where)
    return files


def _hunk(lines, i, fp, where):
    """Read the hunk at lines[i]; the index after it."""
    m = HUNK.match(lines[i].decode("utf-8", "replace"))
    if not m:
        raise PatchError("%s:%d: a malformed hunk header" % (where, i + 1))
    old_n = int(m.group(2)) if m.group(2) is not None else 1
    new_n = int(m.group(4)) if m.group(4) is not None else 1
    body, i = [], i + 1
    while old_n > 0 or new_n > 0:
        if i >= len(lines):
            raise PatchError("%s: hunk cut short at the end" % where)
        line = lines[i]
        kind = line[:1]
        if line == b"" and i == len(lines) - 1:
            raise PatchError("%s: hunk cut short at the end" % where)
        if kind == b" " or line == b"":
            old_n, new_n = old_n - 1, new_n - 1
            kind = b" "
        elif kind == b"-":
            old_n -= 1
        elif kind == b"+":
            new_n -= 1
        else:
            raise PatchError("%s:%d: a hunk line that isn't ' ', '-' or '+'" % (where, i + 1))
        if old_n < 0 or new_n < 0:
            raise PatchError("%s:%d: the hunk has more lines than its header says" %
                             (where, i + 1))
        body.append([kind.decode(), line[1:], True])
        i += 1
        if i < len(lines) and lines[i].startswith(b"\\ "):
            body[-1][2] = False   # "\ No newline at end of file"
            i += 1
    fp.hunks.append((int(m.group(1)), body))
    return i


def _split_file(data):
    """A file as [line without its newline], and whether it ends in one."""
    if data == b"":
        return [], True
    lines = data.split(b"\n")
    if lines[-1] == b"":
        return lines[:-1], True
    return lines, False


def _find(lines, old, want, lo):
    """Where old (a list of lines) sits in lines, nearest to want, at lo
    or later; None if nowhere exactly."""
    n = len(old)
    for delta in range(0, len(lines) + 1):
        for at in (want + delta, want - delta) if delta else (want,):
            if lo <= at <= len(lines) - n and lines[at:at + n] == old:
                return at
    return None


def apply_file(fp, data, where):
    """The file's new bytes (None: delete it)."""
    lines, final_nl = _split_file(data if data is not None else b"")
    out, pos, offset = [], 0, 0
    for k, (start, body) in enumerate(fp.hunks, 1):
        old = [b for kind, b, _ in body if kind in " -"]
        new = [b for kind, b, _ in body if kind in " +"]
        want = max(start - 1, 0) + offset if old else start + offset
        at = _find(lines, old, want, pos)
        if at is None:
            raise PatchError("%s: hunk %d of %s doesn't apply exactly (fuzz 0): read "
                             "upstream's change and write the patch again" % (where, k, fp.path))
        old_end = [nl for kind, _, nl in body if kind in " -"]
        if old_end and not old_end[-1] and (at + len(old) != len(lines) or final_nl):
            raise PatchError("%s: hunk %d of %s: the file's last line differs" %
                             (where, k, fp.path))
        out += lines[pos:at] + new
        pos = at + len(old)
        offset = at - (start - 1) if old else offset
        new_nl = [nl for kind, _, nl in body if kind in " +"]
        if pos == len(lines) and new_nl:
            final_nl = new_nl[-1]
    out += lines[pos:]
    if fp.new is None:
        if out:
            raise PatchError("%s: %s is deleted but the hunks leave lines" % (where, fp.path))
        return None
    return b"\n".join(out) + (b"\n" if out and final_nl else b"")


def _inside(root, rel):
    """root/rel, refused if any part of it on disk is a symlink."""
    path = root
    for part in rel.split("/"):
        path = os.path.join(path, part)
        if os.path.islink(path):
            raise PatchError("%s goes through a symlink" % rel)
    return path


def apply(patch_path, root):
    """Apply one patch to the tree at root, all or nothing: every file's
    result is made before anything is written. Returns the paths changed."""
    where = os.path.basename(patch_path)
    with open(patch_path, "rb") as f:
        data = f.read()
    results, seen = [], set()
    for fp in parse(data, where):
        if fp.path in seen:
            raise PatchError("%s: %s is patched twice in one patch" % (where, fp.path))
        seen.add(fp.path)
        target = _inside(root, fp.path)
        exists = os.path.isfile(target)
        if fp.old is None and exists:
            raise PatchError("%s: adds %s, which is already there" % (where, fp.path))
        if fp.old is not None and not exists:
            raise PatchError("%s: %s is not in the source" % (where, fp.path))
        before = None
        if exists:
            with open(target, "rb") as f:
                before = f.read()
        results.append((fp, target, apply_file(fp, before, where)))
    for fp, target, after in results:
        if after is None:
            os.remove(target)
            continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        mode = os.stat(target).st_mode & 0o777 if os.path.exists(target) else 0o644
        with open(target, "wb") as f:
            f.write(after)
        os.chmod(target, mode)
    return [fp.path for fp, _, _ in results]


def targets(patch_path):
    """The source paths a patch touches (for the lint)."""
    with open(patch_path, "rb") as f:
        return [fp.path for fp in parse(f.read(), os.path.basename(patch_path))]
