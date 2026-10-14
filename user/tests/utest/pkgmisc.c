/* The small parts of the ports formats (<pkgfmt.h>), each against what
 * its rule says: the version order, the path grammar, the program lists,
 * the ustar header (any one changed bit of the first header is refused),
 * the page-hash table (against SHA-256 taken by hand, in pieces of any
 * size, in both modes), the ELF note finder (cut anywhere), the
 * repository's names, the index's lookups and the audit's range rule. */
#include <os.h>
#include <pkgfmt.h>
#include "pkgcases.h"
#include "pkgcases_int.h"

static unsigned bad;

#define EXPECT(c, ...)                                \
    do {                                              \
        if (!(c)) {                                   \
            printf("pkgfmt: " __VA_ARGS__);           \
            printf("\n");                             \
            bad++;                                    \
        }                                             \
    } while (0)

#define STR(s) ((struct pkg_str){ (s), (uint32_t)strlen(s) })

static void versions(void)
{
    static const struct { const char *a, *b; int want; } t[] = {
        { "1.2", "1.2.1", -1 },   { "1.10", "1.9", 1 },    { "1.0", "1_0", 0 },
        { "1.0a", "1.0", 1 },     { "2", "a", -1 },        { "01", "1", 0 },
        { "1.2.3", "1.2.3.0", -1 }, { "9.1.0", "9.1.0", 0 }, { "1.0~rc1", "1.0", 1 },
        { "abc", "abd", -1 },     { "1a", "1b", -1 },      { "10", "9", 1 },
        { "1.0-1", "1.0-2", -1 }, { "0", "00", 0 },        { "A", "a", -1 },
        { "1.0.0", "1.0", 1 },    { "a1", "a2", -1 },      { "99999999999999999999", "1", 1 },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        EXPECT(pkgc_version_cmp(t[i].a, t[i].b) == t[i].want, "version %s against %s", t[i].a, t[i].b);
        EXPECT(pkgc_version_cmp(t[i].b, t[i].a) == -t[i].want, "version %s against %s (reversed)",
               t[i].b, t[i].a);
    }
}

static void paths(void)
{
    static const char *const good[] = { "bin/vim", "a", "share/vim/vim91/defaults.vim", "a.b/c",
                                        "x+y", "a-b_c", "bin/+x", "a/b.c/d" };
    static const char *const refused[] = {
        "", "/abs", "a//b", "a/./b", "a/../b", "..", ".", "a/", "a/b.", "a.", "+MANIFEST", "+x/y",
        "a b", "a\\b", "a:b", "a*b", "a?b", "a<b", "a>b", "a|b", "a\"b", "a\tb", "caf\xc3\xa9",
        "a\177b",
    };
    char longest[300];
    for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++)
        EXPECT(pkgc_path_ok(good[i]), "path %s refused", good[i]);
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
        EXPECT(!pkgc_path_ok(refused[i]), "path %s taken", refused[i]);
    memset(longest, 'a', 255);
    longest[255] = '\0';
    EXPECT(pkgc_path_ok(longest), "a path of 255 bytes refused");
    longest[255] = 'a';
    longest[256] = '\0';
    EXPECT(!pkgc_path_ok(longest), "a path of 256 bytes taken");
    EXPECT(pkg_path_fold_equal(STR("Bin/Vim"), STR("bin/vim")) &&
               !pkg_path_fold_equal(STR("bin/vim"), STR("bin/vi")),
           "FAT32's name sameness");
}

