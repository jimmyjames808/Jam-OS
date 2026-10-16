/* The ports formats on Jam OS: strict readers of what the Mac's
 * tools/ports.py writes and what `pkg` and init must trust before they
 * act on it (docs/M13.7-PLAN.md sections 6, 7, 9 and 13; the grammars in
 * full are the plan's "PO3 notes", the Mac's reference reader is
 * tools/portslib/pkgverify.py, and ports/vectors/ holds the bytes both
 * readers are held to). Five formats:
 *
 *   package   a `.jpk`, a ustar archive: +MANIFEST, the files, then each
 *             program's page-hash table (+TABLES/<path>)
 *   manifest  `jamos-pkg 1`: the package's signed description
 *   index     `jamos-ports 1`: the repository's signed catalogue, with a
 *             serial that only grows
 *   vulns     `jamos-vulns 1`: the repository's signed advisories
 *   journal   `jamos-pkg-journal 1`: the install in progress, not signed
 *             (only `pkg` and init can write /data/pkg/db), but checked
 *             whole by its own SHA-256 so a write cut short is refused
 *
 * The bytes come from the network (through bin/pkg) or from a stick, so
 * every reader here follows one order: the size bound, the first line,
 * the bytes (printable ASCII and '\n'), the signature line's shape, **the
 * signature**, and only then any field. It reads only within its length,
 * writes nothing outside the structure it fills, refuses everything the
 * format does not allow (it never skips, repairs or guesses; what it
 * skips is only a well-formed extension line), and calls no allocator or
 * system call: user/lib/pkg*.c are built for the Mac too
 * (tools/pkgcheck.c, `make check`) and held to PO3's vectors.
 *
 * Text readers keep pointers into the text they were given
 * (struct pkg_str): the text must outlive the structure. The big
 * structures (a manifest of 4,096 files is about 270 KB, an index about
 * 800 KB) go on the heap, never the stack, and are valid only after a
 * reader returns OK. Their reasons (`why`, may be NULL) are static
 * strings.
 *
 * Status: OK; ERR_INVALID_ARGS (not this format: a grammar rule broken);
 * ERR_OUT_OF_RANGE (the text is over or under its size bound);
 * ERR_NOT_SUPPORTED (a must-understand extension line this reader does
 * not know, or another version of the format's first line); ERR_ACCESS_DENIED
 * (no signature, the signature is not the key's, or the bytes are not
 * the ones a signed hash names, or a program asks for what a package
 * may not); ERR_BAD_STATE (a replay: a serial below the last accepted,
 * or the same serial with other bytes); ERR_BUFFER_TOO_SMALL. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/status.h>
#include <sha256.h>

/* ---- the formats' limits (tools/portslib/pkgformat.py has the same) ------------------- */

#define PKG_MANIFEST_MAX     (4u << 20)    /* bytes of a manifest, its signature line included */
#define PKG_INDEX_MAX        (1u << 20)
#define PKG_VULNS_MAX        (1u << 20)
#define PKG_JOURNAL_MAX      (2u << 20)    /* 4,096 busy lines of 255-byte paths */
#define PKG_FILES_MAX        4096u         /* file lines of a manifest */
#define PKG_PROGRAMS_MAX     256u
#define PKG_DEPS_MAX         64u           /* runs lines of a package; built-with lines */
#define PKG_PATH_MAX         255u          /* bytes of a path under the prefix */
#define PKG_FILE_MAX         (1ull << 30)  /* bytes of one file: VMO_VERIFIED_MAX, the biggest program */
#define PKG_FILES_TOTAL_MAX  (2ull << 30)  /* bytes of a package's files together */
#define PKG_ARCHIVE_MAX      (4ull << 30)  /* bytes of a .jpk or a .src.tar */
#define PKG_PACKAGES_MAX     4096u         /* package lines of an index */
#define PKG_VULNS_ENTRIES_MAX 4096u
#define PKG_BUSY_MAX         PKG_FILES_MAX /* busy lines of a journal */
#define PKG_TEXT_MAX         200u          /* bytes of a summary, a licence, an extension's value */
#define PKG_LIST_MAX         1023u         /* bytes of a program's list, decoded */
#define PKG_SIG_BYTES        64u
#define PKG_KEY_BYTES        32u
#define PKG_BLOCK            512u          /* ustar */
#define PKG_END_BYTES        (2u * PKG_BLOCK)
#define PKG_PAGE             4096u         /* a page-hash table's page */
#define PKG_TABLE_HEAD       64u

