"""The reference verifier of the ports formats: what bin/pkg and init do
with a package, an index or a vulnerability list (with user/lib/pkgfmt.c,
not built yet), written in Python so the Mac's self-test can refuse every
damaged form the PC must refuse, and the C can be held to the same
answers on the same test vectors (ports/vectors/).

It is written as the C will be: the text's bytes bounded first, the
signature checked before any field is believed (jamos-sign, the same
Monocypher), every line matched against its grammar in its fixed place,
every count and size bounded, and the archive read member by member
against the manifest, never the other way round. Each refusal raises
Refused with the reason; nothing is ever repaired or guessed.

    verify_package(data, pub, abi=None)       a .jpk's bytes -> Manifest
    verify_index(text, pub, last=None)        INDEX -> Index
    verify_vulns(text, pub, last=None)        VULNS -> Vulns
    check_package_in_index(index, name, data) a fetched package against
                                              the index's size and hash
    affected(vulns, manifest)                 `pkg audit` for one package

`last` is (serial, sha256 of the text) of the last one accepted, kept
by pkg in /data/pkg/db: a lower serial is a replay and refused; the same
serial is taken only for the same bytes.
"""
import hashlib
import os
import sys

from . import pagetable
from . import pkgformat as fmt
from . import sign as signmod
from . import ustar

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import checkwants  # noqa: E402


class Refused(Exception):
    pass


UNCHECKED = object()   # as pub: don't check the signature (an unsigned package's tests)


def need(cond, why):
    if not cond:
        raise Refused(why)


# ---- the common text rules --------------------------------------------------------------

def signed_lines(text, first, maxlen, pub):
    """The lines of a signed text between its first line and its signature
    line, after the signature checked: (lines, sha256 of the text). pub
    UNCHECKED skips the signature (`ports.py package --unsigned` only: a
    text whose signature line is bare is refused everywhere else)."""
    need(isinstance(text, (bytes, bytearray)) and 0 < len(text) <= maxlen,
         "not 1..%d bytes" % maxlen)
    need(text.startswith((first + "\n").encode()), "the first line isn't %r" % first)
    need(text.endswith(b"\n"), "the last line has no newline")
    need(all(c == 0x0a or 0x20 <= c <= 0x7e for c in text), "a byte that isn't printable ASCII")
    lines = text[:-1].decode("ascii").split("\n")
    sig = lines[-1].split(" ")
    if pub is UNCHECKED:
        need(lines[-1] == "signature" or
             (len(sig) == 2 and sig[0] == "signature" and fmt.SIG_HEX.match(sig[1])),
             "no signature line")
        return lines[1:-1], hashlib.sha256(text).hexdigest()
    need(len(lines) >= 3 and len(sig) == 2 and sig[0] == "signature" and fmt.SIG_HEX.match(sig[1]),
         "no 'signature <128 hex digits>' last line")
    need(signmod.check(bytes(text), first, pub), "the signature doesn't check (another key, "
         "another format, or the text changed)")
    return lines[1:-1], hashlib.sha256(text).hexdigest()


def words(line, key, lo, hi=None):
    """line's fields after key, lo..hi of them (single spaces only)."""
    w = line.split(" ")
    need(w[0] == key and all(w), "line %r: not a '%s' line of single spaces" % (line, key))
    n = len(w) - 1
    need(lo <= n <= (hi if hi is not None else lo), "line %r: %d fields" % (line, n))
    return w[1:]


def rest(line, key):
    """The rest of a 'key <text>' line."""
    need(line.startswith(key + " "), "no '%s' line where it must be: %r" % (key, line))
    return line[len(key) + 1:]


def field(value, pattern, what):
    need(pattern.match(value) is not None, "%s %r" % (what, value))
    return value


def size(value, top, what):
    need(fmt.SIZE.match(value) is not None and int(value) <= top, "%s %r" % (what, value))
    return int(value)


def extensions(lines, at, known):
    """Skip the extension lines from at: `<key>` or `<key> <value>`; a key
    starting with '!' (must-understand) is refused: this reader knows
    none. Returns the index after them."""
    while at < len(lines):
        key, _, value = lines[at].partition(" ")
        if key in known or not fmt.EXT_KEY.match(key):
            return at
        need(not key.startswith("!"), "a must-understand line this reader doesn't know: %r" % key)
        need(value == "" or fmt.text_ok(value), "an extension line's value: %r" % lines[at])
        need(lines[at] == key or value != "", "an extension line ending in a space")
        at += 1
    return at


