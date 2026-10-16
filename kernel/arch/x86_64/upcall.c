/* Fault upcalls (docs/SIGNALS-PLAN.md section 1): a user fault that would
 * kill its process goes to the process's exception entry instead. The ABI
 * is in <jam/abi.h> (struct exc_frame) and abi/syscalls.def (170, 171);
 * until the delivery lands both calls are ERR_NOT_SUPPORTED. */
#include <jam/status.h>
#include <jam/syscall_impl.h>

int64_t sysc_exception_entry_set(uint64_t entry)
{
    (void)entry;
    return ERR_NOT_SUPPORTED;
}

int64_t sysc_exception_done(uint64_t frame, uint32_t how)
{
    (void)frame;
    (void)how;
    return ERR_NOT_SUPPORTED;
}
