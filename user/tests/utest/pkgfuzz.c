/* Mutated ports formats (see pkgcases.h): the vectors' packages,
 * indexes, VULNS and manifests with bytes flipped, cut, inserted, copied
 * and swapped, read by the C readers. The Mac's run is sanitised, so a
 * read outside a buffer or an overflow stops it. Two things must hold
 * for every mutant: with its signature checked it is refused unless it
 * is the original (the formats have one spelling), and read without the
 * signature it keeps every rule the format has (pkgc_*_ok). */
#include <os.h>
#include <pkgfmt.h>
#include "pkgcases.h"
#include "pkgcases_int.h"

/* One random change to b (len bytes, room for cap): the new length. */
static size_t mutate_once(uint8_t *b, size_t len, size_t cap, uint64_t *rng)
{
    static const uint8_t special[] = { 0, '\n', ' ', 0xff, '0', '9', '-', '/', '.', '!', 'a', 'f' };
    uint64_t r = pkgc_rand(rng);
    size_t at = len ? (size_t)(pkgc_rand(rng) % len) : 0, n = 1 + (size_t)(pkgc_rand(rng) % 16);
    switch (r % 8) {
    case 0:
        if (len)
            b[at] ^= (uint8_t)(1u << (pkgc_rand(rng) & 7));
        break;
    case 1:
        if (len)
            b[at] = (uint8_t)pkgc_rand(rng);
        break;
    case 2:
        if (len)
            b[at] = special[pkgc_rand(rng) % sizeof(special)];
        break;
    case 3:   /* delete a range */
        n = n < len - at ? n : len - at;
        memmove(b + at, b + at + n, len - at - n);
        return len - n;
    case 4:   /* insert bytes */
        if (len + n > cap)
            break;
        memmove(b + at + n, b + at, len - at);
        for (size_t i = 0; i < n; i++)
            b[at + i] = special[pkgc_rand(rng) % sizeof(special)];
        return len + n;
    case 5:   /* cut it short */
        return at;
    case 6: { /* copy a range over another place */
        size_t to = len ? (size_t)(pkgc_rand(rng) % len) : 0;
        n = n < len - at ? n : len - at;
        n = n < len - to ? n : len - to;
        memmove(b + to, b + at, n);
        break;
    }
    default:  /* swap two ranges */
        if (len > 2 * n) {
            size_t to = (size_t)(pkgc_rand(rng) % (len - n));
            for (size_t i = 0; i < n && at + i < len; i++) {
                uint8_t t = b[at + i];
                b[at + i] = b[to + i];
                b[to + i] = t;
            }
        }
        break;
    }
    return len;
}

/* A mutant of base in a buffer of exactly its length (so a read past it
 * is the sanitiser's to see); *len its length. */
static uint8_t *mutant(const uint8_t *base, size_t blen, uint64_t *rng, size_t *len)
{
    size_t cap = blen + 64, n = blen;
    uint8_t *work = malloc(cap);
    if (!work)
        return NULL;
    memcpy(work, base, blen);
    for (unsigned k = 1 + (unsigned)(pkgc_rand(rng) % 3); k; k--)
        n = mutate_once(work, n, cap, rng);
    uint8_t *exact = malloc(n ? n : 1);
    if (exact)
        memcpy(exact, work, n);
    free(work);
    *len = n;
    return exact;
}

struct base {
    const char    *name;     /* the vector (for a manifest: its package) */
    const uint8_t *data;
    size_t         len;
    int            kind;     /* 0 package, 1 index, 2 vulns, 3 manifest text */
};

static unsigned check_package(const struct base *b, const uint8_t *d, size_t len)
{
    int v = pkgc_package_verdict(d, len);
    if (v < 0) {
        printf("pkgfmt: fuzz: %s: the whole and the streamed read disagree (or an accepted manifest breaks a rule)\n",
               b->name);
        return 1;
    }
    if (v == 1 && (len != b->len || memcmp(d, b->data, len))) {
        printf("pkgfmt: fuzz: %s: a changed package was accepted\n", b->name);
        return 1;
    }
    return 0;
}

static unsigned check_text(const struct base *b, const uint8_t *d, size_t len)
{
    struct pkgc_work *k = pkgc_work();
    unsigned bad = 0;
    bool same = len == b->len && !memcmp(d, b->data, len);
    if (b->kind == 1) {
        if (pkg_index_parse(d, len, k->key, NULL, k->x, NULL) == OK && !same)
            bad++;
        if (pkg_index_parse_unchecked(d, len, k->x, NULL) == OK && !pkgc_index_ok(k->x))
            bad++;
    } else if (b->kind == 2) {
        if (pkg_vulns_parse(d, len, k->key, NULL, k->v, NULL) == OK && !same)
            bad++;
        if (pkg_vulns_parse_unchecked(d, len, k->v, NULL) == OK && !pkgc_vulns_ok(k->v))
            bad++;
    } else {
        if (pkg_manifest_parse(d, len, k->key, k->m, NULL) == OK && !same)
            bad++;
        if (pkg_manifest_parse_unchecked(d, len, k->m, NULL) == OK && !pkgc_manifest_ok(k->m))
            bad++;
    }
    if (bad)
        printf("pkgfmt: fuzz: %s: a mutant was %s\n", b->name,
               same ? "read without its rules" : "accepted though changed");
    return bad;
}

unsigned pkgc_fuzz(uint64_t seed, unsigned iterations)
{
    static const char *const names[] = { "good.jpk", "data.jpk", "p-ext.jpk", "multi.jpk",
                                         "INDEX-good", "VULNS-good" };
    struct base bases[7];
    unsigned nbases = 0, fails = 0;
    uint64_t rng = seed;
    if (!pkgc_work())
        return 1;
    for (unsigned i = 0; i < 6; i++) {
        const struct pkgv_file *f = pkgv_find(names[i]);
        if (!f) {
            printf("pkgfmt: fuzz: no vector %s\n", names[i]);
            return 1;
        }
        bases[nbases++] = (struct base){ names[i], f->data, f->len, i < 4 ? 0 : (int)i - 3 };
    }
    uint32_t msize;   /* the manifest text inside good.jpk */
    const struct pkgv_file *g = pkgv_find("good.jpk");
    if (pkg_first_header(g->data, &msize) != OK)
        return 1;
    bases[nbases++] = (struct base){ "good.jpk's manifest", g->data + PKG_BLOCK, msize, 3 };
    for (unsigned it = 0; it < iterations; it++) {
        const struct base *b = &bases[it % nbases];
        size_t len;
        uint8_t *d = mutant(b->data, b->len, &rng, &len);
        if (!d)
            return fails + 1;
        fails += b->kind == 0 ? check_package(b, d, len) : check_text(b, d, len);
        free(d);
        if (fails > 20)
            break;
    }
    return fails;
}
