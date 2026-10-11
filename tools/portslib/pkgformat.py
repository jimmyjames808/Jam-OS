"""The ports formats' shared definitions: limits, field grammars, the
version order, the ABI stamp, the program list's encoding and the
repository's file names (docs/M13.7-PLAN.md sections 6, 7 and 13; the
grammars in full are the plan's "PO3 notes").

Four signed texts share one shape, the update manifest's (<update.h>):
lines each ended by one '\\n', fields split by single spaces, nothing but
printable ASCII, a first line naming the format and a last line
`signature <128 hex digits>`, the Ed25519 signature (tools/jamos-sign.c)
of every byte before that line. The first lines are

    jamos-pkg 1      a package's +MANIFEST
    jamos-ports 1    the repository's INDEX
    jamos-vulns 1    the repository's VULNS

and each reader demands its own, so a signature over one format's text
never verifies as another's (the first line is signed with the rest).

This module only defines; package.py and repo.py write the formats,
pkgverify.py reads them as the PC will (user/lib/pkgfmt.c), and each
keeps to the definitions here.
"""
import hashlib
import os
import re

PKG_FIRST = "jamos-pkg 1"
INDEX_FIRST = "jamos-ports 1"
VULNS_FIRST = "jamos-vulns 1"
# jamos-sign's FORMAT word for each (its first line's first word).
SIGN_FORMAT = {PKG_FIRST: "jamos-pkg", INDEX_FIRST: "jamos-ports", VULNS_FIRST: "jamos-vulns"}

# Bytes of each signed text, its signature line included (jamos-sign.c's
# formats[] has the same).
MANIFEST_MAX = 4 << 20
INDEX_MAX = 1 << 20
VULNS_MAX = 1 << 20

FILES_MAX = 4096              # file lines in a manifest
PROGRAMS_MAX = 256            # program lines in a manifest
DEPS_MAX = 64                 # runs lines of a package; built-with lines
PATH_MAX = 255                # bytes of a path under the prefix
FILE_MAX = 64 << 20           # bytes of one file (the shell's PROGRAM_MAX)
FILES_TOTAL_MAX = 512 << 20   # bytes of a package's files together
ARCHIVE_MAX = 1 << 30         # bytes of a .jpk or a .src.tar
PACKAGES_MAX = 4096           # package lines in an index
VULNS_ENTRIES_MAX = 4096      # vuln lines in VULNS
TEXT_MAX = 200                # bytes of a summary, a licence, an extension's value
LIST_MAX = 1023               # bytes of a program's list (<wants.h> WANTS_TEXT_MAX less the NUL)
SERIAL_MAX = (1 << 63) - 1

PAGE = 4096
TABLE_MAGIC = b"jampages"     # a page-hash table's first 8 bytes
TABLE_VERSION = 1
TABLE_HEAD = 64               # bytes of its header; the page hashes follow
HASH = 32                     # bytes of a SHA-256

NAME = re.compile(r"[a-z0-9][a-z0-9+._-]{0,62}\Z")
VERSION = re.compile(r"[0-9A-Za-z][0-9A-Za-z._+~-]{0,62}\Z")
REVISION = re.compile(r"[1-9][0-9]{0,8}\Z")
SIZE = re.compile(r"(0|[1-9][0-9]{0,18})\Z")
SHA = re.compile(r"[0-9a-f]{64}\Z")
ABI = re.compile(r"[0-9a-f]{16}\Z")
MODE = ("644", "755")
SERIAL = re.compile(r"[1-9][0-9]{0,18}\Z")
MADE = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\Z")
LICENCE = re.compile(r"[A-Za-z0-9.+()-]+( [A-Za-z0-9.+()-]+)*\Z")
VULN_ID = re.compile(r"[A-Za-z][A-Za-z0-9-]{0,62}\Z")
EXT_KEY = re.compile(r"(![a-z0-9][a-z0-9-]{0,30}|[a-z0-9][a-z0-9-]{0,31})\Z")
SIG_HEX = re.compile(r"[0-9a-f]{128}\Z")
# Bytes FAT32 can't hold in a name, besides controls and the space (which
# the line format can't hold): a path is made of the rest of 0x21..0x7e.
PATH_BAD = set('"*:<>?\\|')


def printable(s, space=True):
    """Whether s is printable ASCII (0x21..0x7e, and 0x20 if space)."""
    lo = 0x20 if space else 0x21
    return all(lo <= ord(c) <= 0x7e for c in s)


def text_ok(s):
    """A summary or an extension's value: 1..TEXT_MAX printable bytes, no
    space at either end."""
    return 0 < len(s) <= TEXT_MAX and printable(s) and s == s.strip(" ")


