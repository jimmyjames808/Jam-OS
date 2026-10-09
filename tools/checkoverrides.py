#!/usr/bin/env python3
"""Check that the upstream files an OVERRIDES list replaces are still the
ones our overrides were written against.

    checkoverrides.py <OVERRIDES> <upstream root>   check every line
    checkoverrides.py --selftest <OVERRIDES> <upstream root>
                                                    prove the check refuses
                                                    a changed, a missing and
                                                    a malformed line

An OVERRIDES file (user/posix/musl/OVERRIDES is musl's) has one override
per line: the upstream file (relative to the upstream root), the SHA-256
it had when our file was written, and our file (relative to the top of
the tree). '#' starts a comment line. The check fails, naming each line,
when an upstream file is missing or its hash differs (someone dropped in
a new release: read upstream's change, bring our file up to date, record
the new hash), when our file is missing, or when a line is malformed or
names one upstream file twice. Exit 0 when every line holds, 1 otherwise.
"""
import hashlib
import os
import re
import shutil
import sys
import tempfile

HEX64 = re.compile(r"^[0-9a-f]{64}$")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def read_list(path):
    """The list's lines as (line number, words), comments and blanks out."""
    out = []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if line and not line.startswith("#"):
                out.append((n, line.split()))
    return out


def check(list_path, root, top="."):
    """Every problem with the list, as text (empty: it holds)."""
    problems = []
    seen = set()
    for n, words in read_list(list_path):
        where = "%s:%d" % (list_path, n)
        if len(words) != 3 or not HEX64.match(words[1]):
            problems.append("%s: want '<upstream file> <sha256> <our file>'" % where)
            continue
        up, want, ours = words
        if up in seen:
            problems.append("%s: %s is overridden twice" % (where, up))
        seen.add(up)
        upath = os.path.join(root, up)
        if not os.path.isfile(upath):
            problems.append("%s: upstream %s is gone" % (where, up))
        elif sha256(upath) != want:
            problems.append("%s: upstream %s changed (sha256 %s, the override was "
                            "written against %s): read the change, update %s, record "
                            "the new hash" % (where, up, sha256(upath), want, ours))
        if not os.path.isfile(os.path.join(top, ours)):
            problems.append("%s: our file %s is missing" % (where, ours))
    return problems


def selftest(list_path, root):
    """The check passes on the real tree, and refuses three broken copies."""
    fails = []
    if check(list_path, root):
        fails.append("the real list doesn't pass")
    entries = [w for _, w in read_list(list_path) if len(w) == 3]
    if not entries:
        return ["the list has no entries to test with"]
    up = entries[0][0]
    with tempfile.TemporaryDirectory() as tmp:
        # A copy of the upstream tree's overridden files only: enough for
        # the check, which reads nothing else.
        copy = os.path.join(tmp, "up")
        for _, w in read_list(list_path):
            if len(w) == 3:
                dst = os.path.join(copy, w[0])
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(os.path.join(root, w[0]), dst)
        if check(list_path, copy):
            fails.append("the copied tree doesn't pass")
        with open(os.path.join(copy, up), "ab") as f:
            f.write(b"\n/* a new release */\n")
        if not any("changed" in p for p in check(list_path, copy)):
            fails.append("a changed upstream file was not refused")
        os.remove(os.path.join(copy, up))
        if not any("gone" in p for p in check(list_path, copy)):
            fails.append("a missing upstream file was not refused")
        bad = os.path.join(tmp, "OVERRIDES")
        with open(bad, "w", encoding="utf-8") as f:
            f.write("%s not-a-hash %s\n" % (up, entries[0][2]))
        if not check(bad, root):
            fails.append("a malformed line was not refused")
        with open(bad, "w", encoding="utf-8") as f:
            f.write("%s %s %s\n" % tuple(entries[0]) * 2)
        if not any("twice" in p for p in check(bad, root)):
            fails.append("a file overridden twice was not refused")
    return fails


def main(argv):
    if len(argv) == 4 and argv[1] == "--selftest":
        fails = selftest(argv[2], argv[3])
        for f in fails:
            print("checkoverrides --selftest: " + f)
        print("checkoverrides --selftest: %s" % ("FAIL" if fails else "PASS"))
        return 1 if fails else 0
    if len(argv) != 3:
        print(__doc__.strip().split("\n\n")[1])
        return 2
    problems = check(argv[1], argv[2])
    for p in problems:
        print("checkoverrides: " + p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
