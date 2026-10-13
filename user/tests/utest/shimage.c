/* utest: how a program on /data is checked, by the shell and init
 * (/svc/exec) alike (libos's user/lib/dataprog.c, <dataprog.h>).
 *
 * t_sh_ptable: the page table `allow` writes beside an approval: each
 * page's SHA-256 (the last page zero-padded, as the kernel hashes it), the
 * header and every way a table is refused (another size: the program
 * changed; another hash, a wrong length, a bad magic: broken), and an
 * allow line's hash read back as bytes.
 * t_sh_image_wants: a program's list read in pieces through a reader is
 * the list wants_read finds in the whole file, for the boot image's
 * programs with lists (and one without), while only the headers and the
 * notes are read; a list in notes past the limit, a note past the file's
 * end and a reader's error (a page that failed its hash: ERR_IO) are
 * refused. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <dataprog.h>
#include <os.h>
#include <sha256.h>
#include <wants.h>
#include "utest.h"

/* ---- the page table ------------------------------------------------------------------ */

bool t_sh_ptable(void)
{
    CHECK_EQ(dp_ptable_pages(0), 0);
    CHECK_EQ(dp_ptable_pages(1), 1);
    CHECK_EQ(dp_ptable_pages(PAGE_SIZE), 1);
    CHECK_EQ(dp_ptable_pages(PAGE_SIZE + 1), 2);
    /* Three pages and a hundred bytes of a pattern: each whole page's hash
     * is its own, the last is of its bytes and zeros up to a page. */
    uint64_t size = 3 * PAGE_SIZE + 100;
    uint8_t *bytes = malloc(4 * PAGE_SIZE), *hashes = malloc(4 * SHA256_BYTES);
    CHECK(bytes && hashes);
    for (uint64_t i = 0; i < 4 * PAGE_SIZE; i++)
        bytes[i] = (uint8_t)(i * 7 + i / PAGE_SIZE);
    dp_ptable_hash(bytes, size, hashes);
    uint8_t want[SHA256_BYTES];
    for (unsigned p = 0; p < 3; p++) {
        sha256(bytes + p * PAGE_SIZE, PAGE_SIZE, want);
        CHECK(!memcmp(hashes + p * SHA256_BYTES, want, SHA256_BYTES));
    }
    memset(bytes + size, 0, 4 * PAGE_SIZE - size);
    sha256(bytes + 3 * PAGE_SIZE, PAGE_SIZE, want);
    CHECK(!memcmp(hashes + 3 * SHA256_BYTES, want, SHA256_BYTES));
    /* The header, and what is refused. */
    uint8_t sha[SHA256_BYTES], other[SHA256_BYTES];
    sha256(bytes, size, sha);
    memcpy(other, sha, sizeof(other));
    other[5] ^= 1;
    struct dp_ptable_head h, bad;
    dp_ptable_head(size, sha, &h);
    uint64_t file = sizeof(h) + 4 * SHA256_BYTES;
    CHECK_EQ(h.pages, 4);
    CHECK_ST(dp_ptable_check(&h, file, size, sha), OK);
    CHECK_ST(dp_ptable_check(&h, file, size + 1, sha), ERR_OUT_OF_RANGE);   /* it changed */
    CHECK_ST(dp_ptable_check(&h, file, size, other), ERR_INVALID_ARGS);     /* another's */
    CHECK_ST(dp_ptable_check(&h, file - 1, size, sha), ERR_INVALID_ARGS);
    CHECK_ST(dp_ptable_check(&h, file + SHA256_BYTES, size, sha), ERR_INVALID_ARGS);
    bad = h;
    bad.magic[0] = 'X';
    CHECK_ST(dp_ptable_check(&bad, file, size, sha), ERR_INVALID_ARGS);
    bad = h;
    bad.version = DP_PTABLE_VERSION + 1;
    CHECK_ST(dp_ptable_check(&bad, file, size, sha), ERR_INVALID_ARGS);
    bad = h;
    bad.page_size = 2 * PAGE_SIZE;
    CHECK_ST(dp_ptable_check(&bad, file, size, sha), ERR_INVALID_ARGS);
    bad = h;
    bad.pages = 5;   /* more hashes than its own size has pages */
    CHECK_ST(dp_ptable_check(&bad, file + SHA256_BYTES, size, sha), ERR_INVALID_ARGS);
    bad = h;
    bad.size = UINT64_MAX;   /* its numbers overflow: never a table */
    bad.pages = dp_ptable_pages(UINT64_MAX);
    CHECK_ST(dp_ptable_check(&bad, file, size, sha), ERR_INVALID_ARGS);
    /* An allow line's hash, back to bytes. */
    char hex[2 * SHA256_BYTES + 1];
    uint8_t back[SHA256_BYTES];
    sha256_hex(sha, hex);
    CHECK(dp_hex_bytes(hex, back) && !memcmp(back, sha, SHA256_BYTES));
    hex[3] = 'A';   /* sha256_hex writes lower case only */
    CHECK(!dp_hex_bytes(hex, back));
    sha256_hex(sha, hex);
    hex[63] = '\0';
    CHECK(!dp_hex_bytes(hex, back));
    free(bytes);
    free(hashes);
    return true;
}

