"""Cases for the C readers of the ports formats (user/lib/pkg*.c) from the
Python reference reader (pkgverify.py): `ports.py pkgfuzz DIR` mutates the
test vectors in ports/vectors in the ways a damaged or hostile repository
could (bytes, lines, fields), asks the reference reader about each, and
writes the texts, the archives and the answers into DIR, with CASES:

    manifest|index|vulns <file> ok|refused    a text, read without its
                                              signature being checked
    package <file> ok|refused                 an archive, signature checked
                                              against the vectors' test.pub
    version x<hex a> x<hex b> <-1|0|1>        two versions
    path x<hex> ok|refused                    a path

tools/pkgcheck.c (`make check`) gives every case to the C readers, built
with the sanitisers, and any difference fails: the bytes Python writes
are the bytes Jam OS accepts, and nothing the reference refuses is
taken. The mutations come from a fixed seed, so a run repeats.

Where the C readers are stricter or looser than the reference by design
(they check a program's list with the words `allow` refuses, not with
the build's whole rule set), the reference side is held to the C rule
here (c_list_ok), so a difference is a difference of the formats."""
import functools
import os
import random
import shutil

from . import pkgformat as fmt
from . import pkgverify
from . import ustar

UNCHECKED = pkgverify.UNCHECKED

# What init refuses a program from a package (user/lib/pkglist.c).
REFUSED_LINES = {"svc devmgr", "svc devmgr-ctl", "svc volumes", "svc init", "svc net-sys",
                 "svc dns-sys", "svc serve", "svc session", "svc net listen low", "svc net-low",
                 "right debug"}

TOKENS = ["0", "1", "2", "00", "-", "-1", "644", "755", "600", "a", "A", "vec", "bin/vec",
          "../x", "/x", "x" * 64, "0" * 64, "f" * 64, "F" * 64, "9999999999",
          "99999999999999999999", "4194304", "67108864", "67108865", "1.0", "1.0.0", "z",
          "+x", "!x", "a.", ".", "..", "MIT", "runs", "file", "signature", "\\x0a", "\\", "~"]
SPECIAL = [b"\n", b" ", b"\0", b"\xff", b"0", b"9", b"-", b"/", b".", b"!", b"a", b"f", b"\r"]


def c_list_ok(listed):
    """The C reader's rule for a program's list (decoded bytes)."""
    lines = listed.decode("ascii").split("\n")[:-1]
    return not (set(lines) & REFUSED_LINES) and "posix" in lines


def py_manifest(text):
    lines, _ = pkgverify.signed_lines(text, fmt.PKG_FIRST, fmt.MANIFEST_MAX, UNCHECKED)
    m = pkgverify.parse_manifest(lines)
    for _, listed in m.programs:
        pkgverify.need(c_list_ok(listed), "a list the C rule refuses")
    return m


def py_index(text):
    return pkgverify.verify_index(text, UNCHECKED)


def py_vulns(text):
    return pkgverify.verify_vulns(text, UNCHECKED)


def verdict(fn, *args):
    try:
        fn(*args)
        return "ok"
    except pkgverify.Refused:
        return "refused"


def mutate_bytes(rng, data):
    b = bytearray(data)
    for _ in range(rng.choice([1, 1, 2, 3])):
        if not b:
            break
        at = rng.randrange(len(b))
        how = rng.randrange(6)
        if how == 0:
            b[at] ^= 1 << rng.randrange(8)
        elif how == 1:
            b[at] = rng.randrange(256)
        elif how == 2:
            b[at:at + 1] = rng.choice(SPECIAL)
        elif how == 3:
            del b[at:at + rng.randrange(1, 12)]
        elif how == 4:
            b[at:at] = b"".join(rng.choice(SPECIAL) for _ in range(rng.randrange(1, 6)))
        else:
            del b[at:]
    return bytes(b)


def mutate_lines(rng, text):
    """Line- and field-level changes to a signed text, keeping its first
    and last lines unless the mutation is a byte one."""
    lines = text.decode("ascii").split("\n")
    body = lines[1:-2]   # between the first line and the signature line
    sig = lines[-2]
    for _ in range(rng.choice([1, 1, 2])):
        if not body:
            break
        how = rng.randrange(7)
        i = rng.randrange(len(body))
        if how == 0:
            del body[i]
        elif how == 1:
            body.insert(i, body[i])
        elif how == 2 and len(body) > 1:
            j = rng.randrange(len(body))
            body[i], body[j] = body[j], body[i]
        elif how in (3, 4, 5):
            w = body[i].split(" ")
            k = rng.randrange(len(w))
            if how == 3:
                w[k] = rng.choice(TOKENS)
            elif how == 4:
                w[k] = rng.choice(rng.choice(body).split(" "))
            else:
                w.insert(k, rng.choice(["", rng.choice(TOKENS)]))
            body[i] = " ".join(w)
        else:
            body.insert(i, rng.choice(["future-thing a value", "!must-know", "k", "k ", "K v",
                                       "", "x" * 40 + " y"]))
    if rng.random() < 0.5:
        sig = "signature"
    return ("\n".join([lines[0]] + body + [sig]) + "\n").encode("ascii")