static void lists(void)
{
    uint8_t out[PKG_LIST_MAX];
    size_t n;
    static const char *const bad_fields[] = {
        "", "posix", "\\x0a", "posix\\x0a\\x0a", "posix\\x0b", "pos\\ix\\x0a", "posix\\x0", "\\\\x0a",
        "\\x0aposix\\x0a",
    };
    EXPECT(pkg_list_decode(STR("posix\\x0amount /data r\\x0a"), out, &n) == OK &&
               n == 20 && !memcmp(out, "posix\nmount /data r\n", 20), "a good list decoded");
    for (unsigned i = 0; i < sizeof(bad_fields) / sizeof(bad_fields[0]); i++)
        EXPECT(pkg_list_decode(STR(bad_fields[i]), out, &n) != OK, "list field %u taken", i);
    EXPECT(!pkg_list_refused((const uint8_t *)"posix\n", 6), "posix alone refused");
    EXPECT(!pkg_list_refused((const uint8_t *)"posix\nmount /data rw\nsvc net\n", 28),
           "an ordinary list refused");
    static const char *const refused[] = {
        "posix\nsvc session\n", "posix\nsvc init\n", "posix\nsvc devmgr\n", "posix\nsvc devmgr-ctl\n",
        "posix\nsvc volumes\n", "posix\nsvc net-sys\n", "posix\nsvc dns-sys\n", "posix\nsvc serve\n",
        "posix\nsvc net listen low\n", "posix\nright debug\n", "mount /data r\n", "svc net\n",
    };
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
        EXPECT(pkg_list_refused((const uint8_t *)refused[i], strlen(refused[i])) != NULL,
               "list %u (%s) passed", i, refused[i]);
}

static void headers(void)
{
    uint8_t h[PKG_BLOCK];
    char path[400];
    const struct pkgv_file *g = pkgv_find("good.jpk");
    uint32_t msize;
    EXPECT(g && pkg_first_header(g->data, &msize) == OK, "good.jpk's first header refused");
    if (!g || pkg_first_header(g->data, &msize) != OK)
        return;
    EXPECT(pkg_header_make(STR("+MANIFEST"), 0644, msize, h) == OK && !memcmp(h, g->data, PKG_BLOCK),
           "the header made is not good.jpk's");
    for (unsigned bit = 0; bit < 8 * PKG_BLOCK; bit++) {
        memcpy(h, g->data, PKG_BLOCK);
        h[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        if (pkg_first_header(h, &msize) == OK) {
            EXPECT(false, "the first header with bit %u changed was accepted", bit);
            break;
        }
    }
    EXPECT(pkg_header_make(STR("a"), 0600, 1, h) != OK, "mode 0600");
    EXPECT(pkg_header_make(STR("a"), 0644, 1ull << 33, h) != OK, "a size over 8^11");
    EXPECT(pkg_header_make(STR("a"), 0644, (1ull << 33) - 1, h) == OK, "the largest size");
    EXPECT(pkg_header_make(STR(""), 0644, 1, h) != OK, "an empty path");
    EXPECT(pkg_header_make(STR("a b"), 0644, 1, h) != OK, "a path with a space");
    memset(path, 'a', 101);
    path[101] = '\0';
    EXPECT(pkg_header_make(STR(path), 0644, 1, h) != OK, "101 bytes with no slash");
    memset(path, 'd', 150);
    path[150] = '/';
    memset(path + 151, 'f', 100);
    path[251] = '\0';   /* a 150-byte directory and a 100-byte name: splits */
    EXPECT(pkg_header_make(STR(path), 0644, 1, h) == OK && h[345] == 'd' && h[0] == 'f' &&
               h[345 + 150] == 0, "a long path was not split at its slash");
    memset(path + 151, 'f', 101);
    path[252] = '\0';   /* a 101-byte name: cannot */
    EXPECT(pkg_header_make(STR(path), 0644, 1, h) != OK, "a 101-byte name after the split");
}

/* A table made by hand from the format's description. */
static void table_by_hand(const uint8_t *data, size_t size, uint8_t *out)
{
    size_t pages = (size + PKG_PAGE - 1) / PKG_PAGE;
    memset(out, 0, PKG_TABLE_HEAD);
    memcpy(out, "JAMPAGES", 8);
    out[8] = 1;
    out[13] = 0x10;   /* 4096, little-endian */
    for (unsigned i = 0; i < 8; i++) {
        out[16 + i] = (uint8_t)((uint64_t)size >> (8 * i));
        out[24 + i] = (uint8_t)((uint64_t)pages >> (8 * i));
    }
    sha256(data, size, out + 32);
    for (size_t p = 0; p < pages; p++) {
        uint8_t page[PKG_PAGE];
        size_t take = size - p * PKG_PAGE < PKG_PAGE ? size - p * PKG_PAGE : PKG_PAGE;
        memset(page, 0, sizeof(page));
        memcpy(page, data + p * PKG_PAGE, take);
        sha256(page, sizeof(page), out + PKG_TABLE_HEAD + 32 * p);
    }
}

static void tables(void)
{
    static const size_t sizes[] = { 1, 100, 4095, 4096, 4097, 8192, 12289 };
    uint64_t rng = 5;
    uint8_t *data = malloc(13000), *want = malloc(13000), *got = malloc(13000);
    if (!data || !want || !got) {
        EXPECT(false, "no memory");
        return;
    }
    for (unsigned i = 0; i < 13000; i++)
        data[i] = (uint8_t)pkgc_rand(&rng);
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        size_t size = sizes[s], tsize = (size_t)pkg_table_size(size);
        struct pkg_tabler t;
        table_by_hand(data, size, want);
        pkg_tabler_begin(&t, size, got, NULL);
        for (size_t at = 0; at < size;) {
            size_t take = 1 + (size_t)(pkgc_rand(&rng) % 3000);
            take = take < size - at ? take : size - at;
            EXPECT(pkg_tabler_add(&t, data + at, take), "size %zu: bytes refused", size);
            at += take;
        }
        EXPECT(pkg_tabler_end(&t) && !memcmp(got, want, tsize), "size %zu: the table is not the "
               "one made by hand", size);
        EXPECT(pkg_table_size(size) == PKG_TABLE_HEAD + 32 * ((size + 4095) / 4096), "table size");
        pkg_tabler_begin(&t, size, NULL, want);   /* compare mode: same bytes pass */
        EXPECT(pkg_tabler_add(&t, data, size) && pkg_tabler_end(&t), "size %zu: compare mode", size);
        want[tsize - 1] ^= 1;
        pkg_tabler_begin(&t, size, NULL, want);   /* ... a changed table byte does not */
        pkg_tabler_add(&t, data, size);
        EXPECT(!pkg_tabler_end(&t), "size %zu: a changed table was accepted", size);
        pkg_tabler_begin(&t, size, got, NULL);
        EXPECT(!pkg_tabler_add(&t, data, size + 1), "size %zu: one byte too many", size);
        pkg_tabler_begin(&t, size, got, NULL);
        pkg_tabler_add(&t, data, size - 1);
        EXPECT(!pkg_tabler_end(&t), "size %zu: one byte too few", size);
    }
    free(data);
    free(want);
    free(got);
}

