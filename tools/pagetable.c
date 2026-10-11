/* pagetable: a program's page-hash table as the PC computes it, on the
 * Mac. The table's format is tools/portslib/pagetable.py's (and
 * /data/etc/allow.d's): a 64-byte header (the magic "jampages", version 1,
 * the page size 4096, the file's size and page count, its SHA-256; all
 * little-endian), then the SHA-256 of each 4 KiB page, the last one
 * zero-padded to 4096 bytes. Built twice by the Makefile, with the
 * SHA-256 of libos (user/lib/sha256.c: build/host/pagetable-libos) and of
 * the kernel (kernel/lib/sha256.c: build/host/pagetable-kernel), which the
 * kernel checks each page of an approved program with; `ports.py
 * selftest` compares their tables with the packager's on the same files,
 * so a package's tables are the ones the PC will compute.
 *
 *   pagetable FILE    the table, on stdout; exit 0, or 1 with a message
 *
 * The two SHA-256 files are built as they are, against a header that
 * stands in for <os.h> or <jam/string.h> (build/host/shim/: memcpy and
 * the fixed-width types are all they take from it). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PAGETABLE_KERNEL
#include <jam/sha256.h>
#else
#include <sha256.h>
#endif

#define PAGE      4096u
#define FILE_MAX  (64u << 20)   /* the shell's PROGRAM_MAX: the largest program */
#define HEAD      64u

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* The file's bytes (1..FILE_MAX of them) in a malloc'd buffer, or NULL. */
static uint8_t *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    uint8_t *buf = malloc(FILE_MAX + 1);
    size_t n = buf ? fread(buf, 1, FILE_MAX + 1, f) : 0;
    fclose(f);
    if (!buf || n == 0 || n > FILE_MAX) {
        fprintf(stderr, "pagetable: %s: not a file of 1..%u bytes\n", path, FILE_MAX);
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

int main(int argc, char **argv)
{
    size_t len;
    if (argc != 2) {
        fprintf(stderr, "usage: pagetable FILE\n");
        return 2;
    }
    uint8_t *data = read_all(argv[1], &len);
    if (!data)
        return 1;
    uint64_t pages = (len + PAGE - 1) / PAGE;
    uint8_t head[HEAD] = { 0 }, page[PAGE], digest[SHA256_BYTES];
    memcpy(head, "jampages", 8);
    put32(head + 8, 1);
    put32(head + 12, PAGE);
    put64(head + 16, len);
    put64(head + 24, pages);
    sha256(data, len, head + 32);
    fwrite(head, 1, HEAD, stdout);
    for (uint64_t i = 0; i < pages; i++) {
        size_t at = (size_t)i * PAGE, n = len - at < PAGE ? len - at : PAGE;
        memset(page, 0, sizeof(page));
        memcpy(page, data + at, n);
        sha256(page, PAGE, digest);
        fwrite(digest, 1, SHA256_BYTES, stdout);
    }
    free(data);
    return fflush(stdout) ? 1 : 0;
}
