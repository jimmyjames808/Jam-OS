"""A port's recipe, ports/<name>/recipe: its parser and the checks one
recipe can make on its own (docs/M13.7-PLAN.md section 2; the checks
across recipes are tree.py's).

The format is lines of `<key> <value>`. A `#` at the start of a line or
after a space starts a comment. Values are words split on spaces and
tabs, with no quoting (an argument can't hold a space). Three variables
are replaced in source URLs and file names and in a style's arguments:
`$name`, `$version` and `$prefix` (the last in arguments only); `$$` is
a dollar. Build commands are never in a recipe: they are the build
style's, or build.sh's functions for `style custom`.

The format is meant to be read by a small C program later (the builder
on Jam OS after M15), so it stays this plain: no includes, no
conditionals, no continuation lines.
"""
import os
import re

from . import hostdata
from . import licence

# Where a port's files go once installed; a port is built for it, since
# programs find their data by the prefix compiled in.
# A package's is /pkg: init's read-only view of where bin/pkg installs
# (/jam/pkg on the Jam OS drive, else /data/pkg), so a program that bakes it
# in finds its files on either (M13.7's PO14; a program asks for the view with
# `mount /pkg r`).
# (A host port, hostdata.py, is installed where it is built: /host is only
# what it is called until then.)
PREFIX = {"package": "/pkg", "boot": "/boot/usr", "host": "/host"}
# A package kept on its old prefix, /data/pkg: Chromium is built once, for
# hours, in a kept tree, and finds its files beside its executable, so its
# prefix is only the name the builder stages it under; moving it would make
# its stage out of date (build.py's up_to_date) and the next build a new one.
KEEP_PREFIX = {"chromium": "/data/pkg"}
STYLES = ("configure", "configure-plain", "make", "cmake", "meson", "custom", "data")
# Which styles take which extra-argument key.
ARG_STYLES = {
    "configure": ("configure", "configure-plain"),
    "make": ("make", "configure", "configure-plain"),
    "cmake": ("cmake",),
    "meson": ("meson",),
}
# key: (how many lines: "one" or "many", fewest words, most words or None)
KEYS = {
    "name": ("one", 1, 1), "version": ("one", 1, 1), "revision": ("one", 1, 1),
    "summary": ("one", 1, None), "homepage": ("one", 1, 1), "category": ("one", 1, 1),
    "licence": ("one", 1, None), "licence-file": ("many", 1, None),
    "lgpl-relink": ("one", 1, None),
    "source": ("many", 3, 4), "vendored": ("one", 1, 1), "local": ("one", 1, 1),
    "place": ("one", 1, 1), "style": ("one", 1, 1),
    "configure": ("many", 1, None), "make": ("many", 1, None),
    "cmake": ("many", 1, None), "meson": ("many", 1, None),
    "needs": ("many", 1, 3), "runs": ("many", 1, 3), "host": ("many", 1, None),
    "program": ("many", 1, 2), "library": ("many", 1, None), "files": ("many", 1, None),
    "subpackage": ("many", 2, None), "multicall": ("many", 2, None),
    "tool": ("many", 1, 1), "host-of": ("one", 1, 1), "data": ("many", 2, 2),
    "cpe": ("one", 1, 1), "fixed": ("many", 2, 2), "not-affected": ("many", 2, None),
    "toolchain": ("one", 1, 2), "tree": ("one", 1, 1), "image-base": ("one", 1, 1),
}
# `image-base <address>`: the port's programs are linked there (the
# drivers' --jamos-image-base, posix.ld's __jamos_image_base) instead of at
# 0x400000: hex, a page's address, from 4 GiB to below 64 TiB (the user
# half's lower half: what is left above it for mappings). Chromium's
# programs sit at 1 TiB, so the low 4 GiB is empty for PartitionAlloc's
# zero segment (docs/CR-PLAN.md, MH).
IMAGE_BASE_LO, IMAGE_BASE_HI = 1 << 32, 1 << 46
# `tree kept`: the port's unpacked source, its build's output and its tools
# stay between builds (kept.py; docs/CR-PLAN.md section 14), for a port too
# big to build afresh each time (Chromium). Style custom, `source` lines.
# `toolchain clang`: the port is compiled by tools/jamos-clang and
# jamos-clang++ (clang and lld, LLVM's runtimes, C++) instead of jamos-cc
# (docs/CR-PLAN.md section 1); without the line, jamos-cc and no C++.
# `toolchain clang rust` adds Chromium's pinned Rust (tools/cr/toolchain.sh
# fetches it; CT2): the build gets $JAMOS_RUST, the directory with bin/cargo
# and bin/rustc, and links by the clang drivers.
TOOLCHAINS = ("clang", "rust")
# The port that builds the runtimes every clang-built program links; every
# other clang port needs it.
RUNTIMES = "llvm-runtimes"
REQUIRED = ("name", "version", "revision", "summary", "licence", "licence-file",
            "place", "style")

