"""A program's page-hash table (docs/M12.5-PLAN.md, "PK4 notes"; the
package's +TABLES/<path>, docs/M13.7-PLAN.md section 6): what the kernel
checks each page of an approved program against when the program runs
from the pager (vmo_make_exec_verified, syscall 59).

The table is a 64-byte header and then the SHA-256 of each 4 KiB page of
the file, page 0's first, the last page zero-padded to 4096 bytes before
it is hashed (what fat supplies past the end of the file, and so what
the kernel hashes). All numbers little-endian:

    0   8  magic "jampages"
    8   4  version: 1
    12  4  the page size: 4096
    16  8  the file's size in bytes (1..64 MiB)
    24  8  pages: (size + 4095) / 4096
    32 32  the SHA-256 of the whole file
    64     pages x 32 bytes: each page's SHA-256

so a table is 64 + 32 x pages bytes. The kernel takes the hashes alone
(from byte 64 on), in a VMO of their own. The SHA-256 is FIPS 180-4's,
the same function as user/lib/sha256.c and kernel/lib/sha256.c: the
self-test compares this module's tables with ones those two files make
on the Mac (tools/pagetable.c).
"""
import hashlib
import struct

from . import pkgformat as fmt

HEAD = struct.Struct("<8sIIQQ32s")
assert HEAD.size == fmt.TABLE_HEAD


def make(data):
    """The table of a file's bytes."""
    if not 0 < len(data) <= fmt.FILE_MAX:
        raise ValueError("a table is for a file of 1..%d bytes, not %d" % (fmt.FILE_MAX, len(data)))
    n = fmt.pages(len(data))
    out = [HEAD.pack(fmt.TABLE_MAGIC, fmt.TABLE_VERSION, fmt.PAGE, len(data), n,
                     hashlib.sha256(data).digest())]
    for i in range(n):
        page = data[i * fmt.PAGE:(i + 1) * fmt.PAGE]
        out.append(hashlib.sha256(page + b"\0" * (fmt.PAGE - len(page))).digest())
    return b"".join(out)


def parse_head(table):
    """(size, pages, file sha256 bytes) from a table's header; ValueError
    if the header or the table's length is wrong."""
    if len(table) < fmt.TABLE_HEAD:
        raise ValueError("a table shorter than its header")
    magic, version, page, size, n, digest = HEAD.unpack_from(table)
    if magic != fmt.TABLE_MAGIC or version != fmt.TABLE_VERSION or page != fmt.PAGE:
        raise ValueError("not a version-1 table of 4 KiB pages")
    if not 0 < size <= fmt.FILE_MAX or n != fmt.pages(size):
        raise ValueError("a table's size or page count is wrong")
    if len(table) != fmt.table_size(size):
        raise ValueError("a table of %d bytes for %d pages" % (len(table), n))
    return size, n, digest
