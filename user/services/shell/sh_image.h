/* A program on /data as the shell checks it (sh_image.c): the page table
 * `allow` writes beside each approval, and the program's list (<wants.h>)
 * read in pieces through a reader, never from a mapping of the whole file.
 *
 * The page table is /data/etc/allow.d/<sha-256>: a struct sh_ptable_head,
 * then the SHA-256 of each PAGE_SIZE page of the file in order (the last
 * page zero-padded to a whole page, which is how the kernel hashes it:
 * vmo_make_exec_verified). The kernel is handed the hashes alone, as a VMO
 * of VMO_VERIFIED_HASH_BYTES a page from offset 0. /data/etc is outside
 * every program's view, so a table is as trusted as the allow line naming
 * its hash.
 *
 * No state and no system calls: the shell does the I/O (sh_allow.c,
 * sh_dataprog.c), utest checks these (shimage.c). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <os.h>
#include <sha256.h>
#include <wants.h>

#define SH_PTABLE_MAGIC   "JAMPAGES"   /* 8 bytes, no NUL */
#define SH_PTABLE_VERSION 1u

/* The table file's first 64 bytes (x86-64's byte order: the stick is
 * read only by Jam OS). */
struct sh_ptable_head {
    char     magic[8];                /* SH_PTABLE_MAGIC */
    uint32_t version;                 /* SH_PTABLE_VERSION */
    uint32_t page_size;               /* bytes each hash covers: PAGE_SIZE */
    uint64_t size;                    /* the program file's bytes */
    uint64_t pages;                   /* hashes after the header: size in pages, rounded up */
    uint8_t  sha256[SHA256_BYTES];    /* the whole file's: its allow line's hash */
};
_Static_assert(sizeof(struct sh_ptable_head) == 64, "the table's header is 64 bytes");

/* Pages of a file of size bytes (each a hash in its table). */
uint64_t sh_ptable_pages(uint64_t size);
/* The header for a file of size bytes whose SHA-256 is sha. */
void     sh_ptable_head(uint64_t size, const uint8_t sha[SHA256_BYTES],
                        struct sh_ptable_head *out);
/* Is h, the start of a table file of file_bytes bytes, the table of a
 * program of size bytes whose approval's hash is sha? OK; ERR_OUT_OF_RANGE:
 * a well-formed table of a file of another size (the program changed since
 * it was allowed); ERR_INVALID_ARGS: not a table, another hash's, or its
 * length is wrong (broken). */
status_t sh_ptable_check(const struct sh_ptable_head *h, uint64_t file_bytes, uint64_t size,
                         const uint8_t sha[SHA256_BYTES]);
/* The page hashes of bytes[0, size) into out (sh_ptable_pages(size)
 * hashes of SHA256_BYTES each). */
void     sh_ptable_hash(const uint8_t *bytes, uint64_t size, uint8_t *out);
/* 64 hex digits (an allow line's hash) as bytes; false if they aren't. */
bool     sh_hex_bytes(const char *hex, uint8_t out[SHA256_BYTES]);

/* Read len bytes of the program at off into dst (off and len inside it:
 * the caller checked); a status of the reader's own otherwise (a pager
 * VMO's page that doesn't match its approval: ERR_IO). */
typedef status_t (*sh_image_read_fn)(void *ctx, uint64_t off, void *dst, uint64_t len);

/* Notes (PT_NOTE segments) read at most, all of them together: a list is
 * at most WANTS_TEXT_MAX bytes and its note's header. */
#define SH_IMAGE_NOTES_MAX (64u << 10)

/* The list of the program of size bytes that read gives, into *w, by
 * wants_read's rules: only the ELF header, the program headers and the
 * PT_NOTE segments are read, wherever they are in the file (a list in a
 * 200 MB program is a few reads). ERR_INVALID_ARGS: not an ELF file the
 * loader takes, a broken list, or notes past SH_IMAGE_NOTES_MAX; a
 * reader's error as it is; ERR_NO_MEMORY. */
status_t sh_image_wants(sh_image_read_fn read, void *ctx, uint64_t size, struct wants *w);
