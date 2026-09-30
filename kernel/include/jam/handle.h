/* Handles: per-table references to kernel objects, each with rights.
 *
 * A handle value is (slot + 1) << 15 | generation (17 + 15 bits; a table has
 * at most 65,536 slots). Closing a slot bumps its generation and freed slots
 * are reused FIFO, so a stale handle value fails with ERR_BAD_HANDLE instead
 * of silently naming whatever object reuses the slot. 0 is never valid.
 *
 * struct khandle is a handle outside any table: one handle count plus one
 * reference on the object, with rights. It is what moves through channel
 * messages. Whoever holds a khandle must eventually insert it into a table
 * or khandle_release it. */
#pragma once

#include <stdint.h>
#include <jam/abi.h>
#include <jam/object.h>
#include <jam/spinlock.h>
#include <jam/status.h>

/* handle_t, rights_t and the RIGHT_* bits are in <jam/abi.h> (user code
 * needs them too). */

struct khandle {
    struct kobject *obj;      /* a reference, or NULL */
    rights_t        rights;   /* what the holder may do with it */
};

/* Wrap a new object (fresh from its create function, refs = 1) as a
 * khandle, taking over the creator's reference. */
struct khandle khandle_from_new(struct kobject *obj, rights_t rights);
void khandle_release(struct khandle *kh);

struct handle_slot;
struct job;

struct handle_table {
    spinlock_t          lock;        /* "handle table": guards everything below */
    struct handle_slot *slots;       /* capacity entries, grown on demand */
    uint32_t            capacity;    /* entries in slots */
    uint32_t            used;        /* entries holding an object */
    uint32_t            free_head;   /* slot index + 1 of first free, 0 = none */
    uint32_t            free_tail;   /* slot index + 1 of last free (FIFO reuse) */
    /* The job every slot in use or reserved is charged to (one
     * JOB_LIMIT_HANDLES unit each; NULL for kernel tables), and how many
     * units are charged now (lock). A full job fails an insert with
     * ERR_NO_RESOURCES, like a full table. The owner (the process) holds
     * the job reference. */
    struct job         *job;
    uint32_t            charged;
};

#define HANDLE_TABLE_MAX 65536

void     handle_table_init(struct handle_table *t);
/* Close every handle and free the table's memory. */
void     handle_table_destroy(struct handle_table *t);

/* Put a khandle into the table (it is consumed on success). */
status_t handle_insert(struct handle_table *t, struct khandle *kh, handle_t *out);
/* Look up h: must have every right in `need` and (unless OBJ_NONE) be of
 * `type`. On success *obj holds a NEW reference the caller must unref. */
status_t handle_get(struct handle_table *t, handle_t h, enum obj_type type, rights_t need,
                    struct kobject **obj, rights_t *rights);
status_t handle_close(struct handle_table *t, handle_t h);
/* Take h out of the table as a khandle, whatever its rights (kernel use:
 * undoing an insert the kernel made itself). */
status_t handle_remove(struct handle_table *t, handle_t h, struct khandle *out);
/* New handle to the same object with rights that are a subset (or
 * RIGHT_SAME). Needs RIGHT_DUPLICATE. */
status_t handle_duplicate(struct handle_table *t, handle_t h, rights_t rights, handle_t *out);
/* Take h out of the table as a khandle (for sending). Needs RIGHT_TRANSFER. */
status_t handle_take(struct handle_table *t, handle_t h, struct khandle *out);
/* Replace h with a new handle to the same object with fewer rights. */
status_t handle_replace(struct handle_table *t, handle_t h, rights_t rights, handle_t *out);
/* Undo handle_take for a send that FAILED: put kh back under its original
 * value h. The slot was reserved by handle_take and held, so this is O(1) and
 * cannot fail (*out == h; kh is consumed). */
status_t handle_untake(struct handle_table *t, handle_t h, struct khandle *kh, handle_t *out);
/* Finish handle_take for a send that SUCCEEDED: release the reserved slot so
 * it can be reused. Call once per handle whose khandle the send consumed. */
status_t handle_commit(struct handle_table *t, handle_t h);
#ifndef JAM_NO_KTESTS
/* Tests: the rights of every live handle in t to an object of `type`
 * (up to cap of them into out); returns how many there are. */
uint32_t handle_table_rights(struct handle_table *t, enum obj_type type, rights_t *out,
                             uint32_t cap);
/* Tests: a new reference to the object of the first live handle of
 * `type` in t (another process's table: what it holds right now), or
 * ERR_NOT_FOUND. */
status_t handle_table_find(struct handle_table *t, enum obj_type type, struct kobject **out);
#endif
/* Reserve n empty slots (all or none; ERR_NO_RESOURCES if the table can't
 * hold them) so a receive can't fail halfway and lose handles. Each value in
 * out[] is filled with handle_untake (which then cannot fail) or given back
 * unused with handle_commit. */
status_t handle_reserve(struct handle_table *t, uint32_t n, handle_t *out);
