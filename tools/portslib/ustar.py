"""The one ustar form packages and source tarballs are written in
(POSIX.1-1988 ustar, as pax's "ustar" format; docs/M13.7-PLAN.md section
6): every header is a function of the member's path, mode and size
alone, so an archive's bytes depend on nothing but its files, and a
reader can rebuild each header and compare all 512 bytes.

A member is a regular file (typeflag '0'); there are no directory, link
or device entries and no pax or GNU extension headers. A header's fields:

    name      0..99    the path, or its part after the split (below)
    mode    100..107   "%07o\\0": 0000644 or 0000755
    uid     108..115   "0000000\\0"
    gid     116..123   "0000000\\0"
    size    124..135   "%011o\\0"
    mtime   136..147   "00000000000\\0" (every time 0: the epoch)
    chksum  148..155   "%06o\\0 ": the sum of the header's 512 bytes as
                       unsigned, this field counted as 8 spaces
    type    156        '0'
    linkname 157..256  all NUL
    magic   257..262   "ustar\\0"
    version 263..264   "00"
    uname   265..296   all NUL
    gname   297..328   all NUL
    devmajor 329..336  "0000000\\0"
    devminor 337..344  "0000000\\0"
    prefix  345..499   the part before the split, or all NUL
            500..511   NUL

A path of at most 100 bytes is all in name (NUL-padded, no NUL when it
is exactly 100). A longer one is split at its last '/' that leaves at
most 155 bytes before it: those go in prefix, the rest (1..100 bytes)
in name; a path no split fits is refused. The data follows its header,
NUL-padded to a multiple of 512, and the archive ends with two blocks of
512 NULs and nothing after them.
"""
import struct

BLOCK = 512
END = b"\0" * (2 * BLOCK)
NAME_MAX, PREFIX_MAX = 100, 155
MODES = (0o644, 0o755)


class UstarError(Exception):
    pass


def split(path):
    """(prefix, name) for a path, as above; UstarError if none fits."""
    raw = path.encode("ascii")
    if len(raw) <= NAME_MAX:
        return b"", raw
    at = raw.rfind(b"/", 0, PREFIX_MAX + 1)
    if at <= 0 or len(raw) - at - 1 > NAME_MAX or at == len(raw) - 1:
        raise UstarError("%s: no ustar name fits (a part after the last '/' within 155 bytes "
                         "must be 1..100 bytes)" % path)
    return raw[:at], raw[at + 1:]


def header(path, mode, size):
    """The 512-byte header of a member."""
    if mode not in MODES:
        raise UstarError("%s: mode %o (only 644 and 755)" % (path, mode))
    if not 0 <= size < 8 ** 11:
        raise UstarError("%s: size %d" % (path, size))
    prefix, name = split(path)
    h = bytearray(BLOCK)
    h[0:len(name)] = name
    h[100:108] = b"%07o\0" % mode
    h[108:116] = b"0000000\0"
    h[116:124] = b"0000000\0"
    h[124:136] = b"%011o\0" % size
    h[136:148] = b"00000000000\0"
    h[148:156] = b" " * 8
    h[156:157] = b"0"
    h[257:263] = b"ustar\0"
    h[263:265] = b"00"
    h[329:337] = b"0000000\0"
    h[337:345] = b"0000000\0"
    h[345:345 + len(prefix)] = prefix
    h[148:156] = b"%06o\0 " % sum(h)
    return bytes(h)


def member(path, mode, data):
    """A member's header, its data and the padding."""
    pad = -len(data) % BLOCK
    return header(path, mode, len(data)) + data + b"\0" * pad


def archive(members):
    """An archive of [(path, mode, data)], in the order given."""
    return b"".join(member(p, m, d) for p, m, d in members) + END


def parse_header(h):
    """(path, mode, size) of a header that is exactly the canonical one for
    them; UstarError otherwise (the reason)."""
    if len(h) != BLOCK:
        raise UstarError("a header cut short")
    if h[257:265] != b"ustar\x0000":
        raise UstarError("not a ustar header")
    name, prefix = h[0:100].split(b"\0")[0], h[345:500].split(b"\0")[0]
    raw = prefix + b"/" + name if prefix else name
    try:
        path = raw.decode("ascii")
        mode = int(h[100:107], 8)
        size = int(h[124:135], 8)
    except (UnicodeDecodeError, ValueError):
        raise UstarError("a header field isn't what this format writes")
    try:
        want = header(path, mode, size)
    except UstarError as e:
        raise UstarError("a header: %s" % e)
    if want != h:
        if struct.unpack_from("8s", h, 148)[0] != want[148:156]:
            raise UstarError("%s: its header isn't the one this format writes (the checksum, "
                             "or another field)" % path)
        raise UstarError("%s: its header isn't the one this format writes" % path)
    return path, mode, size


def members(data):
    """Every member of an archive as (path, mode, bytes), each header
    checked as parse_header does, the padding NUL, the two end blocks
    there and nothing after them."""
    out, at = [], 0
    while True:
        if at + BLOCK > len(data):
            raise UstarError("the archive ends without its two end blocks")
        if data[at:at + BLOCK] == b"\0" * BLOCK:
            if data[at:] != END:
                raise UstarError("bytes after the first end block that aren't one more "
                                 "end block and the end of the file")
            return out
        path, mode, size = parse_header(data[at:at + BLOCK])
        at += BLOCK
        pad = -size % BLOCK
        if at + size + pad > len(data):
            raise UstarError("%s: the archive ends inside it" % path)
        if data[at + size:at + size + pad] != b"\0" * pad:
            raise UstarError("%s: its padding isn't NUL" % path)
        out.append((path, mode, data[at:at + size]))
        at += size + pad