def take(lines, at, key):
    """The run of lines from at that start with key: (their lines, the
    index after them)."""
    end = at
    while end < len(lines) and lines[end].split(" ", 1)[0] == key:
        end += 1
    return lines[at:end], end


def increasing(keys, what):
    need(all(a.encode() < b.encode() for a, b in zip(keys, keys[1:])),
         "%s not in strictly increasing byte order" % what)


# ---- the manifest -----------------------------------------------------------------------

class Manifest:
    def __init__(self):
        self.name = self.version = self.abi = self.licence = self.summary = None
        self.revision = 0
        self.runs = []          # [(name, minimum version or None)]
        self.built_with = []    # [(what, version, revision or 0)]
        self.files = []         # [(path, mode, size, sha256)] in order
        self.programs = []      # [(path, list bytes)] in order
        self.tables = []        # [(path, size, sha256)] in order
        self.sha256 = None      # of the manifest's text


PKG_KEYS = {"runs", "built-with", "file", "program", "table", "signature", "name", "version",
            "revision", "abi", "licence", "summary"}


def parse_manifest(lines):
    """A manifest's lines (between the first and the signature line)."""
    m, at = Manifest(), 6
    need(len(lines) >= 7, "a manifest cut short")
    m.name = field(words(lines[0], "name", 1)[0], fmt.NAME, "name")
    m.version = field(words(lines[1], "version", 1)[0], fmt.VERSION, "version")
    m.revision = int(field(words(lines[2], "revision", 1)[0], fmt.REVISION, "revision"))
    m.abi = field(words(lines[3], "abi", 1)[0], fmt.ABI, "abi")
    m.licence = rest(lines[4], "licence")
    need(fmt.LICENCE.match(m.licence) and len(m.licence) <= fmt.TEXT_MAX, "the licence line")
    m.summary = rest(lines[5], "summary")
    need(fmt.text_ok(m.summary), "the summary line")
    at = extensions(lines, at, PKG_KEYS)
    runs, at = take(lines, at, "runs")
    for line in runs:
        w = words(line, "runs", 1, 2)
        m.runs.append((field(w[0], fmt.NAME, "runs"),
                       field(w[1], fmt.VERSION, "runs version") if len(w) == 2 else None))
    increasing([n for n, _ in m.runs], "runs")
    need(len(m.runs) <= fmt.DEPS_MAX and m.name not in [n for n, _ in m.runs], "the runs lines")
    built, at = take(lines, at, "built-with")
    for line in built:
        w = words(line, "built-with", 2, 3)
        m.built_with.append((field(w[0], fmt.NAME, "built-with"),
                             field(w[1], fmt.VERSION, "built-with version"),
                             int(field(w[2], fmt.REVISION, "built-with revision"))
                             if len(w) == 3 else 0))
    increasing([w for w, _, _ in m.built_with], "built-with")
    need(len(m.built_with) <= fmt.DEPS_MAX, "too many built-with lines")
    at = _files(m, lines, at)
    at = _programs(m, lines, at)
    need(at == len(lines), "a line out of place: %r" % (lines[at] if at < len(lines) else ""))
    return m


def _files(m, lines, at):
    files, at = take(lines, at, "file")
    need(0 < len(files) <= fmt.FILES_MAX, "%d file lines (1..%d)" % (len(files), fmt.FILES_MAX))
    total = 0
    for line in files:
        w = words(line, "file", 4)
        why = fmt.path_problem(w[0])
        need(why is None, "file %r: %s" % (w[0], why))
        need(w[1] in fmt.MODE, "file %s: mode %r" % (w[0], w[1]))
        n = size(w[2], fmt.FILE_MAX, "file size")
        total += n
        m.files.append((w[0], int(w[1], 8), n, field(w[3], fmt.SHA, "file sha256")))
    paths = [p for p, _, _, _ in m.files]
    increasing(paths, "file lines")
    need(fmt.clash_problem(paths) is None, "paths FAT32 can't hold side by side: %s" %
         fmt.clash_problem(paths))
    need(total <= fmt.FILES_TOTAL_MAX, "the files are over %d bytes in all" % fmt.FILES_TOTAL_MAX)
    return at