NAME = re.compile(r"^[a-z0-9][a-z0-9+._-]{0,62}$")
VERSION = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._+~-]{0,62}$")
FILE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
HEX40 = re.compile(r"^[0-9a-f]{40}$")
CVE = re.compile(r"^CVE-[0-9]{4}-[0-9]{4,}$")
TOOL = re.compile(r"^[a-z0-9][a-z0-9.+_-]*$")
URL = re.compile(r"^(https?|ftp|file)://[^\s]+$")
VAR = re.compile(r"\$(\$|[A-Za-z_]+)")
VARS_URL = ("name", "version")
VARS_ARGS = ("name", "version", "prefix")


class Source:
    """One `source` line: a tarball by URL, size and SHA-256, or a git
    commit (kind "git": no size, and the hash is the commit's)."""

    def __init__(self, kind, url, size, digest, file):
        self.kind = kind        # "url" or "git"
        self.url = url          # with $name and $version replaced
        self.size = size        # bytes (url); None (git)
        self.digest = digest    # SHA-256 hex (url); the commit (git)
        self.file = file        # its name in the cache and the mirror


class Recipe:
    """A parsed recipe. Lists keep the recipe's order."""

    def __init__(self, path):
        self.path = path                  # .../ports/<name>/recipe
        self.dir = os.path.dirname(path)  # the port's directory
        self.text = b""                   # the file's bytes (for the stamp)
        self.one = {}                     # single-line keys: key -> words
        self.many = {}                    # repeated keys: key -> [(line number, words)]
        self.sources = []                 # [Source]
        self.prefix_override = None       # a host port's real prefix (hostdata.py)

    def get(self, key, default=None):
        """A single-line key's value as one string (words joined)."""
        words = self.one.get(key)
        return " ".join(words) if words else default

    def lines(self, key):
        return [w for _, w in self.many.get(key, [])]

    @property
    def name(self):
        return self.get("name", "")

    @property
    def version(self):
        return self.get("version", "")

    @property
    def revision(self):
        try:
            return int(self.get("revision", "0"))
        except ValueError:
            return 0

    @property
    def place(self):
        return self.get("place", "")

    @property
    def style(self):
        return self.get("style", "")

    @property
    def toolchain(self):
        """"clang", or None: jamos-cc."""
        words = self.one.get("toolchain")
        return words[0] if words else None

    @property
    def kept(self):
        """True for `tree kept` (kept.py)."""
        return self.get("tree") == "kept"

    @property
    def image_base(self):
        """The `image-base` line's address (text, as the drivers take it), or
        None: 0x400000."""
        return self.get("image-base") or None

    @property
    def rust(self):
        """True for `toolchain clang rust`: built with Chromium's Rust."""
        return "rust" in (self.one.get("toolchain") or [])

    @property
    def prefix(self):
        if self.place == "package" and self.name in KEEP_PREFIX:
            return KEEP_PREFIX[self.name]
        return self.prefix_override or PREFIX.get(self.place, "/")

    def deps(self, key):
        """`needs` or `runs` as [(port, minimum version or None)]."""
        return [(w[0], w[2] if len(w) == 3 else None) for w in self.lines(key)]

    def args(self, key):
        """A style's extra arguments, every line's words in order, with the
        variables replaced."""
        out = []
        for words in self.lines(key):
            out.extend(self.expand(w, VARS_ARGS) for w in words)
        return out

    def programs(self):
        """[(path, wants file relative to the port, or None for `posix`)];
        a program without a second word takes wants/<its name> if the port
        has one."""
        out = []
        for w in self.lines("program"):
            wants = w[1] if len(w) == 2 else None
            if wants is None and os.path.isfile(self.port_file("wants", os.path.basename(w[0]))):
                wants = "wants/" + os.path.basename(w[0])
            out.append((w[0], wants))
        return out

    def port_file(self, *parts):
        return os.path.join(self.dir, *parts)

    def expand(self, word, allowed):
        """word with $name, $version, $prefix replaced; a variable not in
        allowed is left as it is (the lint names it)."""
        values = {"name": self.name, "version": self.version, "prefix": self.prefix, "$": "$"}

        def one(m):
            v = m.group(1)
            return values[v] if (v == "$" or v in allowed) else m.group(0)
        return VAR.sub(one, word)