/* ---- the list ------------------------------------------------------------------------ */

/* A reader over bytes in memory that counts what it was asked for, and
 * fails every read from `fail_at` on with ERR_IO (a page that doesn't
 * match its approval). */
struct mem {
    const uint8_t *bytes;
    uint64_t       size;
    uint64_t       read;      /* bytes handed out */
    uint64_t       fail_at;   /* UINT64_MAX: never */
};

static status_t mem_read(void *ctx, uint64_t off, void *dst, uint64_t len)
{
    struct mem *m = ctx;
    if (off > m->size || len > m->size - off)
        return ERR_INTERNAL;   /* dp_image_wants asked outside the file */
    if (off + len > m->fail_at)
        return ERR_IO;
    memcpy(dst, m->bytes + off, len);
    m->read += len;
    return OK;
}

static bool same_wants(const struct wants *a, const struct wants *b)
{
    CHECK_EQ(a->found, b->found);
    CHECK_EQ(a->posix, b->posix);
    CHECK_EQ(a->n, b->n);
    CHECK_EQ(a->rights, b->rights);
    for (unsigned i = 0; i < a->n; i++)
        CHECK(!strcmp(a->grant[i], b->grant[i]));
    CHECK(!strcmp(a->text, b->text));
    return true;
}

/* One program of the boot image: its list the same both ways, and only a
 * small part of the file read. */
static bool one_program(const char *name)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    CHECK_ST(bootfs_default(&fs), OK);
    CHECK_ST(bootfs_lookup(fs, name, &data, &size), OK);
    static struct wants whole, pieces;
    CHECK_ST(wants_read(data, size, &whole), OK);
    struct mem m = { data, size, 0, UINT64_MAX };
    CHECK_ST(dp_image_wants(mem_read, &m, size, &pieces), OK);
    if (!same_wants(&whole, &pieces))
        return false;
    CHECK(m.read <= 64 + 64 * 56 + DP_NOTES_MAX && m.read < size);
    /* A page failing in the middle of it: the reader's error, no list. */
    m = (struct mem){ data, size, 0, 64 };
    CHECK_ST(dp_image_wants(mem_read, &m, size, &pieces), ERR_IO);
    CHECK(!pieces.found && !pieces.n);
    return true;
}

/* A small ELF file: the header, one PT_NOTE program header, `len` bytes
 * at `off` claimed for the note. */
static void small_elf(uint8_t *f, uint64_t off, uint64_t len)
{
    memset(f, 0, 64 + 56);
    memcpy(f, "\x7f" "ELF\x02", 5);
    uint16_t phentsize = 56, phnum = 1;
    uint64_t phoff = 64, note[3] = { off, 0, 0 };
    uint32_t type = 4;
    memcpy(f + 0x20, &phoff, 8);
    memcpy(f + 0x36, &phentsize, 2);
    memcpy(f + 0x38, &phnum, 2);
    memcpy(f + 64, &type, 4);
    memcpy(f + 64 + 8, note, 8);         /* p_offset */
    memcpy(f + 64 + 32, &len, 8);        /* p_filesz */
}

bool t_sh_image_wants(void)
{
    const struct bootfs_view *fs;
    if (bootfs_default(&fs) != OK) {
        printf("utest: %s: no boot image (SR_BOOTFS): skipped\n", utest_cur);
        return true;
    }
    if (!one_program("bin/wantlisten") || !one_program("bin/pagertest") ||
        !one_program("bin/utest") || !one_program("bin/pxtest"))
        return false;
    /* A list's notes past the limit, a note past the end of the file. */
    uint64_t size = DP_NOTES_MAX + 2 * PAGE_SIZE;
    uint8_t *f = malloc(size);
    CHECK(f);
    memset(f, 0, size);
    static struct wants w;
    small_elf(f, PAGE_SIZE, DP_NOTES_MAX + 1);
    struct mem m = { f, size, 0, UINT64_MAX };
    CHECK_ST(dp_image_wants(mem_read, &m, size, &w), ERR_INVALID_ARGS);
    small_elf(f, PAGE_SIZE, size);
    CHECK_ST(dp_image_wants(mem_read, &m, size, &w), ERR_INVALID_ARGS);
    /* An empty note at the limit's edge is no list: OK, none found. */
    small_elf(f, size, 0);
    CHECK_ST(dp_image_wants(mem_read, &m, size, &w), OK);
    CHECK(!w.found);
    /* Not an ELF file; shorter than its header. */
    f[1] = 'X';
    CHECK_ST(dp_image_wants(mem_read, &m, size, &w), ERR_INVALID_ARGS);
    CHECK_ST(dp_image_wants(mem_read, &m, 63, &w), ERR_INVALID_ARGS);
    free(f);
    return true;
}