def _programs(m, lines, at):
    progs, at = take(lines, at, "program")
    need(len(progs) <= fmt.PROGRAMS_MAX, "too many program lines")
    for line in progs:
        w = line.split(" ", 2)
        need(len(w) == 3 and w[0] == "program", "a program line: %r" % line)
        try:
            m.programs.append((w[1], fmt.decode_list(w[2])))
        except ValueError as e:
            raise Refused("program %s: its list: %s" % (w[1], e))
    increasing([p for p, _ in m.programs], "program lines")
    modes = {p: (mode, n) for p, mode, n, _ in m.files}
    for p, _ in m.programs:
        need(p in modes and modes[p][0] == 0o755 and modes[p][1] > 0,
             "program %s: not a non-empty file of mode 755" % p)
    need(sum(1 for mode, _ in modes.values() if mode == 0o755) == len(m.programs),
         "a file of mode 755 with no program line")
    tables, at = take(lines, at, "table")
    need(len(tables) == len(m.programs), "not one table line per program")
    for line, (p, _) in zip(tables, m.programs):
        w = words(line, "table", 3)
        need(w[0] == p, "table %s: not in the programs' order" % w[0])
        need(size(w[1], fmt.table_size(fmt.FILE_MAX), "table size") ==
             fmt.table_size(modes[p][1]), "table %s: the size for its file is %d" %
             (p, fmt.table_size(modes[p][1])))
        m.tables.append((p, int(w[1]), field(w[2], fmt.SHA, "table sha256")))
    return at


# ---- the package ------------------------------------------------------------------------

def verify_package(data, pub, abi=None, lists=True):
    """A .jpk's bytes, checked whole: its manifest's signature and grammar,
    then every member against it, in order. abi: the running build's
    stamp, which the manifest's must be (None: not checked). lists: each
    program's ELF note must be its list, and a list `allow` would refuse
    is refused. Returns the Manifest."""
    need(len(data) <= fmt.ARCHIVE_MAX, "over %d bytes" % fmt.ARCHIVE_MAX)
    try:
        path, mode, n = ustar.parse_header(data[:ustar.BLOCK])
    except ustar.UstarError as e:
        raise Refused("the first header: %s" % e)
    need(path == "+MANIFEST" and mode == 0o644 and 0 < n <= fmt.MANIFEST_MAX,
         "the first member isn't a +MANIFEST of 1..%d bytes" % fmt.MANIFEST_MAX)
    need(len(data) >= ustar.BLOCK + n, "the archive ends inside +MANIFEST")
    text = data[ustar.BLOCK:ustar.BLOCK + n]
    lines, digest = signed_lines(text, fmt.PKG_FIRST, fmt.MANIFEST_MAX, pub)
    m = parse_manifest(lines)
    m.sha256 = digest
    need(abi is None or m.abi == abi, "built for ABI %s, this system is %s" % (m.abi, abi))
    try:
        members = ustar.members(data)
    except ustar.UstarError as e:
        raise Refused("the archive: %s" % e)
    want = [("+MANIFEST", 0o644, n)] + [(p, mode, s) for p, mode, s, _ in m.files]
    want += [("+TABLES/" + p, 0o644, s) for p, s, _ in m.tables]
    need(len(members) == len(want), "%d members, the manifest names %d" % (len(members), len(want)))
    for (p, mode, body), (wp, wmode, ws) in zip(members, want):
        need((p, mode, len(body)) == (wp, wmode, ws), "member %s (mode %o, %d bytes) where the "
             "manifest has %s (mode %o, %d bytes)" % (p, mode, len(body), wp, wmode, ws))
    bodies = {p: body for p, _, body in members}
    for p, _, _, sha in m.files:
        need(hashlib.sha256(bodies[p]).hexdigest() == sha, "%s: not the bytes signed" % p)
    for (p, listed), (_, _, sha) in zip(m.programs, m.tables):
        table = bodies["+TABLES/" + p]
        need(hashlib.sha256(table).hexdigest() == sha, "+TABLES/%s: not the bytes signed" % p)
        need(table == pagetable.make(bodies[p]), "+TABLES/%s: not %s's page table" % (p, p))
        if lists:
            _check_list(p, bodies[p], listed)
    return m