/* The four signed or checked texts' first lines. */
#define PKG_FIRST_PACKAGE "jamos-pkg 1"
#define PKG_FIRST_INDEX   "jamos-ports 1"
#define PKG_FIRST_VULNS   "jamos-vulns 1"
#define PKG_FIRST_JOURNAL "jamos-pkg-journal 1"

/* A piece of the text being read (not NUL-terminated). */
struct pkg_str {
    const char *p;
    uint32_t    n;
};

/* ---- common ----------------------------------------------------------------------------- */

bool pkg_str_eq(struct pkg_str a, const char *s);
/* Bytewise, a shorter string first when it starts the other. */
int  pkg_str_cmp(struct pkg_str a, struct pkg_str b);
/* Hex digits (lower case) to bytes: false if s isn't exactly 2n of them. */
bool pkg_hex(struct pkg_str s, uint8_t *out, size_t n);

/* The version order: a version is the list of its maximal runs of ASCII
 * digits and of ASCII letters (other bytes only separate); two lists
 * compare item by item, a digit run with a digit run as numbers (leading
 * zeros ignored), a letter run with a letter run bytewise, a digit run
 * before a letter run; when one list starts the other the shorter is
 * first. -1, 0 or 1 as a is before, the same as, or after b. */
int  pkg_version_cmp(struct pkg_str a, struct pkg_str b);
/* Is p a path under the prefix: 1..PKG_PATH_MAX bytes of 0x21..0x7e but
 * " * : < > ? \ and |, parts split by single '/', none empty, '.' or
 * '..' or ending in '.' (FAT32 drops it), the first not starting with
 * '+' (the archive's own names). NULL if it is; else why not. */
const char *pkg_path_problem(struct pkg_str p);
/* Are a and b the same name on FAT32 (equal but for ASCII case)? */
bool pkg_path_fold_equal(struct pkg_str a, struct pkg_str b);
/* The decoded form of a program's list field (a manifest's `program`
 * line): "\x0a" is '\n', any other backslash is refused. Into out
 * (PKG_LIST_MAX bytes), *n its length. ERR_INVALID_ARGS if the field or
 * the result is not a list (1..PKG_LIST_MAX bytes of lines of 0x20..0x7e
 * but '\', each ended by '\n'). */
status_t pkg_list_decode(struct pkg_str field, uint8_t out[PKG_LIST_MAX], size_t *n);
/* What a package's program may not ask for, from a decoded list's text:
 * the words `allow` refuses a program on /data (dp_wants_refused in
 * <dataprog.h>: utest checks the two agree), and a list with no `posix`
 * line. NULL if the list passes; else why not. init's own check of the
 * program's ELF note is the final word. */
const char *pkg_list_refused(const uint8_t *list, size_t n);
/* Is the one JamOS type-1 note of the ELF file elf (size bytes) exactly
 * the list (n bytes, without the note's NUL)? Every offset checked. */
bool pkg_elf_list_is(const uint8_t *elf, uint64_t size, const uint8_t *list, size_t n);

/* ---- the manifest ------------------------------------------------------------------------ */

struct pkg_dep {                 /* a `runs` line */
    struct pkg_str name;
    struct pkg_str least;        /* the least version; n == 0: any */
};

struct pkg_built {               /* a `built-with` line */
    struct pkg_str name;
    struct pkg_str version;
    uint32_t       revision;     /* 0: none given */
};

struct pkg_file {                /* a `file` line */
    struct pkg_str path;
    uint16_t       mode;         /* 0644 or 0755 */
    uint16_t       program;      /* its index in programs[], or PKG_NO_PROGRAM */
    uint64_t       size;
    uint8_t        sha256[SHA256_BYTES];
};
#define PKG_NO_PROGRAM 0xffffu

