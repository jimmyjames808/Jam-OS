/* extinv: a volume's invariants, for extcheck's writing tests
 * (tools/extnames.c and the others) on the Mac, where there is no e2fsck.
 * The checker itself is the library's (user/services/extfs/ext/check.c:
 * the tree, link counts, inodes, blocks and counts; ext_check.h lists
 * them), the same code `jamext check` and Jam OS's `disk check` run; this
 * file gives it memory and fails the run at the first problem it reports. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ext_check.h>
#include "extcheck.h"

struct inv_run {
    char first[256];                 /* the first problem, as reported */
    unsigned problems;
};

static void *run_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return calloc(1, n);
}

static void run_release(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

static void run_report(void *ctx, const char *fmt, va_list ap)
{
    struct inv_run *r = ctx;
    if (!r->problems++)
        vsnprintf(r->first, sizeof(r->first), fmt, ap);
}

void check_invariants(struct ext_fs *fs, struct inv *out)
{
    struct inv_run run = { .problems = 0 };
    struct ext_check_io io = {
        .ctx = &run, .alloc = run_alloc, .release = run_release, .report = run_report,
    };
    struct ext_check_result res;
    memset(out, 0, sizeof(*out));
    status_t st = ext_check(fs, &io, &res);
    if (run.problems)
        fail("%s", run.first);
    if (st != OK)
        fail("check: status %d", st);
    out->dirs = res.dirs;
    out->names = res.names;
    out->inodes = res.inodes;
    out->orphans = res.orphans;
    out->free_inodes = res.free_inodes;
    out->free_blocks = res.free_blocks;
    out->exact = res.exact;
    check_pins(fs);
}
