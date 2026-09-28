/* Handles: per-table references to kernel objects, each with rights.
 *
 * A handle value is (slot + 1) << 8 | generation. Closing a slot bumps its
 * generation, so a stale handle value fails with ERR_BAD_HANDLE instead of
 * silently naming whatever object reuses the slot. 0 is never valid.
 *
 * struct khandle is a handle outside any table: one handle count plus one
 * reference on the object, with rights. It is what moves through channel
 * messages. Whoever holds a khandle must eventually insert it into a table
 * or khandle_release it. */
#pragma once

#include <stdint.h>
#include <jam/object.h>
#include <jam/spinlock.h>
#include <jam/status.h>

typedef uint32_t handle_t;
#define HANDLE_INVALID 0u

typedef uint32_t rights_t;
#define RIGHT_READ      (1u << 0)
#define RIGHT_WRITE     (1u << 1)
#define RIGHT_EXEC      (1u << 2)
#define RIGHT_MAP       (1u << 3)
#define RIGHT_DUPLICATE (1u << 4)
#define RIGHT_TRANSFER  (1u << 5)
#define RIGHT_SIGNAL    (1u << 6)   /* may set/clear user signals */
#define RIGHT_WAIT      (1u << 7)   /* may wait on it / bind it to a port */
#define RIGHT_INSPECT   (1u << 8)
#define RIGHT_SAME      0x80000000u /* in duplicate: keep the same rights */

#define RIGHTS_BASIC (RIGHT_DUPLICATE | RIGHT_TRANSFER | RIGHT_WAIT | RIGHT_INSPECT)
#define RIGHTS_IO    (RIGHT_READ | RIGHT_WRITE)

struct khandle {
    struct kobject *obj;
    rights_t        rights;
};

/* Wrap a new object (fresh from its create function, refs = 1) as a
 * khandle, taking over the creator's reference. */
struct khandle khandle_from_new(struct kobject *obj, rights_t rights);
void khandle_release(struct khandle *kh);

struct handle_slot;

struct handle_table {
    spinlock_t          lock;
    struct handle_slot *slots;
    uint32_t            capacity;
    uint32_t            used;
    uint32_t            free_head;   /* slot index + 1 of first free, 0 = none */
    uint32_t            free_tail;   /* slot index + 1 of last free (FIFO reuse) */
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