struct pkg_program {             /* a `program` line (and its `table` line) */
    struct pkg_str path;
    struct pkg_str list;         /* the encoded list field: pkg_list_decode */
    uint32_t       file;         /* its index in files[] */
    uint64_t       table_size;   /* the table line's size, and */
    uint8_t        table_sha256[SHA256_BYTES];   /* the SHA-256 of +TABLES/<path> */
};

struct pkg_manifest {
    struct pkg_str  name, version, licence, summary;
    uint32_t        revision;
    char            abi[17];     /* 16 hex digits */
    uint8_t         sha256[SHA256_BYTES];   /* of the whole text */
    uint32_t        nruns, nbuilt, nfiles, nprograms;
    uint64_t        total;       /* bytes of all the files */
    struct pkg_dep   runs[PKG_DEPS_MAX];
    struct pkg_built built_with[PKG_DEPS_MAX];
    struct pkg_file  files[PKG_FILES_MAX];
    struct pkg_program programs[PKG_PROGRAMS_MAX];
};

/* The text (a package's +MANIFEST) of len bytes read into *m: the size
 * bound, the first line "jamos-pkg 1", the bytes, the signature line,
 * the Ed25519 signature of every byte before that line by key (the ports
 * key, /boot/ports.pub: update_key_parse reads it), then every line
 * against the grammar of the plan's section 6 in its fixed place, every
 * count and size bounded, the paths FAT32 can hold side by side, each
 * program a non-empty file of mode 755 (and every such file a program),
 * each list decoded and passed by pkg_list_refused. Extension lines are
 * skipped; a must-understand one is ERR_NOT_SUPPORTED. */
status_t pkg_manifest_parse(const void *text, size_t len, const uint8_t key[PKG_KEY_BYTES],
                            struct pkg_manifest *m, const char **why);
/* The same without the signature check (the signature line must still
 * have its shape): only for tests and the host check, which feed it text
 * nobody signed. Never for text from outside. */
status_t pkg_manifest_parse_unchecked(const void *text, size_t len, struct pkg_manifest *m,
                                      const char **why);

/* ---- the archive ------------------------------------------------------------------------- */

/* Bytes of the page-hash tables of all a manifest's programs together. */
uint64_t pkg_tables_bytes(const struct pkg_manifest *m);
/* A page-hash table's size for a file of size bytes: 64 + 32 per page. */
uint64_t pkg_table_size(uint64_t size);
/* The ustar header of a member, the one form the packager writes: a
 * function of (path, mode, size) alone. ERR_INVALID_ARGS: mode not 0644
 * or 0755, a path no ustar name fits, or a size over 8^11. */
status_t pkg_header_make(struct pkg_str path, unsigned mode, uint64_t size,
                         uint8_t out[PKG_BLOCK]);
/* The archive's first header, which must be exactly the canonical one for
 * +MANIFEST (mode 0644) of 1..PKG_MANIFEST_MAX bytes: that size. */
status_t pkg_first_header(const uint8_t h[PKG_BLOCK], uint32_t *manifest_size);

/* A program's page-hash table made, or checked, as bytes arrive (the
 * format is /data/etc/allow.d's, <dataprog.h>: a 64-byte header, the
 * whole file's SHA-256, then the SHA-256 of each 4 KiB page, the last
 * zero-padded). Begin with exactly one of out (the table is written
 * there: pkg_table_size(size) bytes) and expect (the table is compared
 * with those bytes instead). */
struct pkg_tabler {
    struct sha256 whole, page;
    uint64_t      size, seen;
    uint64_t      pages_done;
    uint32_t      fill;          /* bytes in the page being hashed */
    uint8_t      *out;
    const uint8_t *expect;
    bool          ok;            /* compare mode: everything so far equal */
};
void pkg_tabler_begin(struct pkg_tabler *t, uint64_t size, uint8_t *out, const uint8_t *expect);
/* More of the file; false if that is more than its size. */
bool pkg_tabler_add(struct pkg_tabler *t, const void *data, size_t n);
/* The file is whole: true if exactly size bytes came and, in compare
 * mode, every byte of the table matched. */
