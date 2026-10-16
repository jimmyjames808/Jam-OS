/* System calls 160-166: pagers (abi/syscalls.def; the object is
 * <jam/pager.h>). pager_supply's bytes are copied straight from the
 * caller's buffer into each fresh page (struct pager_src), one page at a
 * time with no lock held, so up to PAGER_SUPPLY_MAX bytes never pass
 * through the kernel stack. The rules every sysc_* follows are in sysc.h. */
#include <stdint.h>
#include <jam/pager.h>
#include <jam/syscall_impl.h>
#include "sysc.h"

int64_t sysc_pager_create(handle_t root, uint64_t max_pages, uint64_t answer_ns, uint32_t flags,
                          uint64_t out)
{
    SYSC_TABLE(t);
    handle_t h;
    status_t st = sys_pager_create(t, root, max_pages, answer_ns, flags, &h);
    return st != OK ? st : sysc_put_handle(t, out, h);
}

int64_t sysc_pager_create_vmo(handle_t pager, uint64_t key, uint64_t size, uint32_t flags,
                              uint64_t out)
{
    SYSC_TABLE(t);
    handle_t h;
    status_t st = sys_pager_create_vmo(t, pager, key, size, flags, &h);
    return st != OK ? st : sysc_put_handle(t, out, h);
}

/* A request that can't be copied out stays delivered: PAGER_NEXT_AGAIN
 * gives it again, else it ends at its deadline as any unanswered one. */
int64_t sysc_pager_next(handle_t pager, uint32_t flags, uint64_t out)
{
    SYSC_TABLE(t);
    struct pager_request req;
    status_t st = sys_pager_next(t, pager, flags, &req);
    if (st != OK)
        return st;
    return copy_to_user(out, &req, sizeof(req)) == OK ? OK : ERR_INVALID_ARGS;
}

int64_t sysc_pager_supply(handle_t pager, uint64_t key, uint64_t offset, uint64_t buf,
                          uint64_t len)
{
    SYSC_TABLE(t);
    struct pager_src src = { .addr = buf, .user = true };
    return sys_pager_supply(t, pager, key, offset, &src, len);
}

int64_t sysc_pager_op(handle_t pager, uint64_t key, uint32_t op, uint64_t offset, uint64_t len,
                      int64_t arg)
{
    SYSC_TABLE(t);
    return sys_pager_op(t, pager, key, op, offset, len, arg);
}

/* Writable pager VMOs' calls (docs/WRITEMAP-PLAN.md): the ABI first, the
 * kernel halves with WK1 and WK2. */
int64_t sysc_pager_dirty(handle_t pager, uint64_t key, uint32_t flags, uint64_t run, uint64_t buf)
{
    (void)pager, (void)key, (void)flags, (void)run, (void)buf;
    return ERR_NOT_SUPPORTED;
}

int64_t sysc_pager_write(handle_t pager, uint64_t key, uint64_t offset, uint64_t buf, uint64_t len)
{
    (void)pager, (void)key, (void)offset, (void)buf, (void)len;
    return ERR_NOT_SUPPORTED;
}