static void elf_notes(void)
{
    static const uint8_t list[] = "posix\nmount /data r\n";
    const struct pkgv_file *g = pkgv_find("good.jpk");
    struct pkgc_work *k = pkgc_work();
    if (!g || pkg_package_verify(g->data, g->len, k->key, NULL, k->m, NULL) != OK) {
        EXPECT(false, "good.jpk is not taken");
        return;
    }
    /* bin/vec is the first file: after the manifest, its padding and its own header */
    uint32_t msize;
    pkg_first_header(g->data, &msize);
    size_t at = PKG_BLOCK + ((msize + 511) / 512) * 512 + PKG_BLOCK;
    uint64_t size = k->m->files[0].size;
    uint8_t *elf = malloc(size);
    if (!elf)
        return;
    memcpy(elf, g->data + at, size);
    EXPECT(pkg_elf_list_is(elf, size, list, sizeof(list) - 1), "the vector's ELF note is not its list");
    EXPECT(!pkg_elf_list_is(elf, size, list, sizeof(list) - 2), "a shorter list taken");
    EXPECT(!pkg_elf_list_is(elf, size, (const uint8_t *)"posix\nmount /data rw\n", 21),
           "another list taken");
    for (uint64_t cut = 0; cut < 220; cut++) {   /* the note ends at byte 220 */
        uint8_t *exact = malloc(cut ? cut : 1);
        if (!exact)
            break;
        memcpy(exact, elf, cut);
        EXPECT(!pkg_elf_list_is(exact, cut, list, sizeof(list) - 1), "an ELF cut at %lu taken",
               (unsigned long)cut);
        free(exact);
    }
    for (unsigned i = 0; i < 300; i++) {   /* every header byte changed: no crash, and a changed note is no match */
        uint8_t save = elf[i];
        elf[i] = (uint8_t)(save ^ 0x55);
        (void)pkg_elf_list_is(elf, size, list, sizeof(list) - 1);
        elf[i] = save;
    }
    for (uint64_t i = 0; i + 5 < size; i++)
        if (!memcmp(elf + i, "JamOS", 5)) {
            elf[i + 4] = 'T';   /* JamOT */
            EXPECT(!pkg_elf_list_is(elf, size, list, sizeof(list) - 1), "a note named JamOT taken");
            break;
        }
    free(elf);
}

