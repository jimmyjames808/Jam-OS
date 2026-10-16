/* Programs on /data as Jam OS checks them before they run: the owner's
 * approvals (`allow`'s lines), each approval's page table, the program's
 * list (<wants.h>) read in pieces, and what no program from /data may be
 * given. Shared by the two that seal such a program for running, the shell
 * (sh_allow.c, sh_dataprog.c: `allow`, and a program typed at it) and init
 * (exec.c: /svc/exec, for a program another program starts); neither
 * trusts the file's bytes, only the approval.
 *
 * The approvals are lines of /data/etc/allow, "<sha-256>\t<path>\t<what
 * its list asks for>", written by the shell's `allow` and, for a
 * package's programs, by init (a line tagged with its package:
 * dp_allow_tag); a path's last line is its approval. Every program's /data is a view that leaves etc
 * alone (<fsview.h>), so only init and the shell, which hold the whole of
 * /data, can change the file or the tables beside it: a program can never
 * approve another, or itself.
 *
 * The page table is /data/etc/allow.d/<sha-256>: a struct dp_ptable_head,
 * then the SHA-256 of each PAGE_SIZE page of the file in order (the last
 * page zero-padded to a whole page, which is how the kernel hashes it:
 * vmo_make_exec_verified). The kernel is handed the hashes alone, as a VMO
 * of VMO_VERIFIED_HASH_BYTES a page from offset 0 (dp_table_read). A table
 * is as trusted as the allow line naming its hash.
 *
 * The pure parts (the table's format and checks, the list's reader, the
 * line's fields, the refusals) make no system calls: utest checks them
 * (shimage.c). dp_allow_hash, dp_table_read and dp_wants_vmo do I/O, and
 * are safe from any thread, as is dp_job_create.
 *
 * The job a started program runs in (dp_job_create, with its own message
 * budget) is here too: the shell, /svc/exec and init's apps, the three
 * that start programs, make it by the same rule, whether the program is on
 * /data or in the boot image. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <os.h>
#include <sha256.h>
#include <wants.h>

#define DP_ALLOW_FILE "/data/etc/allow"
#define DP_ALLOW_DIR  "/data/etc"
#define DP_TABLE_DIR  "/data/etc/allow.d"
#define DP_HEX_LEN    (2 * SHA256_BYTES)   /* an approval's hash as hex, without its NUL */
#define DP_PROGRAM_MAX VMO_VERIFIED_MAX    /* the biggest program (1 GiB): spawn's, the kernel's */
#define DP_ALLOW_MAX  (1u << 20)           /* the most bytes of the approvals read */

/* ---- the page table ------------------------------------------------------------------ */

#define DP_PTABLE_MAGIC   "JAMPAGES"   /* 8 bytes, no NUL */
#define DP_PTABLE_VERSION 1u

/* The table file's first 64 bytes (x86-64's byte order: the stick is
 * read only by Jam OS). */
struct dp_ptable_head {
    char     magic[8];                /* DP_PTABLE_MAGIC */
    uint32_t version;                 /* DP_PTABLE_VERSION */
    uint32_t page_size;               /* bytes each hash covers: PAGE_SIZE */
    uint64_t size;                    /* the program file's bytes */
    uint64_t pages;                   /* hashes after the header: size in pages, rounded up */
    uint8_t  sha256[SHA256_BYTES];    /* the whole file's: its allow line's hash */
};
_Static_assert(sizeof(struct dp_ptable_head) == 64, "the table's header is 64 bytes");

/* Pages of a file of size bytes (each a hash in its table). */
uint64_t dp_ptable_pages(uint64_t size);
/* The header for a file of size bytes whose SHA-256 is sha. */
void     dp_ptable_head(uint64_t size, const uint8_t sha[SHA256_BYTES],
                        struct dp_ptable_head *out);
/* Is h, the start of a table file of file_bytes bytes, the table of a
 * program of size bytes whose approval's hash is sha? OK; ERR_OUT_OF_RANGE:
 * a well-formed table of a file of another size (the program changed since
 * it was allowed); ERR_INVALID_ARGS: not a table, another hash's, or its
 * length is wrong (broken). */
status_t dp_ptable_check(const struct dp_ptable_head *h, uint64_t file_bytes, uint64_t size,
                         const uint8_t sha[SHA256_BYTES]);
/* The page hashes of bytes[0, size) into out (dp_ptable_pages(size)
 * hashes of SHA256_BYTES each). */
void     dp_ptable_hash(const uint8_t *bytes, uint64_t size, uint8_t *out);
/* 64 hex digits (an allow line's hash) as bytes; false if they aren't. */
bool     dp_hex_bytes(const char *hex, uint8_t out[SHA256_BYTES]);

/* ---- the list ------------------------------------------------------------------------ */

/* Read len bytes of the program at off into dst (off and len inside it:
 * the caller checked); a status of the reader's own otherwise (a verified
 * VMO's page that doesn't match its approval: ERR_IO). */
typedef status_t (*dp_read_fn)(void *ctx, uint64_t off, void *dst, uint64_t len);