bool pkg_tabler_end(struct pkg_tabler *t);

/* A whole package in memory (len bytes at data), checked: the first
 * header, the manifest as pkg_manifest_parse reads it (key), the ABI
 * stamp (abi, 16 hex digits; NULL: not checked), then every member
 * exactly the manifest's, in its order, header byte for byte, each
 * file's SHA-256 its line's, each table's SHA-256 its line's and the
 * table the program's own, each program's ELF note its list, the padding
 * NUL, the two end blocks and nothing after them. *m is filled as for the
 * manifest (and valid only on OK). For packages that fit in memory: the
 * streaming reader below is for the rest. */
status_t pkg_package_verify(const void *data, size_t len, const uint8_t key[PKG_KEY_BYTES],
                            const char *abi, struct pkg_manifest *m, const char **why);
/* Whether the manifest's abi is the stamp (16 hex digits). */
bool pkg_abi_is(const struct pkg_manifest *m, const char *abi);

/* The same checks as bytes arrive, for a package too big for memory. The
 * caller reads the archive in order: the first header
 * (pkg_first_header), the manifest's bytes and padding (it parses them
 * itself with pkg_manifest_parse), then pkg_reader_init and, per member
 * of the manifest in order (each file, then each program's table):
 *
 *     pkg_reader_next   the member's 512-byte header; says its size
 *     pkg_reader_data   its bytes, in pieces of any size
 *     pkg_reader_done   its padding (to a multiple of 512 bytes)
 *
 * and last pkg_reader_finish with every byte left. Nothing is believed
 * until pkg_reader_done returns OK for the member, and the package is
 * whole only when pkg_reader_finish does. The programs' ELF notes are
 * not checked here (the bytes are not kept): whoever installs the
 * program does (pkg_elf_list_is, or init's wants_read). tables is the
 * reader's memory for the programs' page hashes: pkg_tables_bytes(m)
 * bytes, which stay valid and hold each program's table once its file is
 * done (for init's comparison with its own table). */
struct pkg_reader {
    const struct pkg_manifest *m;
    uint8_t       *tables;
    uint32_t       next;         /* the next member's index: files, then tables */
    bool           open;         /* a member's bytes are being read */
    uint64_t       size, got;    /* of the open member */
    uint64_t       table_at;     /* where its table is in `tables` */
    struct sha256  sum;
    struct pkg_tabler tab;
};

/* What pkg_reader_next learnt of a member. */
struct pkg_member {
    uint32_t index;              /* its file's index, or the program's index for a table */
    bool     table;              /* +TABLES/<path> of programs[index] */
    uint64_t size;
    uint32_t padding;            /* NUL bytes after it: to a multiple of 512 */
};

status_t pkg_reader_init(struct pkg_reader *r, const struct pkg_manifest *m, uint8_t *tables,
                         size_t tables_cap);
/* ERR_INVALID_ARGS: the header is not the canonical one for the next
 * member (another path, mode, size or any other byte); ERR_BAD_STATE: a
 * member is open, or all are read. */
status_t pkg_reader_next(struct pkg_reader *r, const uint8_t h[PKG_BLOCK], struct pkg_member *out);
/* ERR_OUT_OF_RANGE: more than the member's size; ERR_ACCESS_DENIED: a
 * table's bytes are not the program's own table (the first byte that
 * differs). */
status_t pkg_reader_data(struct pkg_reader *r, const void *data, size_t n);
/* ERR_INVALID_ARGS: the member is not whole, or its padding is wrong;
 * ERR_ACCESS_DENIED: its SHA-256 is not the manifest's. */
status_t pkg_reader_done(struct pkg_reader *r, const void *padding, size_t n);
/* tail: all the bytes after the last member, n of them (give it at most
 * PKG_END_BYTES + 1: a longer tail is refused all the same). OK only for
 * exactly the two end blocks. */
