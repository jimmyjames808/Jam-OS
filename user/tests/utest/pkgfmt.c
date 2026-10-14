/* utest: the ports formats on Jam OS (<pkgfmt.h>, user/lib/pkg*.c): the
 * vectors tools/ports.py made (ports/vectors, linked in as
 * build/gen/pkgvectors.c) each get the answer its Python reference gave,
 * a package read whole and as a stream agree, mutated packages, indexes,
 * VULNS and manifests are refused unless they are the original (and keep
 * every rule when read without their signature), the journal survives
 * every cut and change, and the parts that Jam OS has twice agree with
 * their other copy: the page table with <dataprog.h>'s (/data/etc/allow.d
 * writes it), a package's refused lists with `allow`'s, and the ELF note
 * with libos's wants_read. The same checks run sanitised on the Mac in
 * `make check` (tools/pkgcheck.c); the checks are pkgcases.c, pkgfuzz.c,
 * pkgjnl.c and pkgmisc.c. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <dataprog.h>
#include <os.h>
#include <pkgfmt.h>
#include <sha256.h>
#include <wants.h>
#include "pkgcases.h"
#include "pkgcases_int.h"
#include "utest.h"

#define FUZZ_ITERATIONS 10000

bool t_pkgfmt_vectors(void)
{
    CHECK(pkgv_nfiles > 40);
    unsigned bad = pkgc_vectors();
    pkgc_work_free();
    CHECK_EQ(bad, 0);
    return true;
}

bool t_pkgfmt_misc(void)
{
    unsigned bad = pkgc_misc();
    pkgc_work_free();
    CHECK_EQ(bad, 0);
    return true;
}

bool t_pkgfmt_journal(void)
{
    unsigned bad = pkgc_journal(0x6a6e6c, FUZZ_ITERATIONS / 5);
    pkgc_work_free();
    CHECK_EQ(bad, 0);
    return true;
}

bool t_pkgfmt_fuzz(void)
{
    unsigned bad = pkgc_fuzz(0x66757a7a, FUZZ_ITERATIONS);
    pkgc_work_free();
    CHECK_EQ(bad, 0);
    return true;
}

/* A package's tables are the ones init and the shell write into
 * /data/etc/allow.d: header and page hashes, byte for byte. */
bool t_pkgfmt_table_is_allowd(void)
{
    static const size_t sizes[] = { 1, 4095, 4096, 4097, 3 * 4096 + 100 };
    uint8_t *data = malloc(4 * 4096), *mine = malloc(4096), *hashes = malloc(4 * SHA256_BYTES);
    CHECK(data && mine && hashes);
    for (unsigned i = 0; i < 4 * 4096; i++)
        data[i] = (uint8_t)(i * 7 + i / 4096);
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        struct pkg_tabler t;
        struct dp_ptable_head head;
        uint8_t digest[SHA256_BYTES];
        sha256(data, sizes[s], digest);
        dp_ptable_head(sizes[s], digest, &head);
        dp_ptable_hash(data, sizes[s], hashes);
        pkg_tabler_begin(&t, sizes[s], mine, NULL);
        CHECK(pkg_tabler_add(&t, data, sizes[s]) && pkg_tabler_end(&t));
        CHECK_EQ(pkg_table_size(sizes[s]), sizeof(head) + dp_ptable_pages(sizes[s]) * SHA256_BYTES);
        CHECK(!memcmp(mine, &head, sizeof(head)));
        CHECK(!memcmp(mine + sizeof(head), hashes, dp_ptable_pages(sizes[s]) * SHA256_BYTES));
    }
    free(data);
    free(mine);
    free(hashes);
    return true;
}

/* A list a package may not carry is one `allow` refuses (and the other
 * way round, for the words pkg_list_refused names). */
bool t_pkgfmt_lists_are_allows(void)
{
    static const char *const words[] = {
        "svc session", "svc init", "svc devmgr", "svc devmgr-ctl", "svc volumes", "svc net-sys",
        "svc dns-sys", "svc serve", "svc net listen low", "right debug", "svc net", "svc music",
        "svc net listen", "mount /data rw", "mount /data r", "right clock", "right jit",
    };
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        char list[96];
        struct wants w;
        int n = snprintf(list, sizeof(list), "posix\n%s\n", words[i]);
        CHECK(n > 0);
        CHECK_ST(wants_parse(list, (size_t)n, &w), OK);
        bool allow_refuses = dp_wants_refused(&w) != NULL;
        bool pkg_refuses = pkg_list_refused((const uint8_t *)list, (size_t)n) != NULL;
        if (allow_refuses != pkg_refuses)
            FAIL("\"%s\": allow %s it, pkg_list_refused %s it", words[i],
                 allow_refuses ? "refuses" : "takes", pkg_refuses ? "refuses" : "takes");
    }
    return true;
}

/* The vector program's ELF note is the list libos reads from it. */
bool t_pkgfmt_note_is_wants(void)
{
    const struct pkgv_file *g = pkgv_find("good.jpk");
    struct pkgc_work *k = pkgc_work();
    CHECK(g && k);
    CHECK_ST(pkg_package_verify(g->data, g->len, k->key, NULL, k->m, NULL), OK);
    uint32_t msize;
    CHECK_ST(pkg_first_header(g->data, &msize), OK);
    const uint8_t *elf = g->data + PKG_BLOCK + ((msize + 511) / 512) * 512 + PKG_BLOCK;
    uint64_t size = k->m->files[0].size;
    uint8_t list[PKG_LIST_MAX];
    size_t n;
    CHECK_ST(pkg_list_decode(k->m->programs[0].list, list, &n), OK);
    CHECK(pkg_elf_list_is(elf, size, list, n));
    struct wants w;
    CHECK_ST(wants_read(elf, size, &w), OK);
    CHECK(w.found && w.posix);
    struct wants again;
    CHECK_ST(wants_parse((const char *)list, n, &again), OK);
    CHECK(!strcmp(w.text, again.text) && w.n == again.n);
    pkgc_work_free();
    return true;
}