/* Notes (PT_NOTE segments) read at most, all of them together: a list is
 * at most WANTS_TEXT_MAX bytes and its note's header. */
#define DP_NOTES_MAX (64u << 10)

/* The list of the program of size bytes that read gives, into *w, by
 * wants_read's rules: only the ELF header, the program headers and the
 * PT_NOTE segments are read, wherever they are in the file (a list in a
 * 200 MB program is a few reads). ERR_INVALID_ARGS: not an ELF file the
 * loader takes, a broken list, or notes past DP_NOTES_MAX; a reader's
 * error as it is; ERR_NO_MEMORY. */
status_t dp_image_wants(dp_read_fn read, void *ctx, uint64_t size, struct wants *w);
/* The part of the program a list is read from, gathered as dp_image_wants
 * does it: a small ELF file of the same headers (*len bytes in a new heap
 * block, *out: the caller frees it) holding the ELF header, the program
 * headers and the PT_NOTE segments, and nothing else, so wants_read and
 * pkg_elf_list_is (<pkgfmt.h>) can read the list of a program that is
 * never in memory whole. Errors as dp_image_wants'. */
status_t dp_image_small(dp_read_fn read, void *ctx, uint64_t size, uint8_t **out, uint64_t *len);
/* The same from x, a verified VMO of size bytes, read with vmo_read only
 * (never mapped: a page that fails its hash is ERR_IO here, where a mapping
 * would kill the reader). ERR_IO: a page isn't the approved one (the file
 * changed since it was allowed), or /data can't read it. */
status_t dp_wants_vmo(handle_t x, uint64_t size, struct wants *w);

/* ---- approvals ----------------------------------------------------------------------- */

/* abs is a file path on /data ("/data/x", not "/data" itself). */
bool        dp_on_data(const char *abs);
/* One line of the approvals, its fields cut in place (hash, path, the
 * list as shown). false: not an approval (a comment, or malformed). */
bool        dp_allow_split(char *line, char **hash, char **path, char **text);
/* A package's line (init's approvals, abi/idl/pkgctl.idl) ends in a fourth
 * field, its tag: "<list>\tpkg=<name> <version>-r<revision>". Of
 * dp_allow_split's text: the tag cut off in place, returned without its
 * "pkg=" (NULL for a line without one: the owner's `allow`). */
char       *dp_allow_tag(char *text);
/* The hash of path's approval (its last line in DP_ALLOW_FILE) into hex
 * (DP_HEX_LEN digits and a NUL). ERR_NOT_FOUND: no line for it (or no
 * approvals at all); ERR_OUT_OF_RANGE: the file is past DP_ALLOW_MAX; a
 * read's error. */
status_t    dp_allow_hash(const char *path, char hex[DP_HEX_LEN + 1]);
/* The hashes of approval hash_hex's table for a program file of size
 * bytes, into a new VMO as vmo_make_exec_verified takes them (*table,
 * ours). ERR_NOT_FOUND: no table (an approval from before tables);
 * ERR_OUT_OF_RANGE: a table of a file of another size (it changed), or a
 * file bigger than any program; ERR_INVALID_ARGS: a broken table, or
 * hash_hex isn't one; a read's error. */
status_t    dp_table_read(const char *hash_hex, uint64_t size, handle_t *table);
/* The first thing w asks for that no program from /data may have, or NULL
 * (dataprog.c says why each): "right debug", or a service's name. */
const char *dp_wants_refused(const struct wants *w);
/* The root resource's rights for a list's WANT_RIGHT_* (w->rights): what
 * the program's copy gets. Never RIGHT_TRANSFER: it can narrow its copy
 * (RIGHT_DUPLICATE), never pass it on. */
rights_t    dp_root_rights(uint32_t want);

/* ---- a program's job ------------------------------------------------------------------ */

/* The message bytes a started program's job may have waiting unread
 * (JOB_LIMIT_MSG_BYTES), its own and every job's under it: what it wrote
 * that nobody has read is charged to it and to every job up to the root,
 * whose budget is all the system's (1/8 of memory, at most 64 MiB:
 * kernel/proc/userboot.c). Without a limit of its own, a program writing
 * faster than a service reads could fill the root's, and every other
 * writer in the system would get ERR_NO_MEMORY; with one, the flood stops
 * at its own job. 16 MiB: every queue a program normally writes to holds
 * at most 1 MiB (a pipe, a socketpair: Chromium's Mojo channels, the
 * compositor's connection: CHANNEL_BYTE_BOUND), so this is 16 of them full
 * at once, where Chromium's processes together have kilobytes in flight
 * (its big transfers go in shared memory); and a quarter of the root's. */
#define DP_JOB_MSG_BYTES (16u << 20)

/* A new job under parent for a program to run in, with that budget: what
 * the shell's run and its helpers, /svc/exec's children and init's
 * desktop apps start programs in (init's test runner, boot/init.cfg's
 * programs, gives none). The program's own SR_JOB has no RIGHT_MANAGE, so
 * it can't lift it; whoever holds the job with RIGHT_MANAGE (a /svc/exec
 * caller its child's) may set another. *out ours on OK. */
status_t    dp_job_create(handle_t parent, handle_t *out);
