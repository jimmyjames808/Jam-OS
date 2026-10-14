"""The ports formats' test vectors, ports/vectors/ (`ports.py vectors`,
in `make check`): a good package, index and vulnerability list, and every
damaged or hostile form of them a reader must refuse, each with the
answer it must give in ports/vectors/CASES. The reference verifier
(pkgverify.py) is held to CASES here; the PC's C parser
(user/lib/pkgfmt.c) is to be held to the same file.

Everything is made from fixed inputs: a tiny synthetic program (an ELF
file with a list note, two pages long; it never runs), fixed text, a
fixed ABI stamp, and two throwaway keys whose seeds are public
(sign.test_key: Ed25519 signs the same bytes alike every time). So the
vectors change only when the formats do: `ports.py vectors` makes them
afresh and fails if they differ from the committed ones; `ports.py
vectors --write` writes them (then commit them, and the C parser must
agree).

CASES, one case a line (`#` comments):

    <kind> <file> <ok|refused> [<key>=<value>]...

    package <f.jpk>       verify_package with test.pub; abi=<stamp> also
                          demands that ABI stamp
    index <INDEX file>    verify_index; last=<serial>[:<sha256>] is the
                          last index accepted (no sha256: another text)
    vulns <VULNS file>    verify_vulns; last= as for index
    inindex <INDEX file>  check_package_in_index: pkg=<f.jpk> fetched for
                          name=<package>
    audit <VULNS file>    affected() of pkg=<f.jpk>: ids=<id,id,...> is
                          exactly what it must report (or ids=-)
"""
import hashlib
import os
import shutil
import struct
import tempfile

from . import package as packagemod
from . import pagetable
from . import pkgformat as fmt
from . import pkgverify
from . import sign as signmod
from . import ustar

ABI = "0123456789abcdef"
MADE = "2026-10-11T00:00:00Z"
LIST = b"posix\nmount /data r\n"


def tiny_elf(text, length=4100):
    """A static x86-64 ELF executable of length bytes whose one note is the
    list text: the header, a PT_LOAD of the whole file, a PT_NOTE."""
    desc = text + b"\0"
    note = struct.pack("<III", 6, len(desc), 1) + b"JamOS\0\0\0" + desc + b"\0" * (-len(desc) % 4)
    phoff, notes_at = 64, 64 + 2 * 56
    head = b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8
    head += struct.pack("<HHIQQQIHHHHHH", 2, 62, 1, 0x401000, phoff, 0, 0, 64, 56, 2, 64, 0, 0)
    load = struct.pack("<IIQQQQQQ", 1, 5, 0, 0x400000, 0x400000, length, length, 0x1000)
    pnote = struct.pack("<IIQQQQQQ", 4, 4, notes_at, 0x400000 + notes_at, 0x400000 + notes_at,
                        len(note), len(note), 4)
    body = head + load + pnote + note
    fill = bytes((i * 7 + 3) & 0xff for i in range(length - len(body)))
    return body + fill


class FakeRecipe:
    """What package.Package reads of a recipe."""

    def __init__(self, name, version="1.0", revision=1):
        self.name, self.version, self.revision = name, version, revision
        self.values = {"licence": "MIT", "summary": "a test vector, never installed"}

    def get(self, key, default=None):
        return self.values.get(key, default)


def good_package(name="vec"):
    p = packagemod.Package(name, FakeRecipe(name))
    p.built_with = [["musl", "1.2.6"], ["zlib", "1.3.1", "1"]]
    p.runs = [("vec-data", "1.0")] if name == "vec" else []
    elf = tiny_elf(LIST)
    if name == "vec":
        p.add("bin/vec", 0o755, elf)
        p.lists["bin/vec"] = LIST
    p.add("share/%s/notes.txt" % name, 0o644, b"a data file\n")
    p.add("share/licences/%s/LICENSE" % name, 0o644, b"MIT, for a test vector\n")
    return p


