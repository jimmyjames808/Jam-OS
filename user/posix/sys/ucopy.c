/* The checked copies' C side (ucopy.h): the exception table's search,
 * which the fault path runs in the kernel's window (so it touches only
 * the frame and the table), and uc_or_fault. The routines are ucopy.S's. */
#include <errno.h>
#include "layer.h"

/* A line of ucopy.S's table: a marked instruction and its fixup, each as
 * an offset from its own field. */
struct uc_line {
    int32_t at, fixup;
};
extern const struct uc_line uc_table[], uc_table_end[];

bool uc_fault(struct exc_frame *f)
{
    /* A page fault (Linux's EFAULT for every kind: nothing mapped, no
     * access, a file's page that failed), or a general protection fault:
     * an address that isn't one. Not a trap after the instruction. */
    if (f->kind != EXC_FAULT || (f->vector != 14 && f->vector != 13))
        return false;
    for (const struct uc_line *l = uc_table; l < uc_table_end; l++)
        if ((uintptr_t)&l->at + (uintptr_t)(intptr_t)l->at == f->rip) {
            f->rip = (uintptr_t)&l->fixup + (uintptr_t)(intptr_t)l->fixup;
            return true;
        }
    return false;
}

long uc_or_fault(long r, const void *p, size_t n, bool write)
{
    if (r != -EINVAL)
        return r;
    long c = write ? uc_writable((void *)(uintptr_t)p, n) : uc_readable(p, n);
    return c ? c : r;
}
