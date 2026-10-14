/* bigprog: a program as big as Chromium's, for the test of programs that
 * size on /data (tools/bigprog-test.sh; docs/CR-PLAN.md section 6, track
 * CP1). Its test builds it with a table of 76,800 pages (300 MiB:
 * $(BUILD)/bigprog/bigprog), copies it to /data and allows it there; the
 * boot image's bin/bigprog has 16, for a quick look.
 *
 * The table (table.S) is constant data in its read-only segment, which
 * the loader maps straight from the program's VMO: a page of it is
 * fetched from /data only when it is touched, and checked by the kernel
 * against `allow`'s hash of it then. Page i holds the 64-bit word i in
 * each of its 512 places, so a page in the wrong place shows too.
 *   bigprog lazy           touch none of the table
 *   bigprog check [step]   read every step-th page of the table (64 if not
 *                          given) and its last, every word of each
 * It says what it did; a page the kernel can't give it (changed since it
 * was allowed) ends it with the kernel's line instead.
 * Exit codes: 0 done; 1 a page holds the wrong words; 64 usage. */
#include <os.h>

#define WORDS (PAGE_SIZE / 8)

extern const uint64_t bigprog_table[], bigprog_table_end[];

static int usage(void)
{
    printf("usage: bigprog lazy | check [step]\n");
    return 64;
}

/* Every word of page p of the table: p, or false. */
static bool page_ok(uint64_t p)
{
    const volatile uint64_t *w = bigprog_table + p * WORDS;
    for (unsigned i = 0; i < WORDS; i++)
        if (w[i] != p) {
            printf("bigprog: page %lu holds %lx at word %u\n", (unsigned long)p,
                   (unsigned long)w[i], i);
            return false;
        }
    return true;
}

int main(int argc, char **argv)
{
    uint64_t pages = (uint64_t)(bigprog_table_end - bigprog_table) / WORDS;
    if (argc == 2 && !strcmp(argv[1], "lazy")) {
        printf("bigprog: lazy: a table of %lu pages, none touched\n", (unsigned long)pages);
        return 0;
    }
    if ((argc == 2 || argc == 3) && !strcmp(argv[1], "check")) {
        uint64_t step = argc == 3 ? 0 : 64;
        for (const char *s = argc == 3 ? argv[2] : ""; *s; s++) {
            if (*s < '0' || *s > '9' || step > pages)
                return usage();
            step = step * 10 + (uint64_t)(*s - '0');
        }
        if (!step)
            return usage();
        uint64_t n = 0, t0 = now();
        for (uint64_t p = 0; p < pages; p += step, n++)
            if (!page_ok(p))
                return 1;
        if ((pages - 1) % step) {
            if (!page_ok(pages - 1))
                return 1;
            n++;
        }
        printf("bigprog: checked %lu of %lu pages in %lu ms\n", (unsigned long)n,
               (unsigned long)pages, (unsigned long)((now() - t0) / NS_PER_MS));
        return 0;
    }
    return usage();
}
