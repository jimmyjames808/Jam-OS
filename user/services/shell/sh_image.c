/* A program on /data as the shell checks it: the approval's page table and
 * the program's list read in pieces (sh_image.h says what each is).
 *
 * The list is read without the whole file: the ELF header, the program
 * headers and each PT_NOTE segment are read through the caller's reader
 * and put together as a small ELF file of their own (the header, the
 * program headers with each note's offset moved to where its bytes now
 * are, then the notes), which wants_read parses by its usual rules. So a
 * list is accepted or refused exactly as wants_read would over the whole
 * file, and the shell reads a verified program's list with vmo_read, a few
 * pages, wherever the linker put it. */
#include <sha256.h>
#include <wants.h>
#include "sh_image.h"

#define PT_NOTE   4
#define MAX_PHDRS 64   /* wants_read's and the loader's limit */

/* The ELF64 header and program header fields read here (wants.c and
 * spawn.c read the same layout). */
struct ehdr {
    uint8_t  ident[16];                  /* "\x7fELF", class 2: 64-bit */
    uint16_t type, machine;              /* (checked by the loader) */
    uint32_t version;                    /* (not checked) */
    uint64_t entry, phoff, shoff;        /* entry; program / section header offsets */
    uint32_t flags;                      /* (none on x86-64) */
    uint16_t ehsize, phentsize, phnum;   /* this header's size; a program header's; how many */
    uint16_t shentsize, shnum, shstrndx; /* section headers (not used) */
};

struct phdr {
    uint32_t type, flags;                /* PT_*; PF_* */
    uint64_t offset, vaddr, paddr;       /* in the file; in memory (not used) */
    uint64_t filesz, memsz;              /* bytes in the file; in memory (not used) */
    uint64_t align;                      /* (not used) */
};

_Static_assert(sizeof(struct ehdr) == 64, "ELF64 header");
_Static_assert(sizeof(struct phdr) == 56, "ELF64 program header");

/* ---- the page table ------------------------------------------------------------------ */

uint64_t sh_ptable_pages(uint64_t size)
{
    return size / PAGE_SIZE + (size % PAGE_SIZE ? 1 : 0);
}

void sh_ptable_head(uint64_t size, const uint8_t sha[SHA256_BYTES], struct sh_ptable_head *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->magic, SH_PTABLE_MAGIC, sizeof(out->magic));
    out->version = SH_PTABLE_VERSION;
    out->page_size = (uint32_t)PAGE_SIZE;
    out->size = size;
    out->pages = sh_ptable_pages(size);
    memcpy(out->sha256, sha, SHA256_BYTES);
}

status_t sh_ptable_check(const struct sh_ptable_head *h, uint64_t file_bytes, uint64_t size,
                         const uint8_t sha[SHA256_BYTES])
{
    if (memcmp(h->magic, SH_PTABLE_MAGIC, sizeof(h->magic)) || h->version != SH_PTABLE_VERSION ||
        h->page_size != PAGE_SIZE || memcmp(h->sha256, sha, SHA256_BYTES))
        return ERR_INVALID_ARGS;
    /* Its own numbers agree with each other and with the file's length:
     * only then is it a table at all, of whatever size. */
    uint64_t hashes, total;
    if (h->pages != sh_ptable_pages(h->size) ||
        __builtin_mul_overflow(h->pages, (uint64_t)SHA256_BYTES, &hashes) ||
        __builtin_add_overflow(hashes, (uint64_t)sizeof(*h), &total) || total != file_bytes)
        return ERR_INVALID_ARGS;
    return h->size == size ? OK : ERR_OUT_OF_RANGE;
}

void sh_ptable_hash(const uint8_t *bytes, uint64_t size, uint8_t *out)
{
    uint64_t whole = size / PAGE_SIZE;
    for (uint64_t i = 0; i < whole; i++)
        sha256(bytes + i * PAGE_SIZE, PAGE_SIZE, out + i * SHA256_BYTES);
    if (size % PAGE_SIZE) {
        static uint8_t last[PAGE_SIZE];   /* the shell runs one command at a time */
        memset(last, 0, sizeof(last));
        memcpy(last, bytes + whole * PAGE_SIZE, size % PAGE_SIZE);
        sha256(last, sizeof(last), out + whole * SHA256_BYTES);
    }
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;   /* sha256_hex writes lower case only */
}

bool sh_hex_bytes(const char *hex, uint8_t out[SHA256_BYTES])
{
    for (unsigned i = 0; i < SHA256_BYTES; i++) {
        int hi = hex_digit(hex[2 * i]), lo = hi < 0 ? -1 : hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return hex[2 * SHA256_BYTES] == '\0';
}

/* ---- the list ------------------------------------------------------------------------ */

/* The ELF header and program headers as wants_read checks them. */
static bool headers_ok(const struct ehdr *eh, uint64_t size)
{
    return !memcmp(eh->ident, "\x7f" "ELF", 4) && eh->ident[4] == 2 &&
           eh->phentsize == sizeof(struct phdr) && eh->phnum <= MAX_PHDRS &&
           eh->phoff <= size && eh->phnum * sizeof(struct phdr) <= size - eh->phoff;
}

/* The bytes the small ELF file needs for the notes of ph[0, n): false if
 * a note reaches past the file or they come to more than the limit. */
static bool notes_fit(const struct phdr *ph, unsigned n, uint64_t size, uint64_t *bytes)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < n; i++) {
        if (ph[i].type != PT_NOTE)
            continue;
        if (ph[i].offset > size || ph[i].filesz > size - ph[i].offset ||
            ph[i].filesz > SH_IMAGE_NOTES_MAX - sum)
            return false;
        sum += ph[i].filesz;
    }
    *bytes = sum;
    return true;
}

status_t sh_image_wants(sh_image_read_fn read, void *ctx, uint64_t size, struct wants *w)
{
    memset(w, 0, sizeof(*w));
    struct ehdr eh;
    struct phdr ph[MAX_PHDRS];
    if (size < sizeof(eh))
        return ERR_INVALID_ARGS;
    status_t st = read(ctx, 0, &eh, sizeof(eh));
    if (st == OK && !headers_ok(&eh, size))
        st = ERR_INVALID_ARGS;
    if (st == OK && eh.phnum)
        st = read(ctx, eh.phoff, ph, eh.phnum * sizeof(struct phdr));
    uint64_t notes = 0;
    if (st == OK && !notes_fit(ph, eh.phnum, size, &notes))
        st = ERR_INVALID_ARGS;
    if (st != OK)
        return st;
    /* The small file: the header, the program headers right after it,
     * then each note where its program header now says. */
    uint64_t at = sizeof(eh) + eh.phnum * sizeof(struct phdr);
    uint8_t *small = malloc(at + notes);
    if (!small)
        return ERR_NO_MEMORY;
    for (unsigned i = 0; i < eh.phnum && st == OK; i++) {
        if (ph[i].type != PT_NOTE)
            continue;
        if (ph[i].filesz)
            st = read(ctx, ph[i].offset, small + at, ph[i].filesz);
        ph[i].offset = at;
        at += ph[i].filesz;
    }
    if (st == OK) {
        eh.phoff = sizeof(eh);
        memcpy(small, &eh, sizeof(eh));
        memcpy(small + sizeof(eh), ph, eh.phnum * sizeof(struct phdr));
        st = wants_read(small, at, w);
    }
    free(small);
    return st;
}