def valid_edits(rng, text):
    """Changes that keep a text valid more often than not (so the cases are
    not all refusals): an extension line, a bare signature, another
    value of a field."""
    s = text.decode("ascii")
    lines = s.split("\n")
    i = rng.randrange(1, max(2, len(lines) - 2))
    ext = rng.choice(["note a value", "k", "x-y-z 1", "future-thing some text here"])
    if rng.random() < 0.6:
        lines.insert(i, ext)
    lines[-2] = "signature"
    return "\n".join(lines).encode("ascii")


def text_variants(rng, base, n):
    out = []
    for i in range(n):
        how = rng.randrange(10)
        if how < 4:
            out.append(mutate_lines(rng, base))
        elif how < 6:
            out.append(mutate_bytes(rng, base))
        elif how < 8:
            out.append(valid_edits(rng, base))
        else:
            out.append(valid_edits(rng, mutate_lines(rng, base)) if rng.random() < 0.5 else base)
    return out


def manifest_text(pkg):
    path, mode, n = ustar.parse_header(pkg[:ustar.BLOCK])
    return pkg[ustar.BLOCK:ustar.BLOCK + n]


@functools.lru_cache(maxsize=None)
def _check(text, first, pub):
    return _orig_check(text, first, pub)


_orig_check = pkgverify.signmod.check


def py_package(data, pub, abi):
    return pkgverify.verify_package(data, pub, abi)


def version_cases(rng, out):
    alphabet = "0123456789abAB.-_+~0123"
    pairs = [("1.2", "1.2.1"), ("1.0", "1_0"), ("01", "1"), ("2", "a"), ("1.0a", "1.0")]
    for _ in range(700):
        a = "".join(rng.choice(alphabet) for _ in range(rng.randrange(1, 12)))
        b = a if rng.random() < 0.2 else "".join(rng.choice(alphabet) for _ in range(rng.randrange(1, 12)))
        if rng.random() < 0.3:   # near copies exercise the tie rules
            b = list(a)
            b[rng.randrange(len(b))] = rng.choice(alphabet)
            b = "".join(b)
        pairs.append((a, b))
    for a, b in pairs:
        out.append("version x%s x%s %d" % (a.encode().hex(), b.encode().hex(), fmt.vcmp(a, b)))


def path_cases(rng, out):
    pieces = ["a", "bin", "vim", "..", ".", "", "+x", "a.", "A", "x y", "é", "*", ":", "?", "\\",
              "|", "\"", "<", ">", "\x7f", "\t", "-", "_", "+"]
    paths = ["", "a" * 255, "a" * 256, "a/" * 127 + "a", "a/" * 128]
    for _ in range(700):
        paths.append("/".join(rng.choice(pieces) for _ in range(rng.randrange(1, 5))))
    for p in paths:
        raw = p.encode("latin-1", "replace")
        s = raw.decode("latin-1")
        out.append("path x%s %s" % (raw.hex(), "ok" if fmt.path_problem(s) is None else "refused"))


def make(vectors_dir, outdir, count=600, seed=0x706b67):
    """Write the cases into outdir (emptied first); (the number of cases, how many the
    reference accepted)."""
    rng = random.Random(seed)
    shutil.rmtree(outdir, ignore_errors=True)
    os.makedirs(outdir)

    def read(name):
        with open(os.path.join(vectors_dir, name), "rb") as f:
            return f.read()
    pub = os.path.join(vectors_dir, "test.pub")
    pkgverify.signmod.check = _check
    try:
        return _make(rng, read, pub, outdir, count)
    finally:
        pkgverify.signmod.check = _orig_check


def _make(rng, read, pub, outdir, count):
    lines, accepted, n = [], 0, 0
    abi = "0123456789abcdef"
    good = read("good.jpk")
    bases = [("manifest", manifest_text(good), py_manifest), ("index", read("INDEX-good"), py_index),
             ("vulns", read("VULNS-good"), py_vulns)]
    for kind, base, fn in bases:
        for text in text_variants(rng, base, count):
            n += 1
            name = "t%05d" % n
            with open(os.path.join(outdir, name), "wb") as f:
                f.write(text)
            v = verdict(fn, text)
            accepted += v == "ok"
            lines.append("%s %s %s" % (kind, name, v))
    for name in ("good.jpk", "data.jpk", "p-ext.jpk", "multi.jpk"):
        base = read(name)
        for _ in range(count // 3):
            data = mutate_bytes(rng, base) if rng.random() < 0.9 else base
            n += 1
            fname = "a%05d" % n
            with open(os.path.join(outdir, fname), "wb") as f:
                f.write(data)
            v = verdict(py_package, data, pub, abi)
            accepted += v == "ok"
            lines.append("package %s %s" % (fname, v))
    version_cases(rng, lines)
    path_cases(rng, lines)
    with open(os.path.join(outdir, "CASES"), "w") as f:
        f.write("\n".join(lines) + "\n")
    return len(lines), accepted


def main(top, ports, outdir):
    vectors_dir = os.path.join(ports, "vectors")
    total, accepted = make(vectors_dir, outdir)
    print("ports pkgfuzz: %d cases in %s (%d texts and archives taken by the reference)" %
          (total, outdir, accepted))
    # Cases that are all refusals would test nothing: some must be taken.
    if accepted < 40:
        print("ports pkgfuzz: FAIL: the reference took only %d cases" % accepted)
        return 1
    return 0