def _check_list(path, data, listed):
    try:
        notes = checkwants.notes(data)
    except (ValueError, IndexError) as e:
        raise Refused("program %s: no list readable in it: %s" % (path, e))
    need(notes == [listed], "program %s: its list isn't the manifest's" % path)
    problems = refused_list(listed)
    need(not problems, "program %s asks for what a package can't have: %s" %
         (path, "; ".join(problems)))


def refused_list(text):
    """What init refuses a package's program: what `allow` refuses one on
    /data (tools/checkwants.py's rules for a program outside the services,
    and svc devmgr, right debug)."""
    from . import tree as treemod
    return treemod.list_problems(text, "package", "ports")


# ---- the index --------------------------------------------------------------------------

class Index:
    def __init__(self):
        self.serial = 0
        self.made = self.abi = self.sha256 = None
        self.packages = {}   # name -> dict(version, revision, size, sha256, port, runs)
        self.sources = {}    # port -> dict(version, revision, size, sha256)


INDEX_KEYS = {"package", "runs", "src", "signature", "serial", "made", "abi"}


def serial_line(line, last, digest):
    s = int(field(words(line, "serial", 1)[0], fmt.SERIAL, "serial"))
    need(s <= fmt.SERIAL_MAX, "serial too big")
    if last is not None:
        need(s > last[0] or (s == last[0] and digest == last[1]),
             "serial %d after %d: an old one replayed, or another with the same serial" %
             (s, last[0]))
    return s


def verify_index(text, pub, last=None):
    lines, digest = signed_lines(text, fmt.INDEX_FIRST, fmt.INDEX_MAX, pub)
    x = Index()
    x.sha256 = digest
    need(len(lines) >= 3, "an index cut short")
    x.serial = serial_line(lines[0], last, digest)
    x.made = field(words(lines[1], "made", 1)[0], fmt.MADE, "made")
    x.abi = field(words(lines[2], "abi", 1)[0], fmt.ABI, "abi")
    at = extensions(lines, 3, INDEX_KEYS)
    while at < len(lines) and lines[at].startswith("package "):
        at = _package_line(x, lines, at)
    increasing(list(x.packages), "package lines")
    srcs, at = take(lines, at, "src")
    for line in srcs:
        w = words(line, "src", 5)
        x.sources[field(w[0], fmt.NAME, "src")] = dict(
            version=field(w[1], fmt.VERSION, "src version"),
            revision=int(field(w[2], fmt.REVISION, "src revision")),
            size=size(w[3], fmt.ARCHIVE_MAX, "src size"), sha256=field(w[4], fmt.SHA, "src sha"))
    increasing([line.split(" ")[1] for line in srcs], "src lines")
    need(at == len(lines), "a line out of place: %r" % (lines[at] if at < len(lines) else ""))
    _index_whole(x)
    return x


def _package_line(x, lines, at):
    w = words(lines[at], "package", 6)
    need(len(x.packages) < fmt.PACKAGES_MAX, "too many packages")
    name = field(w[0], fmt.NAME, "package")
    need(name not in x.packages, "package %s twice" % name)
    p = dict(version=field(w[1], fmt.VERSION, "version"),
             revision=int(field(w[2], fmt.REVISION, "revision")),
             size=size(w[3], fmt.ARCHIVE_MAX, "package size"),
             sha256=field(w[4], fmt.SHA, "package sha256"), port=field(w[5], fmt.NAME, "port"),
             runs=[])
    need(p["size"] >= 3 * ustar.BLOCK, "package %s: too small to be one" % name)
    runs, at = take(lines, at + 1, "runs")
    for line in runs:
        r = words(line, "runs", 2, 3)
        need(r[0] == name, "runs %s under package %s" % (r[0], name))
        p["runs"].append((field(r[1], fmt.NAME, "runs"),
                          field(r[2], fmt.VERSION, "runs version") if len(r) == 3 else None))
    increasing([d for d, _ in p["runs"]], "%s's runs" % name)
    need(len(p["runs"]) <= fmt.DEPS_MAX and name not in [d for d, _ in p["runs"]],
         "%s's runs lines" % name)
    x.packages[name] = p
    return at