def _split(line):
    """A line's words with the comment cut off."""
    out = []
    for w in line.split():
        if w.startswith("#"):
            break
        out.append(w)
    return out


def parse(path):
    """(Recipe, problems): every problem as "path:line: text"."""
    r = Recipe(path)
    problems = []
    try:
        with open(path, "rb") as f:
            r.text = f.read()
        text = r.text.decode("utf-8")
    except (OSError, UnicodeDecodeError) as e:
        return r, ["%s: %s" % (path, e)]
    for n, line in enumerate(text.split("\n"), 1):
        words = _split(line)
        if not words:
            continue
        key, words = words[0], words[1:]
        where = "%s:%d" % (path, n)
        if key not in KEYS:
            problems.append("%s: '%s' is not a recipe key" % (where, key))
            continue
        count, lo, hi = KEYS[key]
        if len(words) < lo or (hi is not None and len(words) > hi):
            problems.append("%s: '%s' takes %s word%s" %
                            (where, key, lo if hi == lo else "%d to %s" % (lo, hi or "any"),
                             "" if hi == lo == 1 else "s"))
            continue
        if count == "one":
            if key in r.one:
                problems.append("%s: '%s' given twice" % (where, key))
            r.one[key] = words
        else:
            r.many.setdefault(key, []).append((n, words))
    for key in REQUIRED:
        if key not in r.one and key not in r.many:
            problems.append("%s: no '%s' line" % (path, key))
    problems += _sources(r)
    return r, problems


def _sources(r):
    problems = []
    for n, w in r.many.get("source", []):
        where = "%s:%d" % (r.path, n)
        if w[0] == "git":
            if len(w) not in (3, 4) or not HEX40.match(w[2]):
                problems.append("%s: want 'source git <url> <40-digit commit> [file]'" % where)
                continue
            url, size, digest = r.expand(w[1], VARS_URL), None, w[2]
            file = w[3] if len(w) == 4 else "%s-git-%s.tar" % (r.name, w[2][:12])
            kind = "git"
        else:
            if len(w) not in (3, 4) or not w[1].isdigit() or not HEX64.match(w[2]):
                problems.append("%s: want 'source <url> <size> <sha256> [file]'" % where)
                continue
            url, size, digest = r.expand(w[0], VARS_URL), int(w[1]), w[2]
            file = r.expand(w[3], VARS_URL) if len(w) == 4 else url.rstrip("/").split("/")[-1]
            kind = "url"
        r.sources.append(Source(kind, url, size, digest, file))
    return problems


def _safe_rel(path):
    """A relative path with no '..', no empty part, no absolute start."""
    parts = path.split("/")
    return not path.startswith("/") and all(p not in ("", ".", "..") for p in parts)


def lint(r):
    """The checks one recipe can make by itself: values well-formed, the
    files it names in its own directory there."""
    p, where = [], r.path
    name = r.name
    if not NAME.match(name):
        p.append("%s: the name '%s' is not [a-z0-9][a-z0-9+._-]*" % (where, name))
    elif name != os.path.basename(r.dir):
        p.append("%s: the name '%s' is not its directory's" % (where, name))
    if r.version and not VERSION.match(r.version):
        p.append("%s: the version '%s' has a character it can't" % (where, r.version))
    if "revision" in r.one and (not r.get("revision").isdigit() or r.revision < 1):
        p.append("%s: the revision must be a number from 1" % where)
    if r.place and r.place not in PREFIX:
        p.append("%s: place must be 'boot', 'package' or 'host'" % where)
    if r.style and r.style not in STYLES:
        p.append("%s: style must be one of %s" % (where, ", ".join(STYLES)))
    if "homepage" in r.one and not URL.match(r.get("homepage")):
        p.append("%s: the homepage is not a URL" % where)
    p += _lint_licence(r)
    p += _lint_source(r)
    p += _lint_vars(r)
    p += _lint_style(r)
    p += _lint_lists(r)
    p += _lint_security(r)
    p += hostdata.lint(r)
    return p