status_t pkg_reader_finish(struct pkg_reader *r, const void *tail, size_t n);

/* ---- the index --------------------------------------------------------------------------- */

/* The last index (or VULNS) accepted, which `pkg` keeps in /data/pkg/db:
 * its serial and the SHA-256 of its text (all zero: unknown). */
struct pkg_last {
    uint64_t serial;
    uint8_t  sha256[SHA256_BYTES];
};

struct pkg_ipkg {                /* a `package` line */
    struct pkg_str name, version, port;
    uint32_t       revision;
    uint32_t       nruns;
    uint32_t       runs_at;      /* the first `runs` line's offset in the text */
    uint32_t       source;       /* its `src` line's index in sources[] */
    uint64_t       size;
    uint8_t        sha256[SHA256_BYTES];
};

struct pkg_isrc {                /* a `src` line */
    struct pkg_str port, version;
    uint32_t       revision;
    bool           used;
    uint64_t       size;
    uint8_t        sha256[SHA256_BYTES];
};

struct pkg_index {
    uint64_t        serial;
    char            made[21];    /* YYYY-MM-DDTHH:MM:SSZ */
    char            abi[17];
    uint8_t         sha256[SHA256_BYTES];   /* of the text */
    const char     *text;
    size_t          len;
    uint32_t        npackages, nsources;
    struct pkg_ipkg packages[PKG_PACKAGES_MAX];   /* by name, strictly increasing */
    struct pkg_isrc sources[PKG_PACKAGES_MAX];    /* by port, strictly increasing */
};

/* As pkg_manifest_parse, for INDEX ("jamos-ports 1"), plus the rules
 * across lines (every `runs` names a package of the index at the least
 * version it asks, every package has its `src` line and every `src` line
 * is some package's) and the replay rule: with last, a serial below
 * last->serial is ERR_BAD_STATE, and the same serial only for the same
 * text. */
status_t pkg_index_parse(const void *text, size_t len, const uint8_t key[PKG_KEY_BYTES],
                         const struct pkg_last *last, struct pkg_index *x, const char **why);
status_t pkg_index_parse_unchecked(const void *text, size_t len, struct pkg_index *x,
                                   const char **why);
/* The package called name, or NULL. */
const struct pkg_ipkg *pkg_index_find(const struct pkg_index *x, struct pkg_str name);
/* Its `runs` lines into out; the count. */
unsigned pkg_index_runs(const struct pkg_index *x, const struct pkg_ipkg *p,
                        struct pkg_dep out[PKG_DEPS_MAX]);
/* Are these bytes (size and SHA-256, hashed by the caller) the package p
 * the index names? OK, or ERR_ACCESS_DENIED. */
status_t pkg_index_check(const struct pkg_ipkg *p, uint64_t size,
                         const uint8_t sha256[SHA256_BYTES]);
/* Where the repository keeps a package (kind 'p': "pkg/<name>-<version>-
 * r<revision>-<16 hex digits of sha256>.jpk") or a port's source (kind
 * 's': "src/<port>-<version>-r<revision>-<16 hex>.src.tar"), into out of
 * cap bytes, NUL-terminated. ERR_BUFFER_TOO_SMALL if it does not fit.
 * The path is derived, never read from the repository. */
status_t pkg_repo_path(char kind, struct pkg_str name, struct pkg_str version, uint32_t revision,
                       const uint8_t sha256[SHA256_BYTES], char *out, size_t cap);

/* ---- VULNS -------------------------------------------------------------------------------- */

struct pkg_vuln {                /* a `vuln` line */
    struct pkg_str component;
    struct pkg_str first;        /* n == 0: from the start ('-') */
    struct pkg_str fixed;        /* n == 0: no fix yet ('-') */
    uint32_t       fixrev;       /* 0: none ('-') */
    struct pkg_str id, summary;
};

struct pkg_vulns {
    uint64_t        serial;
    char            made[21];
    uint8_t         sha256[SHA256_BYTES];
    uint32_t        n;
    struct pkg_vuln v[PKG_VULNS_ENTRIES_MAX];   /* by component, then id, strictly increasing */
};

