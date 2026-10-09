/* The only way the kernel touches user memory (usercopy.S).
 *
 * User addresses are uint64_t in kernel code, never C pointers. Each copy
 * checks the range against [USER_BASE, USER_TOP), runs with SMAP opened
 * (stac/clac) only for the copy itself, and resolves page faults through
 * the current thread's address space (aspace_fault). A bad range or an
 * unresolvable fault returns ERR_INVALID_ARGS; part of the data may have
 * been copied by then. A page of a pager VMO (<jam/pager.h>) is waited
 * for: ERR_IO if its pager failed it, is gone or didn't answer in time,
 * ERR_CANCELED if the thread was killed meanwhile. Only reads can wait (a
 * pager VMO is never mapped writable), so copy_to_user never does. A copy
 * may wait seconds for a pager: never make one holding a mutex another
 * process's progress needs (PK2's audit, docs/M12.5-PLAN.md). */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <jam/status.h>

status_t copy_from_user(void *dst, uint64_t usrc, size_t n);
status_t copy_to_user(uint64_t udst, const void *src, size_t n);
/* Copy a NUL-terminated string of at most cap - 1 characters (cap >= 1);
 * *len (may be NULL) gets its length. ERR_OUT_OF_RANGE if there is no NUL
 * within cap bytes. */
status_t copy_str_from_user(char *dst, uint64_t usrc, size_t cap, size_t *len);
