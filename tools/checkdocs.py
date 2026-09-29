#!/usr/bin/env python3
"""Check that our Markdown docs still match the tree (run by `make check`).

    python3 tools/checkdocs.py

Every Markdown file of ours (not third_party/, build/, .claude/, and not
docs/history/, whose finished plans keep the paths of their time) is read,
and these must hold:

  links      [text](target): a relative target exists; a #anchor exists
             as a heading in the target (or in this file for a bare #anchor).
             http(s):// and mailto: links are not fetched.
  paths      a `code span` whose first component is a top-level entry of
             the repo (kernel/..., tools/x.py, docs/..., Makefile) exists;
             one with a * must match at least one file (glob).
  headers    `jam/x.h`, `idl/x.h`, `<jam/x.h>` exist in an include
             directory (kernel/include, drivers/include, user/include).
  file names a bare file name with a source extension (`pmm.c`,
             `port.h`, `usbkeys-test.sh`) exists somewhere in the repo.
  make       `make <target> ...` names a target the Makefile defines.

Spans with <placeholders>, $VARS, spaces (other than `make ...`) or
starting with / or build/ are not paths and are skipped; so are fenced
code blocks. IGNORE lists intentional non-paths that look like paths.
Exit status 1 and one line per problem (file:line: what) if anything fails.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP_DIRS = {"third_party", "build", ".claude", ".git", os.path.join("docs", "history")}
INCLUDE_DIRS = ["kernel/include", "drivers/include", "user/include"]
SOURCE_EXT = (".c", ".h", ".S", ".py", ".sh", ".idl", ".def", ".ld", ".cfg", ".conf")

# Spans that look like paths but name something else, each with the reason:
#     "boot/limine/": "the directory on the stick's ESP, not in the repo",
IGNORE = {
}

LINK = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
SPAN = re.compile(r"(?<!`)`([^`\n]+)`(?!`)")


def our_markdown():
    for d, dirs, files in os.walk(ROOT):
        rel = os.path.relpath(d, ROOT)
        dirs[:] = sorted(x for x in dirs
                         if os.path.normpath(os.path.join(rel, x)) not in SKIP_DIRS)
        for f in sorted(files):
            if f.endswith(".md"):
                yield os.path.normpath(os.path.join(rel, f))


def slug(heading, seen):
    """GitHub's anchor for a heading: lower case, punctuation dropped,
    spaces to hyphens, -1, -2 ... for repeats."""
    text = re.sub(r"`|\*\*|__|\[([^\]]*)\]\([^)]*\)", r"\1", heading).strip().lower()
    s = "".join(c for c in text if c.isalnum() or c in " -_").replace(" ", "-")
    n = seen.get(s, 0)
    seen[s] = n + 1
    return s if n == 0 else f"{s}-{n}"


_anchors = {}


def anchors(path):
    if path not in _anchors:
        seen, out, fence = {}, set(), False
        with open(os.path.join(ROOT, path), encoding="utf-8") as f:
            for line in f:
                if line.lstrip().startswith("```"):
                    fence = not fence
                m = None if fence else re.match(r"#{1,6}\s+(.*?)\s*#*\s*$", line)
                if m:
                    out.add(slug(m.group(1), seen))
        _anchors[path] = out
    return _anchors[path]


def repo_files():
    names = set()
    for d, dirs, files in os.walk(ROOT):
        dirs[:] = [x for x in dirs if x not in (".git", "build", ".claude")]
        names.update(files)
    return names


def make_targets():
    targets = set()
    with open(os.path.join(ROOT, "Makefile"), encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^([A-Za-z0-9_.-][A-Za-z0-9_. -]*):(?!=)", line)
            if m:
                targets.update(m.group(1).split())
    return targets


def check_link(md, target):
    if re.match(r"^[a-z]+:", target):
        return None
    path, _, anchor = target.partition("#")
    if path:
        full = os.path.normpath(os.path.join(os.path.dirname(md), path))
        if not os.path.exists(os.path.join(ROOT, full)):
            return f"link to a missing file: {target}"
    else:
        full = md
    if anchor and full.endswith(".md") and anchor not in anchors(full):
        return f"link to a missing anchor: {target}"
    return None


def check_span(span, top, names, targets):
    s = span.strip()
    if s in IGNORE:
        return None
    if s.startswith("make "):
        words = [w for w in s.split()[1:] if "=" not in w and not w.startswith("-")]
        if words and words[0] not in targets:
            return f"no Makefile target: `{s}`"
        return None
    header = re.fullmatch(r"<?((?:jam|idl)/[\w.-]+\.h)>?", s)
    if header:
        if not any(os.path.exists(os.path.join(ROOT, d, header.group(1))) for d in INCLUDE_DIRS):
            return f"no such header: `{s}`"
        return None
    if re.search(r"[<>$\s{}=()\"',;|]", s) or s.startswith(("/", "build/", "-")):
        return None
    s = re.sub(r":\d+(-\d+)?$", "", s)   # file.c:123
    first = s.split("/")[0]
    if first in top and ("/" in s or os.path.isfile(os.path.join(ROOT, s))):
        if "*" in s:
            return None if glob.glob(os.path.join(ROOT, s)) else f"nothing matches `{s}`"
        return None if os.path.exists(os.path.join(ROOT, s)) else f"no such path: `{s}`"
    if re.fullmatch(r"\w[\w.+-]*\.\w+", s) and s.endswith(SOURCE_EXT) and s not in names:
        return f"no file named `{s}` in the repo"
    return None


def main():
    top = set(os.listdir(ROOT)) - {".git", "build", ".claude"}
    names, targets = repo_files(), make_targets()
    problems = []
    for md in our_markdown():
        fence = False
        with open(os.path.join(ROOT, md), encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                if line.lstrip().startswith("```"):
                    fence = not fence
                    continue
                if fence:
                    continue
                for m in LINK.finditer(line):
                    why = check_link(md, m.group(1))
                    if why:
                        problems.append(f"{md}:{n}: {why}")
                for m in SPAN.finditer(line):
                    why = check_span(m.group(1), top, names, targets)
                    if why:
                        problems.append(f"{md}:{n}: {why}")
    for p in problems:
        print(p)
    if problems:
        print(f"checkdocs: {len(problems)} problem(s); fix the doc, or add a real non-path "
              "to IGNORE in tools/checkdocs.py", file=sys.stderr)
        return 1
    print(f"checkdocs: {sum(1 for _ in our_markdown())} Markdown files ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