def _lint_licence(r):
    p = []
    if "licence" in r.one:
        try:
            licence.parse(r.get("licence"))
        except ValueError as e:
            p.append("%s: licence: %s" % (r.path, e))
    for w in r.lines("licence-file"):
        for f in w:
            if not _safe_rel(f):
                p.append("%s: licence-file '%s' must be a path inside the source" % (r.path, f))
    if "lgpl-relink" in r.one and not ("licence" in r.one):
        p.append("%s: lgpl-relink without a licence" % r.path)
    return p


def _lint_source(r):
    """Where the source is: `source` lines (fetched), `vendored`
    (third_party/<dir>) or `local` (<dir> in the port's own directory:
    Jam OS's own code, a test program such as ports/cr-hello's)."""
    p = []
    has_source = "source" in r.many
    given = int(has_source) + int("vendored" in r.one) + int("local" in r.one)
    if given != 1:
        p.append("%s: a recipe has 'source' lines, one 'vendored' line or one 'local' line: "
                 "%s" % (r.path, "more than one" if given else "none"))
    if "local" in r.one:
        d = r.get("local")
        if not FILE_NAME.match(d):
            p.append("%s: local names a directory in the port's directory" % r.path)
        elif not os.path.isdir(r.port_file(d)):
            p.append("%s: local: %s is not there" % (r.path, r.port_file(d)))
    if "vendored" in r.one:
        d = r.get("vendored")
        if not FILE_NAME.match(d):
            p.append("%s: vendored names a directory of third_party/" % r.path)
        elif not os.path.isdir(os.path.join("third_party", d)):
            p.append("%s: vendored: third_party/%s is not there" % (r.path, d))
    files = set()
    for s in r.sources:
        if s.kind == "url" and not URL.match(s.url):
            p.append("%s: source '%s' is not an http, https, ftp or file URL" % (r.path, s.url))
        if s.kind == "url" and s.size <= 0:
            p.append("%s: source %s has no size" % (r.path, s.file))
        if not FILE_NAME.match(s.file):
            p.append("%s: source file name '%s' must be a plain name" % (r.path, s.file))
        elif s.kind == "git" and not s.file.endswith(".tar"):
            p.append("%s: a git source's file is a .tar (what git archive makes)" % r.path)
        if s.file in files:
            p.append("%s: two sources are both %s" % (r.path, s.file))
        files.add(s.file)
    return p


def _lint_vars(r):
    p = []
    checks = [(w, VARS_URL, "source") for ws in r.lines("source") for w in ws]
    for key in ARG_STYLES:
        checks += [(w, VARS_ARGS, key) for ws in r.lines(key) for w in ws]
    for word, allowed, key in checks:
        for m in VAR.finditer(word):
            v = m.group(1)
            if v != "$" and v not in allowed:
                p.append("%s: %s: '$%s' is not a variable here (%s; $$ for a dollar)" %
                         (r.path, key, v, ", ".join("$" + a for a in allowed)))
    return p