def _index_whole(x):
    """What holds across lines: every runs names a package of a version it
    takes; every package's source is listed, and every source used."""
    used = set()
    for name, p in x.packages.items():
        for dep, least in p["runs"]:
            need(dep in x.packages, "%s runs %s, which the index hasn't" % (name, dep))
            need(least is None or fmt.vcmp(x.packages[dep]["version"], least) >= 0,
                 "%s runs %s >= %s, the index has %s" % (name, dep, least,
                                                          x.packages[dep]["version"]))
        s = x.sources.get(p["port"])
        need(s is not None and (s["version"], s["revision"]) == (p["version"], p["revision"]),
             "%s: no src line for %s %s r%d" % (name, p["port"], p["version"], p["revision"]))
        used.add(p["port"])
    need(used == set(x.sources), "a src line no package uses")


def check_package_in_index(x, name, data):
    """A fetched package's bytes against the index's line for name."""
    p = x.packages.get(name)
    need(p is not None, "the index has no package %s" % name)
    need(len(data) == p["size"] and hashlib.sha256(data).hexdigest() == p["sha256"],
         "%s: not the package the index names (size or SHA-256)" % name)


# ---- VULNS --------------------------------------------------------------------------------

class Vulns:
    def __init__(self):
        self.serial = 0
        self.made = self.sha256 = None
        self.entries = []   # [(component, first, fixed, fixrev, id, summary)]


def vuln_line(line):
    """One `vuln <component> <first> <fixed> <fixrev> <id> <summary>` line:
    the tuple, fixrev 0 for '-', first and fixed None for '-'."""
    w = line.split(" ", 6)
    need(len(w) == 7 and w[0] == "vuln" and all(w[:6]), "a vuln line: %r" % line)
    comp = field(w[1], fmt.NAME, "component")
    first = None if w[2] == "-" else field(w[2], fmt.VERSION, "first affected")
    fixed = None if w[3] == "-" else field(w[3], fmt.VERSION, "fixed in")
    need(w[4] == "-" or (fixed is not None and fmt.REVISION.match(w[4])),
         "fixrev %r (a revision of the fixed version, or '-')" % w[4])
    need(fmt.text_ok(w[6]), "a vuln line's summary")
    return (comp, first, fixed, 0 if w[4] == "-" else int(w[4]),
            field(w[5], fmt.VULN_ID, "vuln id"), w[6])


def verify_vulns(text, pub, last=None):
    lines, digest = signed_lines(text, fmt.VULNS_FIRST, fmt.VULNS_MAX, pub)
    v = Vulns()
    v.sha256 = digest
    need(len(lines) >= 2, "a vulnerability list cut short")
    v.serial = serial_line(lines[0], last, digest)
    v.made = field(words(lines[1], "made", 1)[0], fmt.MADE, "made")
    at = extensions(lines, 2, {"vuln", "signature", "serial", "made"})
    entries, at = take(lines, at, "vuln")
    need(len(entries) <= fmt.VULNS_ENTRIES_MAX, "too many vuln lines")
    v.entries = [vuln_line(line) for line in entries]
    keys = ["%s %s" % (e[0], e[4]) for e in v.entries]
    increasing(keys, "vuln lines (by component, then id)")
    need(at == len(lines), "a line out of place: %r" % (lines[at] if at < len(lines) else ""))
    return v


def is_affected(entry, version, revision):
    """Whether a component at version (and revision; 0 when it has none)
    is inside entry's range."""
    _, first, fixed, fixrev, _, _ = entry
    if first is not None and fmt.vcmp(version, first) < 0:
        return False
    if fixed is None:
        return True
    c = fmt.vcmp(version, fixed)
    return c < 0 or (c == 0 and fixrev > 0 and revision < fixrev)


def affected(v, m):
    """`pkg audit` for one installed package: [(component, id, summary)]
    for the package itself and each of its built-with lines."""
    have = [(m.name, m.version, m.revision)] + list(m.built_with)
    out = []
    for e in v.entries:
        for comp, version, revision in have:
            if comp == e[0] and is_affected(e, version, revision):
                out.append((comp, e[4], e[5]))
    return out
