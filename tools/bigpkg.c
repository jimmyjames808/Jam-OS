/* bigpkg: a POSIX program as big as Chromium's, for the test of a big
 * package (tools/pkg-test.sh: `pkg install bigpkg` from a test repository
 * onto /jam, docs/M13.7-PLAN.md "PO13 notes"). The Makefile's
 * $(BUILD)/bigpkg/bigpkg links this with bigprog's table (BIGPROG_PAGES
 * pages of constant data, page i holding the word i in each of its 512
 * places: user/tests/bigprog/table.S) by jamos-cc, with the list
 * `posix`, `mount /jam rw` and `mount /pkg r`; generated when a test asks for it, never
 * kept. The table is in the read-only segment, so a page of it is fetched
 * from the volume only when it is touched, and checked by the kernel against
 * the approval's page table then.
 *   bigpkg [hash [step]]   FNV-1a (64 bits) of the words of every step-th
 *                          page (16 if not given) and of the last, each
 *                          word checked to be its page's; prints the hash
 *   bigpkg guard           the packages' prefix is not ours to change:
 *                          /jam/pkg refuses a new directory, a new file and
 *                          a removal, and /jam beside it takes a file
 *   bigpkg view            /pkg, init's read-only view of the prefix (the list's
 *                          `mount /pkg r`): it holds this program, of the size
 *                          /jam/pkg/bin/bigpkg has, and refuses a new directory, a
 *                          new file and a removal (M13.7's PO14)
 * Exit codes: 0 done; 1 a page holds the wrong words, or the guard was
 * wrong; 64 usage. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WORDS (4096 / 8)

extern const uint64_t bigprog_table[], bigprog_table_end[];

static int usage(void)
{
    printf("usage: bigpkg [hash [step]] | guard | view\n");
    return 64;
}

/* The words of page p into *h; false if one is not p. */
static int page_hash(uint64_t p, uint64_t *h)
{
    const volatile uint64_t *w = bigprog_table + p * WORDS;
    for (unsigned i = 0; i < WORDS; i++) {
        if (w[i] != p) {
            printf("bigpkg: page %lu holds %lx at word %u\n", (unsigned long)p,
                   (unsigned long)w[i], i);
            return 0;
        }
        *h = (*h ^ w[i]) * 1099511628211ull;
    }
    return 1;
}

static int hash(unsigned long step)
{
    uint64_t pages = (uint64_t)(bigprog_table_end - bigprog_table) / WORDS;
    uint64_t h = 1469598103934665603ull, last = pages - 1, n = 0;
    for (uint64_t p = 0; p < pages; p += step, n++)
        if (!page_hash(p, &h))
            return 1;
    if ((pages - 1) % step && !page_hash(last, &h))
        return 1;
    printf("bigpkg: %lu pages, every %lu: hash %016lx\n", (unsigned long)pages, step,
           (unsigned long)h);
    return 0;
}

/* The prefix refuses what is ours to ask, the volume beside it does not. */
static int guard(void)
{
    int bad = 0;
    errno = 0;
    if (mkdir("/jam/pkg/bigpkg-evil", 0755) == 0) {
        printf("bigpkg: guard: made a directory in /jam/pkg\n");
        bad = 1;
    }
    int fd = open("/jam/pkg/bigpkg-evil.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        close(fd);
        unlink("/jam/pkg/bigpkg-evil.txt");
        printf("bigpkg: guard: made a file in /jam/pkg\n");
        bad = 1;
    }
    if (unlink("/jam/pkg/bin/bigpkg") == 0) {
        printf("bigpkg: guard: removed a package's program\n");
        bad = 1;
    }
    fd = open("/jam/bigpkg-guard.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("bigpkg: guard: /jam beside the prefix refused a file (errno %d)\n", errno);
        bad = 1;
    } else {
        close(fd);
        unlink("/jam/bigpkg-guard.txt");
    }
    if (!bad)
        printf("bigpkg: guard: /jam/pkg refused a directory, a file and a removal; "
               "/jam took a file\n");
    return bad;
}

/* /pkg is the prefix as a read-only view: the program is there, and nothing can be added to
 * it or taken from it. */
static int view(void)
{
    int bad = 0;
    struct stat a, b;
    if (stat("/pkg/bin/bigpkg", &a) || stat("/jam/pkg/bin/bigpkg", &b) || !a.st_size ||
        a.st_size != b.st_size) {
        printf("bigpkg: view: /pkg/bin/bigpkg is not the program /jam/pkg/bin/bigpkg is "
               "(errno %d)\n", errno);
        bad = 1;
    }
    if (mkdir("/pkg/bigpkg-evil", 0755) == 0) {
        printf("bigpkg: view: made a directory in /pkg\n");
        bad = 1;
    }
    int fd = open("/pkg/bigpkg-evil.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        close(fd);
        unlink("/pkg/bigpkg-evil.txt");
        printf("bigpkg: view: made a file in /pkg\n");
        bad = 1;
    }
    if (unlink("/pkg/bin/bigpkg") == 0) {
        printf("bigpkg: view: removed a package's program\n");
        bad = 1;
    }
    if (!bad)
        printf("bigpkg: view: /pkg holds the program, as big as /jam/pkg's, and refused a "
               "directory, a file and a removal\n");
    return bad;
}

int main(int argc, char **argv)
{
    if (argc == 1 || (argc >= 2 && !strcmp(argv[1], "hash") && argc <= 3)) {
        unsigned long step = 16;
        if (argc == 3) {
            step = 0;
            for (const char *s = argv[2]; *s; s++) {
                if (*s < '0' || *s > '9' || step > 1000000)
                    return usage();
                step = step * 10 + (unsigned long)(*s - '0');
            }
        }
        return step ? hash(step) : usage();
    }
    if (argc == 2 && !strcmp(argv[1], "guard"))
        return guard();
    if (argc == 2 && !strcmp(argv[1], "view"))
        return view();
    return usage();
}