def path_problem(p):
    """Why p is no package path (None: it is one). A path is relative to
    the prefix, 1..PATH_MAX bytes of 0x21..0x7e but PATH_BAD, its parts
    split by single '/', none empty, '.' or '..', none ending in '.'
    (FAT32 drops it), and its first part doesn't start with '+' (the
    archive's own members do)."""
    if not 0 < len(p) <= PATH_MAX:
        return "a path of %d bytes (1..%d)" % (len(p), PATH_MAX)
    if not printable(p, space=False) or set(p) & PATH_BAD:
        return "a byte a path can't hold (printable ASCII but space and %s)" % \
            "".join(sorted(PATH_BAD))
    parts = p.split("/")
    if any(x in ("", ".", "..") for x in parts):
        return "an empty, '.' or '..' part"
    if any(x.endswith(".") for x in parts):
        return "a part ending in '.' (FAT32 drops it)"
    if parts[0].startswith("+"):
        return "a first part starting with '+' (the archive's own names)"
    return None


def fold(p):
    """p with ASCII letters in lower case: FAT32's sameness of names."""
    return p.lower()


def clash_problem(paths):
    """Why a package's paths can't live side by side on FAT32 (None: they
    can): two equal but for case, or one a directory another needs."""
    files = set()
    for p in paths:
        f = fold(p)
        if f in files:
            return "%s: twice, or differs from another path only in case" % p
        files.add(f)
    for p in paths:
        parts = fold(p).split("/")
        for i in range(1, len(parts)):
            if "/".join(parts[:i]) in files:
                return "%s: a file is named where it needs a directory" % p
    return None


def vkey(v):
    """A version's order: runs of digits compare as numbers, runs of
    letters bytewise, a number before a word; other bytes only separate;
    a version that is a prefix of another's runs comes first ('1.2' before
    '1.2.1'). tools/portslib/tree.py's vkey, the same order."""
    runs = re.findall(r"[0-9]+|[A-Za-z]+", v)
    return [(0, int(x), "") if x.isdigit() else (1, 0, x) for x in runs]


def vcmp(a, b):
    """-1, 0 or 1 as version a is before, the same as, or after b."""
    ka, kb = vkey(a), vkey(b)
    return (ka > kb) - (ka < kb)


def encode_list(text):
    """A program's list (bytes) as a manifest's field: '\\n' written as the
    four bytes '\\x0a'; ValueError if it isn't a list's text (1..LIST_MAX
    bytes of lines of printable ASCII but '\\\\', each ended by '\\n')."""
    s = text.decode("ascii", "replace")
    if not 0 < len(text) <= LIST_MAX or not s.endswith("\n"):
        raise ValueError("a list of 1..%d bytes ending in a newline" % LIST_MAX)
    for line in s[:-1].split("\n"):
        if not line or not printable(line) or "\\" in line:
            raise ValueError("a list line %r: empty, or not printable ASCII without '\\'" % line)
    return s.replace("\n", "\\x0a")


def decode_list(field):
    """The manifest's list field back to bytes; ValueError if a backslash
    is anything but '\\x0a' or the result isn't a list's text."""
    out, i = [], 0
    while i < len(field):
        if field[i] == "\\":
            if field[i + 1:i + 4] != "x0a":
                raise ValueError("a backslash not starting \\x0a")
            out.append("\n")
            i += 4
        else:
            out.append(field[i])
            i += 1
    text = "".join(out).encode("ascii", "replace")
    encode_list(text)   # the same rules
    return text


def pages(size):
    return (size + PAGE - 1) // PAGE


def table_size(size):
    return TABLE_HEAD + HASH * pages(size)


def pkg_file(name, version, revision, sha):
    """A package's path in the repository (the index names it this way)."""
    return "pkg/%s-%s-r%d-%s.jpk" % (name, version, int(revision), sha[:16])


def src_file(port, version, revision, sha):
    """A port's source tarball's path in the repository."""
    return "src/%s-%s-r%d-%s.src.tar" % (port, version, int(revision), sha[:16])


ABI_FILES = ("abi/syscalls.def", "abi/idl/*.idl", "user/include/posixfds.h")


def abi_stamp(top):
    """The ABI stamp (docs/M13.7-PLAN.md section 11): the first 16 hex
    digits of the SHA-256 of abi/syscalls.def, every abi/idl/*.idl in
    byte order of their names, and user/include/posixfds.h, one after the
    other (as `cat` would give them)."""
    h = hashlib.sha256()
    idl = os.path.join(top, "abi", "idl")
    files = [os.path.join(top, "abi", "syscalls.def")]
    files += [os.path.join(idl, f) for f in sorted(os.listdir(idl)) if f.endswith(".idl")]
    files.append(os.path.join(top, "user", "include", "posixfds.h"))
    for f in files:
        with open(f, "rb") as fh:
            h.update(fh.read())
    return h.hexdigest()[:16]
