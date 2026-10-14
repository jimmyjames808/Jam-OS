/* The ports formats' checks (<pkgfmt.h>), written once for two homes:
 * utest runs them on Jam OS (user/tests/utest/pkgfmt.c) and `make check`
 * runs them on the Mac (tools/pkgcheck.c, ASan and UBSan), both over the
 * same bytes, ports/vectors/, which tools/ports.py made and its Python
 * reference reader gives an answer to (CASES). Where the vectors come
 * from differs: utest has them linked in (build/gen/pkgvectors.c, made by
 * tools/pkgvec2c.py), pkgcheck reads the directory. Each check prints what
 * failed and returns how many things did. Only <os.h>'s printf, malloc and
 * string functions are used. */
#pragma once

#include <stddef.h>
#include <stdint.h>

struct pkgv_file {
    const char    *name;
    const uint8_t *data;
    size_t         len;
};
extern const struct pkgv_file *pkgv_files;
extern unsigned                pkgv_nfiles;

/* The vector called name, or NULL. */
const struct pkgv_file *pkgv_find(const char *name);

/* Every line of CASES: the C readers give the answer the Python
 * reference gave (a package is read whole and as a stream of chunks, and
 * the two must agree). */
unsigned pkgc_vectors(void);
/* iterations mutated copies of the vectors' packages, indexes, VULNS and
 * manifests: nothing crashes (the Mac's run is sanitised), a mutant that
 * is accepted with its signature checked is the original, and one that
 * is read without it keeps every rule of the format. */
unsigned pkgc_fuzz(uint64_t seed, unsigned iterations);
/* The journal: round trips, hand-written damage, a torn write at every
 * length, and iterations mutations of a journal (an accepted one is
 * written back as the same bytes). */
unsigned pkgc_journal(uint64_t seed, unsigned iterations);
/* The small parts: versions, paths, lists, headers, page tables, the
 * ELF note finder, repository names, the audit's range rule. */
unsigned pkgc_misc(void);

/* For tools/pkgcheck.c's comparison with Python's reader on texts it made:
 * 1 if the C reader takes the text (signature not checked) and 0 if it
 * refuses. kind: "manifest", "index" or "vulns". */
int pkgc_text_verdict(const char *kind, const uint8_t *text, size_t len);
/* 1 if the package (signature checked against the vectors' test.pub, abi
 * of the vectors' packages) is taken whole and as a stream, 0 if both
 * refuse, -1 if they disagree. */
int pkgc_package_verdict(const uint8_t *data, size_t len);
/* The order of two versions, as Python's vcmp. */
int pkgc_version_cmp(const char *a, const char *b);
/* 1 if pkg_path_problem takes the path. */
int pkgc_path_ok(const char *path);