def multi_package():
    """A bigger package, for the readers that stream: three programs (one
    page, a page and a byte, three pages and a few bytes; one in a
    deeper directory, one with a capital), forty small files (one empty),
    a path long enough to need the ustar prefix, and two directories that
    differ by more than case."""
    p = packagemod.Package("vec-multi", FakeRecipe("vec-multi"))
    p.built_with = [["musl", "1.2.6"], ["sysroot", "0123456789abcdef"]]
    progs = [("bin/alpha", LIST, 4096), ("bin/Beta", b"posix\n", 4097),
             ("libexec/tool/deep/gamma", LIST, 3 * 4096 + 5)]
    for path, text, length in progs:
        p.add(path, 0o755, tiny_elf(text, length))
        p.lists[path] = text
    for i in range(40):
        p.add("share/vec-multi/d%d/f%02d.txt" % (i // 8, i), 0o644, (b"file %d\n" % i) * i)
    p.add("share/vec-multi/%s/%s/%s.txt" % ("d" * 60, "n" * 60, "x" * 50), 0o644, b"long\n")
    p.add("share/vec-multi/Case/a.txt", 0o644, b"upper\n")
    p.add("share/vec-multi/case2/a.txt", 0o644, b"lower\n")
    p.add("share/licences/vec-multi/LICENSE", 0o644, b"MIT, for a test vector\n")
    return p


class Maker:
    """Writes the vectors and their CASES lines into a directory."""

    def __init__(self, d):
        self.d = d
        self.key, self.pub = signmod.test_key(os.path.join(d, ".keys"), "ports vectors")
        self.key2, _ = signmod.test_key(os.path.join(d, ".keys2"), "ports vectors, another")
        self.cases = []

    def write(self, name, data):
        with open(os.path.join(self.d, name), "wb") as f:
            f.write(data)

    def case(self, kind, name, expect, *extra):
        self.cases.append(" ".join((kind, name, expect) + extra))

    def signed(self, text, first, key=None):
        return signmod.sign(text, first, key or self.key)

    def package(self, name, p, expect, manifest=None, members=None, key=None, extra=(),
                sign=True):
        """A .jpk of p (or of the manifest text given, and the members
        given) as a case; the manifest signed unless sign is False."""
        text = manifest if manifest is not None else packagemod.manifest(p, ABI)
        signed = self.signed(text, fmt.PKG_FIRST, key) if sign else text
        data = packagemod.archive(p, signed) if members is None else ustar.archive(
            [("+MANIFEST", 0o644, signed)] + members)
        self.write(name, data)
        self.case("package", name, expect, *extra)
        return data


def sub(text, old, new):
    assert old in text, old
    return text.replace(old, new, 1)


def header_at(data, path):
    """Where member path's header starts in an archive."""
    at = 0
    while True:
        name, _, size = ustar.parse_header(data[at:at + ustar.BLOCK])
        if name == path:
            return at
        at += ustar.BLOCK + size + (-size % ustar.BLOCK)


def members_of(p, signed=b""):
    return [(x, m, d) for x, m, d in ustar.members(packagemod.archive(p, signed or b"x"))][1:]


def package_cases(mk):
    p = good_package()
    good = mk.package("good.jpk", p, "ok", extra=("abi=" + ABI,))
    mk.case("package", "good.jpk", "refused", "abi=fedcba9876543210")
    data = bytearray(good)
    at = good.index(b"\x7fELF") + 300
    data[at] ^= 1
    mk.write("p-data.jpk", bytes(data))
    mk.case("package", "p-data.jpk", "refused")
    text = packagemod.manifest(p, ABI)
    signed = mk.signed(text, fmt.PKG_FIRST)
    mk.package("p-manifest.jpk", p, "refused", manifest=sub(signed, b"version 1.0", b"version 1.1"),
               members=members_of(p), sign=False)
    mk.package("p-otherkey.jpk", p, "refused", key=mk.key2)
    mk.package("p-unsigned.jpk", p, "refused", manifest=text, members=members_of(p), sign=False)
    relabelled = fmt.INDEX_FIRST.encode() + signed[len(fmt.PKG_FIRST):]
    mk.package("p-domain.jpk", p, "refused", manifest=relabelled, members=members_of(p),
               sign=False)
    _member_cases(mk, p, good)
    _grammar_cases(mk, p, text)


def _member_cases(mk, p, good):
    """The archive differs from the signed manifest."""
    ms = members_of(p)
    files = [m for m in ms if not m[0].startswith("+")]
    tables = [m for m in ms if m[0].startswith("+")]
    bad_table = bytearray(tables[0][2])
    bad_table[-1] ^= 1
    mk.package("p-table.jpk", p, "refused",
               members=files + [(tables[0][0], 0o644, bytes(bad_table))])
    # A table whose bytes the manifest signs but that isn't the program's.
    wrong = bytes(bad_table)
    q = good_package()
    text = sub(packagemod.manifest(q, ABI), hashlib.sha256(tables[0][2]).hexdigest().encode(),
               hashlib.sha256(wrong).hexdigest().encode())
    mk.package("p-table-signed.jpk", q, "refused", manifest=text,
               members=files + [(tables[0][0], 0o644, wrong)])
    mk.package("p-extra.jpk", p, "refused", members=ms + [("share/vec/more.txt", 0o644, b"x\n")])
    mk.package("p-missing.jpk", p, "refused", members=ms[1:])
    mk.package("p-order.jpk", p, "refused", members=[ms[1], ms[0]] + ms[2:])
    mk.package("p-mode.jpk", p, "refused", members=[(ms[0][0], 0o644, ms[0][2])] + ms[1:])
    # A header that isn't the canonical one: mtime 1, checksum fixed.
    data = bytearray(good)
    at = header_at(good, "bin/vec")
    h = bytearray(data[at:at + 512])
    h[136:148] = b"00000000001\0"
    h[148:156] = b" " * 8
    h[148:156] = b"%06o\0 " % sum(h)
    data[at:at + 512] = h
    mk.write("p-header.jpk", bytes(data))
    mk.case("package", "p-header.jpk", "refused")
    mk.write("p-trailing.jpk", good + b"\0" * 512)
    mk.case("package", "p-trailing.jpk", "refused")
    mk.write("p-noend.jpk", good[:-1024])
    mk.case("package", "p-noend.jpk", "refused")
    mk.write("p-cut.jpk", good[:len(good) // 2])
    mk.case("package", "p-cut.jpk", "refused")


def _grammar_cases(mk, p, text):
    """Manifests the key signed that break the grammar: the reader must
    refuse them even so."""
    ms = members_of(p)
    cases = [
        ("p-dotdot.jpk", sub(text, b"file share/vec/notes.txt", b"file share/../notes.txt")),
        ("p-runs-self.jpk", sub(text, b"runs vec-data 1.0", b"runs vec 1.0")),
        ("p-must.jpk", sub(text, b"summary a test vector, never installed\n",
                           b"summary a test vector, never installed\n!future-need x\n")),
        ("p-lines-order.jpk", sub(text, b"built-with musl 1.2.6\nbuilt-with zlib 1.3.1 1",
                                  b"built-with zlib 1.3.1 1\nbuilt-with musl 1.2.6")),
        ("p-size.jpk", sub(text, b"notes.txt 644 12 ", b"notes.txt 644 13 ")),
        ("p-mode-line.jpk", sub(text, b"notes.txt 644", b"notes.txt 755")),
        ("p-two-spaces.jpk", sub(text, b"name vec", b"name  vec")),
        ("p-cr.jpk", sub(text, b"name vec\n", b"name vec\r\n")),
        ("p-backslash.jpk", sub(text, b"\\x0amount", b"\\x0bmount")),
    ]
    for name, t in cases:
        mk.package(name, p, "refused", manifest=t, members=ms)
    mk.package("p-ext.jpk", p, "ok", manifest=sub(
        text, b"summary a test vector, never installed\n",
        b"summary a test vector, never installed\nfuture-thing a value this reader skips\n"),
        members=ms)
    # Case-different paths, signed with their members.
    q = good_package()
    q.add("share/vec/Notes.txt", 0o644, b"x\n")
    mk.package("p-case.jpk", q, "refused", manifest=packagemod.manifest(q, ABI))
    # A list the program's note doesn't carry; one asking for svc session.
    q = good_package()
    q.lists["bin/vec"] = b"posix\nmount /data rw\n"
    mk.package("p-list.jpk", q, "refused")
    q = good_package()
    q.files["bin/vec"] = (0o755, tiny_elf(b"posix\nsvc session\n"))
    q.lists["bin/vec"] = b"posix\nsvc session\n"
    mk.package("p-session.jpk", q, "refused")


def index_cases(mk, good, data_pkg):
    def entry(name, blob, port, runs=()):
        out = ["package %s 1.0 1 %d %s %s" % (name, len(blob), hashlib.sha256(blob).hexdigest(),
                                              port)]
        return out + ["runs %s %s" % (name, r) for r in runs]
    src = b"a source tarball's bytes"
    srcline = "src vec 1.0 1 %d %s" % (len(src), hashlib.sha256(src).hexdigest())
    head = ["serial 5", "made " + MADE, "abi " + ABI]
    body = head + entry("vec", good, "vec", ["vec-data 1.0"]) + entry("vec-data", data_pkg, "vec") \
        + [srcline]
    text = ("\n".join([fmt.INDEX_FIRST] + body + ["signature"]) + "\n").encode()
    signed = mk.signed(text, fmt.INDEX_FIRST)
    mk.write("INDEX-good", signed)
    digest = hashlib.sha256(signed).hexdigest()
    mk.case("index", "INDEX-good", "ok")
    mk.case("index", "INDEX-good", "ok", "last=4")
    mk.case("index", "INDEX-good", "ok", "last=5:" + digest)
    mk.case("index", "INDEX-good", "refused", "last=5")
    mk.case("index", "INDEX-good", "refused", "last=6")
    mk.case("inindex", "INDEX-good", "ok", "pkg=good.jpk", "name=vec")
    mk.case("inindex", "INDEX-good", "refused", "pkg=p-ext.jpk", "name=vec")
    mk.case("inindex", "INDEX-good", "refused", "pkg=good.jpk", "name=vec-data")
    mk.write("INDEX-tampered", sub(signed, b"serial 5", b"serial 7"))
    mk.case("index", "INDEX-tampered", "refused")
    mk.write("INDEX-otherkey", mk.signed(text, fmt.INDEX_FIRST, mk.key2))
    mk.case("index", "INDEX-otherkey", "refused")
    mk.write("INDEX-domain", fmt.VULNS_FIRST.encode() + signed[len(fmt.INDEX_FIRST):])
    mk.case("index", "INDEX-domain", "refused")
    mk.case("vulns", "INDEX-domain", "refused")
    mk.case("index", "good.jpk", "refused")
    bad = [("INDEX-runs-missing", b"runs vec vec-data 1.0", b"runs vec vec-lib 1.0"),
           ("INDEX-runs-version", b"runs vec vec-data 1.0", b"runs vec vec-data 2.0"),
           ("INDEX-nosrc", srcline.encode() + b"\n", b"")]
    for name, old, new in bad:
        mk.write(name, mk.signed(sub(text, old, new), fmt.INDEX_FIRST))
        mk.case("index", name, "refused")
    swapped = ["serial 5", "made " + MADE, "abi " + ABI] + entry("vec-data", data_pkg, "vec") \
        + entry("vec", good, "vec", ["vec-data 1.0"]) + [srcline]
    mk.write("INDEX-order", mk.signed(("\n".join([fmt.INDEX_FIRST] + swapped + ["signature"]) +
                                       "\n").encode(), fmt.INDEX_FIRST))
    mk.case("index", "INDEX-order", "refused")


def vulns_cases(mk):
    body = ["serial 5", "made " + MADE,
            "vuln vec - - - JAMOS-2026-1 every vec so far",
            "vuln zlib - 1.2.13 - CVE-2022-37434 inflateGetHeader's extra field overflow",
            "vuln zlib 1.2.0 1.3.1 2 CVE-2026-0001 fixed in 1.3.1's second revision"]
    text = ("\n".join([fmt.VULNS_FIRST] + body + ["signature"]) + "\n").encode()
    signed = mk.signed(text, fmt.VULNS_FIRST)
    mk.write("VULNS-good", signed)
    mk.case("vulns", "VULNS-good", "ok")
    mk.case("vulns", "VULNS-good", "refused", "last=6")
    mk.case("audit", "VULNS-good", "ok", "pkg=good.jpk", "ids=CVE-2026-0001,JAMOS-2026-1")
    mk.write("VULNS-tampered", sub(signed, b"every vec", b"every Vec"))
    mk.case("vulns", "VULNS-tampered", "refused")
    mk.write("VULNS-domain", fmt.INDEX_FIRST.encode() + signed[len(fmt.VULNS_FIRST):])
    mk.case("vulns", "VULNS-domain", "refused")
    mk.case("index", "VULNS-domain", "refused")
    for name, old, new in [("VULNS-order", b"vuln vec", b"vuln zzz"),
                           ("VULNS-fixrev", b"1.2.13 - CVE", b"- 2 CVE")]:
        mk.write(name, mk.signed(sub(text, old, new), fmt.VULNS_FIRST))
        mk.case("vulns", name, "refused")


def make_all(d):
    """Every vector and CASES into d (emptied first)."""
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    mk = Maker(d)
    package_cases(mk)
    with open(os.path.join(d, "good.jpk"), "rb") as f:
        good = f.read()
    data_pkg = mk.package("data.jpk", good_package("vec-data"), "ok")
    mk.package("multi.jpk", multi_package(), "ok")
    index_cases(mk, good, data_pkg)
    vulns_cases(mk)
    shutil.copyfile(mk.pub, os.path.join(d, "test.pub"))
    shutil.rmtree(os.path.join(d, ".keys"))
    shutil.rmtree(os.path.join(d, ".keys2"))
    with open(os.path.join(d, "CASES"), "w") as f:
        f.write("# The ports formats' test vectors: tools/portslib/vectors.py says what each\n"
                "# line means. Every file here is made by `tools/ports.py vectors --write`.\n")
        f.write("".join(c + "\n" for c in mk.cases))


def run_case(d, line):
    """None if the case gives its answer, else what went wrong."""
    w = line.split()
    kind, name, expect = w[:3]
    opts = dict(x.split("=", 1) for x in w[3:])
    pub = os.path.join(d, "test.pub")

    def read(n):
        with open(os.path.join(d, n), "rb") as f:
            return f.read()
    last = None
    if "last" in opts:
        s, _, h = opts["last"].partition(":")
        last = (int(s), h)
    try:
        if kind == "package":
            pkgverify.verify_package(read(name), pub, opts.get("abi"))
        elif kind == "index":
            pkgverify.verify_index(read(name), pub, last)
        elif kind == "vulns":
            pkgverify.verify_vulns(read(name), pub, last)
        elif kind == "inindex":
            x = pkgverify.verify_index(read(name), pub)
            pkgverify.check_package_in_index(x, opts["name"], read(opts["pkg"]))
        elif kind == "audit":
            v = pkgverify.verify_vulns(read(name), pub)
            m = pkgverify.verify_package(read(opts["pkg"]), pub)
            got = ",".join(sorted(i for _, i, _ in pkgverify.affected(v, m))) or "-"
            if got != opts["ids"]:
                return "%s: audit gave %s, not %s" % (line, got, opts["ids"])
        got, why = "ok", ""
    except pkgverify.Refused as e:
        got, why = "refused", str(e)
    if got != expect:
        return "%s: %s%s" % (line, got, " (%s)" % why if why else "")
    return None


def check(d):
    """Every case of d/CASES: [problem]."""
    out = []
    with open(os.path.join(d, "CASES")) as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                why = run_case(d, line.strip())
                if why:
                    out.append(why)
    return out


def same_dirs(a, b):
    """The names in a and b that differ (or are only in one)."""
    na, nb = set(os.listdir(a)), set(os.listdir(b))
    out = sorted(na ^ nb)
    for n in sorted(na & nb):
        with open(os.path.join(a, n), "rb") as fa, open(os.path.join(b, n), "rb") as fb:
            if fa.read() != fb.read():
                out.append(n)
    return out


def main(top, ports, write):
    d = os.path.join(ports, "vectors")
    if write:
        make_all(d)
        problems = check(d)
    else:
        with tempfile.TemporaryDirectory(prefix="ports-vectors.") as tmp:
            fresh = os.path.join(tmp, "v")
            make_all(fresh)
            problems = check(fresh)
            if not os.path.isdir(d):
                problems.append("no %s: `tools/ports.py vectors --write`" % d)
            else:
                differ = same_dirs(fresh, d)
                problems += ["%s: not what the formats make now (`tools/ports.py vectors "
                             "--write`, then commit)" % n for n in differ]
            problems += ["committed: %s" % p for p in (check(d) if os.path.isdir(d) else [])]
    for p in problems:
        print("ports vectors: " + p)
    n = 0
    if os.path.isfile(os.path.join(d, "CASES")):
        with open(os.path.join(d, "CASES")) as f:
            n = sum(1 for line in f if line.strip() and not line.startswith("#"))
    print("ports vectors: %d cases, %s" % (n, "FAIL" if problems else "PASS"))
    return 1 if problems else 0