static void names_and_ranges(void)
{
    char out[96];
    uint8_t sha[SHA256_BYTES];
    for (unsigned i = 0; i < SHA256_BYTES; i++)
        sha[i] = (uint8_t)(i * 17);
    EXPECT(pkg_repo_path('p', STR("vec"), STR("1.0"), 1, sha, out, sizeof(out)) == OK &&
               !strcmp(out, "pkg/vec-1.0-r1-0011223344556677.jpk"), "a package's path: %s", out);
    EXPECT(pkg_repo_path('s', STR("vec"), STR("1.0"), 12, sha, out, sizeof(out)) == OK &&
               !strcmp(out, "src/vec-1.0-r12-0011223344556677.src.tar"), "a source's path: %s", out);
    size_t need = strlen("pkg/vec-1.0-r1-0011223344556677.jpk");
    EXPECT(pkg_repo_path('p', STR("vec"), STR("1.0"), 1, sha, out, need) == ERR_BUFFER_TOO_SMALL &&
               pkg_repo_path('p', STR("vec"), STR("1.0"), 1, sha, out, need + 1) == OK,
           "the path's buffer bound");
    EXPECT(pkg_repo_path('x', STR("vec"), STR("1.0"), 1, sha, out, sizeof(out)) != OK, "kind x");
    /* the range rule */
    struct pkg_vuln e = { STR("zlib"), { NULL, 0 }, STR("1.3.1"), 2, STR("CVE-1"), STR("s") };
    EXPECT(pkg_vuln_affected(&e, STR("1.3.1"), 1) && pkg_vuln_affected(&e, STR("1.3.1"), 0) &&
               !pkg_vuln_affected(&e, STR("1.3.1"), 2) && pkg_vuln_affected(&e, STR("1.2"), 9) &&
               !pkg_vuln_affected(&e, STR("1.3.2"), 0), "fixed 1.3.1 revision 2");
    e.fixrev = 0;
    EXPECT(pkg_vuln_affected(&e, STR("1.3.0"), 0) && !pkg_vuln_affected(&e, STR("1.3.1"), 0),
           "fixed 1.3.1, no revision");
    e.first = STR("1.2.0");
    EXPECT(!pkg_vuln_affected(&e, STR("1.1"), 0) && pkg_vuln_affected(&e, STR("1.2.0"), 0),
           "first affected 1.2.0");
    e.fixed = (struct pkg_str){ NULL, 0 };
    EXPECT(pkg_vuln_affected(&e, STR("99"), 0) && !pkg_vuln_affected(&e, STR("1.1"), 0), "no fix yet");
}

static void index_lookups(void)
{
    struct pkgc_work *k = pkgc_work();
    const struct pkgv_file *f = pkgv_find("INDEX-good");
    struct pkg_dep deps[PKG_DEPS_MAX];
    if (!f || pkg_index_parse(f->data, f->len, k->key, NULL, k->x, NULL) != OK) {
        EXPECT(false, "INDEX-good is not taken");
        return;
    }
    const struct pkg_ipkg *p = pkg_index_find(k->x, STR("vec"));
    EXPECT(p && pkg_index_find(k->x, STR("vec-data")) && !pkg_index_find(k->x, STR("ve")) &&
               !pkg_index_find(k->x, STR("zzz")), "index lookups");
    EXPECT(p && pkg_index_runs(k->x, p, deps) == 1 && pkg_str_eq(deps[0].name, "vec-data") &&
               pkg_str_eq(deps[0].least, "1.0"), "the runs line");
    EXPECT(k->x->serial == 5 && !strcmp(k->x->abi, "0123456789abcdef"), "the index's serial and abi");
}

unsigned pkgc_misc(void)
{
    bad = 0;
    if (!pkgc_work())
        return 1;
    versions();
    paths();
    lists();
    headers();
    tables();
    elf_notes();
    names_and_ranges();
    index_lookups();
    return bad;
}