def _lint_style(r):
    p = []
    for key, styles in ARG_STYLES.items():
        if key in r.many and r.style not in styles:
            p.append("%s: '%s' lines are for style %s" % (r.path, key, " or ".join(styles)))
    has_sh = os.path.isfile(r.port_file("build.sh"))
    if r.style == "custom" and not has_sh:
        p.append("%s: style custom needs build.sh (its build and install functions)" % r.path)
    elif has_sh and r.style != "custom":
        p.append("%s: build.sh is only for style custom" % r.path)
    elif has_sh:
        with open(r.port_file("build.sh"), encoding="utf-8", errors="replace") as f:
            sh = f.read()
        for fn in ("build", "install"):
            if not re.search(r"^%s\s*\(\)" % fn, sh, re.M):
                p.append("%s: build.sh has no %s() function" % (r.path, fn))
    for w in r.lines("tool") + ([r.one["host-of"]] if "host-of" in r.one else []):
        if not NAME.match(w[0]):
            p.append("%s: want a port name, not '%s'" % (r.path, w[0]))
    for w in r.lines("needs") + r.lines("runs"):
        if not NAME.match(w[0]) or (len(w) > 1 and (len(w) != 3 or w[1] != ">=")):
            p.append("%s: want 'needs|runs <port> [>= <version>]', not '%s'" %
                     (r.path, " ".join(w)))
    for w in r.lines("host"):
        for t in w:
            if not TOOL.match(t):
                p.append("%s: host tool '%s'" % (r.path, t))
    if "tree" in r.one:
        if r.get("tree") != "kept":
            p.append("%s: tree takes 'kept' (the source and the build's output stay between"
                     " builds)" % r.path)
        elif r.style != "custom" or "source" not in r.many:
            p.append("%s: a kept tree is for a style custom port with source lines" % r.path)
    tc = r.toolchain
    if tc is not None:
        words = r.one["toolchain"]
        if any(w not in TOOLCHAINS for w in words):
            p.append("%s: toolchain must be one of %s (none: jamos-cc)" %
                     (r.path, ", ".join(TOOLCHAINS)))
        elif words[0] != "clang" or len(words) > 2:
            p.append("%s: toolchain takes 'clang', or 'clang rust' (Rust links by the clang"
                     " drivers)" % r.path)
        elif r.place == "host":
            p.append("%s: a host port is built by the build machine's compiler, not a toolchain"
                     % r.path)
        elif r.name != RUNTIMES and RUNTIMES not in [d for d, _ in r.deps("needs")]:
            p.append("%s: a clang port links LLVM's runtimes: 'needs %s'" % (r.path, RUNTIMES))
    base = r.image_base
    if base is not None:
        ok = re.fullmatch(r"0x[0-9a-f]+000", base) is not None
        if not ok or not IMAGE_BASE_LO <= int(base, 16) < IMAGE_BASE_HI:
            p.append("%s: image-base takes a page's address in hex, from 0x100000000 to below"
                     " 0x400000000000 (lower case)" % r.path)
        elif r.place == "host":
            p.append("%s: a host port's programs are the build machine's: no image-base"
                     % r.path)
    return p


def _lint_lists(r):
    """program, library, files, subpackage and multicall lines."""
    p = []
    names = {}
    for path, wants in r.programs():
        if not _safe_rel(path):
            p.append("%s: program '%s' must be a path under the prefix" % (r.path, path))
        base = os.path.basename(path)
        if base in names:
            p.append("%s: two programs named %s (the list is chosen by the name)" %
                     (r.path, base))
        names[base] = path
        if wants is not None and (not _safe_rel(wants) or
                                  not os.path.isfile(r.port_file(wants))):
            p.append("%s: program %s: its list %s is not in the port's directory" %
                     (r.path, path, wants))
    for key in ("library", "files"):
        for w in r.lines(key):
            for g in w:
                if not _safe_rel(g):
                    p.append("%s: %s '%s' must be a path under the prefix" % (r.path, key, g))
    for w in r.lines("subpackage"):
        if not NAME.match(w[0]) or not w[0].startswith(r.name + "-"):
            p.append("%s: subpackage '%s' must be named %s-<what>" % (r.path, w[0], r.name))
        for g in w[1:]:
            if not _safe_rel(g):
                p.append("%s: subpackage '%s' must be a path under the prefix" % (r.path, g))
    progs = {path for path, _ in r.programs()}
    taken = set(progs)
    for w in r.lines("multicall"):
        if w[0] not in progs:
            p.append("%s: multicall %s is not a program line" % (r.path, w[0]))
        for n in w[1:]:
            if not FILE_NAME.match(n):
                p.append("%s: multicall name '%s'" % (r.path, n))
                continue
            # A name is a file beside its program: not another's, not twice.
            full = os.path.join(os.path.dirname(w[0]), n)
            if full in taken:
                p.append("%s: multicall name '%s' is a program, or given twice" % (r.path, n))
            taken.add(full)
    return p


def _lint_security(r):
    p = []
    cpe = r.get("cpe")
    if cpe is not None and not re.match(r"^cpe:2\.3:[aho]:[^:\s]+:[^:\s]+", cpe):
        p.append("%s: cpe must start cpe:2.3:a:<vendor>:<product>" % r.path)
    for w in r.lines("fixed") + r.lines("not-affected"):
        if not CVE.match(w[0]):
            p.append("%s: '%s' is not a CVE name" % (r.path, w[0]))
    return p
