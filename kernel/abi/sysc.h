/* Shared helpers for the system call implementations (kernel/abi/sysc_*.c).
 *
 * The rules every sysc_* follows (M5-PLAN.md, "Fixed decisions"):
 *   - user pointers are uint64_t and are only touched through
 *     copy_from_user / copy_to_user, never under a spinlock or the aspace
 *     region lock (a copy may fault and sleep);
 *   - arguments are copied into the kernel once, then checked, then used;
 *   - a result that can't be copied out is undone where it can be (a new
 *     handle is closed again, a mapping unmapped) and the call fails with
 *     ERR_INVALID_ARGS, so a bad pointer never leaks a handle;
 *   - every call works on the calling process's handle table; a thread
 *     without a process (the kernel's own ring-3 tests) gets ERR_BAD_STATE. */
#pragma once

#include <jam/handle.h>
#include <jam/process.h>
#include <jam/usercopy.h>

/* The caller's handle table, or NULL for a thread with no process. */
static inline struct handle_table *sysc_table(void)
{
    struct process *p = process_current();
    return p ? process_handles(p) : NULL;
}

#define SYSC_TABLE(t)                        \
    struct handle_table *t = sysc_table();   \
    if (!t)                                  \
        return ERR_BAD_STATE

/* copy_from_user / copy_to_user where an empty copy is always fine, even
 * with a NULL pointer (a message with no handles, an empty reply). */
static inline status_t copy_in(void *dst, uint64_t usrc, size_t n)
{
    return n ? copy_from_user(dst, usrc, n) : OK;
}

static inline status_t copy_out(uint64_t udst, const void *src, size_t n)
{
    return n ? copy_to_user(udst, src, n) : OK;
}

/* Copy a new handle's value to user memory; if that fails, close it. */
static inline status_t sysc_put_handle(struct handle_table *t, uint64_t uout, handle_t h)
{
    if (copy_to_user(uout, &h, sizeof(h)) == OK)
        return OK;
    handle_close(t, h);
    return ERR_INVALID_ARGS;
}

/* A kernel object fresh from its constructor (refs = 1) into t with these
 * rights, and its value out to the user: on any failure the object is gone
 * and nothing is left in t. */
static inline status_t sysc_publish(struct handle_table *t, struct kobject *obj, rights_t rights,
                                    uint64_t uout)
{
    struct khandle kh = khandle_from_new(obj, rights);
    handle_t h;
    status_t st = handle_insert(t, &kh, &h);
    if (st != OK) {
        khandle_release(&kh);
        return st;
    }
    return sysc_put_handle(t, uout, h);
}