status_t pkg_vulns_parse(const void *text, size_t len, const uint8_t key[PKG_KEY_BYTES],
                         const struct pkg_last *last, struct pkg_vulns *v, const char **why);
status_t pkg_vulns_parse_unchecked(const void *text, size_t len, struct pkg_vulns *v,
                                   const char **why);
/* Is a component at version (and revision; 0 when it has none) inside
 * the entry's range: (first is '-' or version >= first) and (fixed is
 * '-', or version < fixed, or version == fixed and fixrev is above
 * revision). */
bool pkg_vuln_affected(const struct pkg_vuln *e, struct pkg_str version, uint32_t revision);
/* `pkg audit` for one installed package: the entries that name the
 * package itself or one of its built-with components at an affected
 * version, in VULNS's order, into out (at most max). The count (it
 * counts past max). */
unsigned pkg_audit(const struct pkg_vulns *v, const struct pkg_manifest *m,
                   const struct pkg_vuln **out, unsigned max);

/* ---- the journal -------------------------------------------------------------------------- */

/* The transaction in progress, /data/pkg/db/journal (plan section 9):
 *
 *     jamos-pkg-journal 1
 *     op install|upgrade|remove
 *     name <name>
 *     version <version>            install, upgrade: the new package's
 *     revision <revision>          (its manifest is db/<name>.new)
 *     manifest <sha256>            ... the SHA-256 of that manifest's text
 *     old <version> <revision> <sha256>   upgrade, remove: the installed
 *                                  package's (db/<name>)
 *     step begin|staged|approved|swapped|revoked|removed|recorded
 *     busy <path>                  upgrade: a file left at its .new name
 *                                  because a program runs from it; sorted
 *                                  strictly, at most PKG_BUSY_MAX
 *     end <sha256 of every byte before this line>
 *
 * Lines in exactly this order (the lines an op does not have are left
 * out), each ended by one '\n'. The steps an op goes through, in order:
 *   install  begin staged approved swapped recorded
 *   upgrade  begin staged approved swapped revoked recorded
 *   remove   begin revoked removed recorded
 * and the journal is removed after `recorded`. Whoever reads a journal
 * trusts nothing it names: every file it points at is checked against its
 * signed manifest. */
enum pkg_op { PKG_OP_INSTALL, PKG_OP_UPGRADE, PKG_OP_REMOVE, PKG_OPS };
enum pkg_step {
    PKG_STEP_BEGIN, PKG_STEP_STAGED, PKG_STEP_APPROVED, PKG_STEP_SWAPPED, PKG_STEP_REVOKED,
    PKG_STEP_REMOVED, PKG_STEP_RECORDED, PKG_STEPS
};

struct pkg_journal {
    enum pkg_op    op;
    enum pkg_step  step;
    struct pkg_str name;
    struct pkg_str version;      /* the new version (install, upgrade) */
    uint32_t       revision;
    uint8_t        manifest[SHA256_BYTES];
    struct pkg_str old_version;  /* upgrade, remove */
    uint32_t       old_revision;
    uint8_t        old_manifest[SHA256_BYTES];
    uint32_t       nbusy;
    struct pkg_str busy[PKG_BUSY_MAX];
};

/* Read a journal of len bytes: its SHA-256 line first (a torn write is
 * ERR_INVALID_ARGS), then the grammar. */
status_t pkg_journal_parse(const void *text, size_t len, struct pkg_journal *j, const char **why);
/* Write j's canonical text into out of cap bytes; the length, or 0 if it
 * does not fit or j breaks the grammar (the same rules as the reader's:
 * parse what you write). */
size_t   pkg_journal_write(const struct pkg_journal *j, char *out, size_t cap);
/* The step after step in op's sequence; PKG_STEPS after the last (the
 * journal is removed), and also for a step op does not have. */
enum pkg_step pkg_journal_next(enum pkg_op op, enum pkg_step step);
const char *pkg_op_name(enum pkg_op op);
const char *pkg_step_name(enum pkg_step step);
