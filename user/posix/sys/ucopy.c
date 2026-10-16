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
void uc_fail_io(void);   /* ucopy.S: the -EIO fixup */

/* A page fault's status that says the file's pager failed the page (an
 * I/O error, the pager gone, or no answer in time): the kernel's own
 * copies answer ERR_IO for such a page, so ours say EIO too. */
static bool pager_failed(const struct exc_frame *f)
{
    return f->vector == 14 &&
           (f->status == ERR_IO || f->status == ERR_PEER_CLOSED || f->status == ERR_TIMED_OUT);
}

bool uc_fault(struct exc_frame *f)
{
    /* A page fault (EFAULT: nothing mapped, no access, past the end of a
     * shrunk file, as Linux; EIO: a page the file's pager failed), or a
     * general protection fault: an address that isn't one. Not a trap
     * after the instruction. */
    if (f->kind != EXC_FAULT || (f->vector != 14 && f->vector != 13))
        return false;
    for (const struct uc_line *l = uc_table; l < uc_table_end; l++)
        if ((uintptr_t)&l->at + (uintptr_t)(intptr_t)l->at == f->rip) {
            f->rip = pager_failed(f) ? (uintptr_t)uc_fail_io
                                     : (uintptr_t)&l->fixup + (uintptr_t)(intptr_t)l->fixup;
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
