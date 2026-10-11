/* The libc-test port's table (generated: gen.py) and what the runner and
 * the tests' shim share. */
#ifndef LCT_H
#define LCT_H

#include <stdio.h>

/* A test: its name ("functional/argv") and its main, renamed by the build
 * (-Dmain=lct_functional_argv) so that one program holds them all. */
struct lct_test {
    const char *name;
    int (*main)(int, char **);
};

/* A test that is expected not to pass, and why: "kind: reason", the kind
 * one of enosys, limit, bug, pending, skip (gen.py's header). A skip one
 * is in no lct_tests entry: it is not built. */
struct lct_expect {
    const char *name;
    const char *why;
};

extern const struct lct_test lct_tests[];
extern const unsigned lct_ntests;
extern const struct lct_expect lct_expected[];
extern const unsigned lct_nexpected;

/* The tests' tmpfile (shim.h): musl's makes its file in /tmp, which Jam OS
 * has none of; this one in $TMPDIR. */
FILE *lct_tmpfile(void);

#endif
