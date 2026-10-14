/* What pkgcases.c, pkgfuzz.c and pkgmisc.c share (pkgcases.h): the work
 * areas, a stream read of a whole archive, and what any accepted
 * structure must satisfy whatever bytes it was read from. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <pkgfmt.h>

/* The big structures, allocated once, and the vectors' key (test.pub). */
struct pkgc_work {
    struct pkg_manifest *m;
    struct pkg_index    *x;
    struct pkg_vulns    *v;
    struct pkg_journal  *j;
    uint8_t              key[PKG_KEY_BYTES];
};
/* NULL if the vectors' test.pub or memory is missing. */
struct pkgc_work *pkgc_work(void);
/* Give the work areas back (the next pkgc_work allocates them again). */
void pkgc_work_free(void);
/* A fixed-seed generator: the tests must repeat. */
uint64_t pkgc_rand(uint64_t *state);

/* The archive read as a stream (chunks of random sizes from rng) with the
 * installer's last check (each program's ELF note); OK or the first
 * refusal. abi NULL: not checked. */
status_t pkgc_stream(const uint8_t *d, size_t len, const char *abi, struct pkg_manifest *m,
                     uint64_t *rng);

/* The rules of the formats, checked again from outside on a structure a
 * reader accepted. */
bool pkgc_manifest_ok(const struct pkg_manifest *m);
bool pkgc_index_ok(const struct pkg_index *x);
bool pkgc_vulns_ok(const struct pkg_vulns *v);
