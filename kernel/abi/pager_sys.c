/* System calls 160-164: pagers (abi/syscalls.def). Not built yet: every
 * call answers ERR_NOT_SUPPORTED until the pager object is in. */
#include <stdint.h>
#include <jam/status.h>
#include <jam/syscall_impl.h>

int64_t sysc_pager_create(handle_t root, uint64_t max_pages, uint64_t answer_ns, uint32_t flags,
                          uint64_t out)
{
    (void)root, (void)max_pages, (void)answer_ns, (void)flags, (void)out;
    return ERR_NOT_SUPPORTED;
}

int64_t sysc_pager_create_vmo(handle_t pager, uint64_t key, uint64_t size, uint32_t flags,
                              uint64_t out)
{
    (void)pager, (void)key, (void)size, (void)flags, (void)out;
    return ERR_NOT_SUPPORTED;
}

int64_t sysc_pager_next(handle_t pager, uint32_t flags, uint64_t out)
{
    (void)pager, (void)flags, (void)out;
    return ERR_NOT_SUPPORTED;
}

int64_t sysc_pager_supply(handle_t pager, uint64_t key, uint64_t offset, uint64_t buf,
                          uint64_t len)
{
    (void)pager, (void)key, (void)offset, (void)buf, (void)len;
    return ERR_NOT_SUPPORTED;
}

int64_t sysc_pager_op(handle_t pager, uint64_t key, uint32_t op, uint64_t offset, uint64_t len,
                      int64_t arg)
{
    (void)pager, (void)key, (void)op, (void)offset, (void)len, (void)arg;
    return ERR_NOT_SUPPORTED;
}
