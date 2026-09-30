/* Handle tables (handle.h).
 *
 * Job charges: every slot that is in use or reserved (in transit, or
 * reserved for a receive) costs its table's job one JOB_LIMIT_HANDLES unit.
 * Charges are taken before a slot is filled and credited when it goes back
 * on the free list; t->charged (under the lock) says how many are out, so
 * destroying the table credits exactly those. */
#include <jam/handle.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/process.h>
#include <jam/string.h>

struct handle_slot {
    struct kobject *obj;        /* NULL: free or in transit */
    rights_t        rights;     /* what this handle may do */
    uint32_t        gen;        /* bumped on each reuse: stale handles don't match */
    uint32_t        next_free;  /* slot index + 1, 0 = end */
    bool            intransit;  /* taken for a send, reserved until commit/untake */
};

/* A handle is (slot + 1) << GEN_BITS | generation. HANDLE_TABLE_MAX is 65536,
 * so slot + 1 needs 17 bits; the remaining 15 bits are the generation.
 * Eight bits would wrap after 256 reuses of a slot, letting a stale handle
 * name a new object; 15 bits plus FIFO slot reuse (below) push that far out
 * of reach (test: auditD_handle_generation_no_wrap). */
#define GEN_BITS 15
#define GEN_MASK ((1u << GEN_BITS) - 1)

static handle_t encode(uint32_t slot, uint32_t gen)
{
    return (slot + 1) << GEN_BITS | (gen & GEN_MASK);
}

/* Returns the slot for h if it is live and current, else NULL. A free or
 * in-transit slot (obj == NULL) never matches. */
static struct handle_slot *decode(const struct handle_table *t, handle_t h)
{
    uint32_t idx = h >> GEN_BITS;
    if (idx == 0 || idx > t->capacity)
        return NULL;
    struct handle_slot *s = &t->slots[idx - 1];
    if (!s->obj || (s->gen & GEN_MASK) != (h & GEN_MASK))
        return NULL;
    return s;
}

/* Append slot `idx` to the free list. Reuse is FIFO (append at the tail, hand
 * out from the head) so a freed slot waits behind every other free slot
 * before it is reused, maximising the reuse distance and, with the wider
 * generation, making a stale handle value hitting its old slot with the same
 * generation practically impossible. */
static void freelist_push(struct handle_table *t, uint32_t idx)
{
    t->slots[idx].next_free = 0;
    if (t->free_tail)
        t->slots[t->free_tail - 1].next_free = idx + 1;
    else
        t->free_head = idx + 1;
    t->free_tail = idx + 1;
}

struct khandle khandle_from_new(struct kobject *obj, rights_t rights)
{
    kobject_handle_gain(obj);
    return (struct khandle){ obj, rights };
}

void khandle_release(struct khandle *kh)
{
    if (!kh->obj)
        return;
    struct kobject *obj = kh->obj;
    kh->obj = NULL;
    kobject_handle_drop(obj);
    kobject_unref(obj);
}

void handle_table_init(struct handle_table *t)
{
    spin_init(&t->lock, "handle table");
    t->slots = NULL;
    t->capacity = t->used = t->free_head = t->free_tail = 0;
    t->job = NULL;
    t->charged = 0;
}

/* Charge n handle units to t's job (no lock needed: job counters are
 * atomic). The caller adds n to t->charged under the lock once the slots
 * are really taken, or uncharges them again. */
static status_t charge(const struct handle_table *t, uint32_t n)
{
    return job_charge(t->job, JOB_LIMIT_HANDLES, n);
}

static void uncharge(const struct handle_table *t, uint32_t n)
{
    job_uncharge(t->job, JOB_LIMIT_HANDLES, n);
}

void handle_table_destroy(struct handle_table *t)
{
    /* Resume scanning from where we left off rather than from slot 0 each
     * time: releasing a handle drops the lock (it may run on_zero_handles),
     * but no concurrent insert can happen during teardown, and slots never
     * move here, so the index stays valid. This is O(n), not O(n^2). */
    uint32_t i = 0;
    for (;;) {
        struct khandle kh = { 0 };
        uint64_t f = spin_lock_irqsave(&t->lock);
        while (i < t->capacity && !t->slots[i].obj)
            i++;
        if (i < t->capacity) {
            kh.obj = t->slots[i].obj;
            kh.rights = t->slots[i].rights;
            t->slots[i].obj = NULL;
            t->used--;
            i++;
        }
        spin_unlock_irqrestore(&t->lock, f);
        if (!kh.obj)
            break;
        khandle_release(&kh);   /* outside the lock: may run on_zero_handles */
    }
    /* Emptied under the lock: a reader of another process's table
     * (handle_table_objects) sees no slots rather than freed ones. */
    uint64_t f = spin_lock_irqsave(&t->lock);
    struct handle_slot *slots = t->slots;
    t->slots = NULL;
    t->capacity = t->free_head = t->free_tail = 0;
    spin_unlock_irqrestore(&t->lock, f);
    kfree(slots);
    uncharge(t, t->charged);   /* the closed slots (and any reservation) */
    t->charged = 0;
}

/* With t->lock held: make sure there is a free slot. Used by the rare paths
 * (duplicate/replace) that hold a decoded slot; the common insert path grows
 * outside the lock via grow_table. */
static status_t ensure_free_locked(struct handle_table *t)
{
    if (t->free_head)
        return OK;
    uint32_t cap = t->capacity ? t->capacity * 2 : 16;
    if (cap > HANDLE_TABLE_MAX)
        cap = HANDLE_TABLE_MAX;
    if (cap == t->capacity)
        return ERR_NO_RESOURCES;
    struct handle_slot *slots = kzalloc(sizeof(*slots) * cap);
    if (!slots)
        return ERR_NO_MEMORY;
    if (t->slots)
        memcpy(slots, t->slots, sizeof(*slots) * t->capacity);
    /* Chain the new slots onto the free list. */
    for (uint32_t i = t->capacity; i < cap; i++)
        slots[i].next_free = i + 1 < cap ? i + 2 : 0;
    kfree(t->slots);
    t->free_head = t->capacity + 1;
    t->free_tail = cap;
    t->slots = slots;
    t->capacity = cap;
    return OK;
}

/* Grow the table if it is still `oldcap` and full, allocating the (up to
 * ~1.5 MiB) new array OUTSIDE the handle lock so a bulk insert doesn't hold
 * the lock with IRQs off across the allocation and copy. The caller retries.
 * Returns OK when it either grew or found the situation already changed. */
static status_t grow_table(struct handle_table *t, uint32_t oldcap)
{
    uint32_t cap = oldcap ? oldcap * 2 : 16;
    if (cap > HANDLE_TABLE_MAX)
        cap = HANDLE_TABLE_MAX;
    if (cap == oldcap)
        return ERR_NO_RESOURCES;
    struct handle_slot *slots = kzalloc(sizeof(*slots) * cap);
    if (!slots)
        return ERR_NO_MEMORY;
    uint64_t f = spin_lock_irqsave(&t->lock);
    if (t->capacity != oldcap || t->free_head) {
        spin_unlock_irqrestore(&t->lock, f);   /* raced another grow/free */
        kfree(slots);
        return OK;
    }
    if (t->slots)
        memcpy(slots, t->slots, sizeof(*slots) * oldcap);
    for (uint32_t i = oldcap; i < cap; i++)
        slots[i].next_free = i + 1 < cap ? i + 2 : 0;
    struct handle_slot *old = t->slots;
    t->slots = slots;
    t->capacity = cap;
    t->free_head = oldcap + 1;
    t->free_tail = cap;
    spin_unlock_irqrestore(&t->lock, f);
    kfree(old);
    return OK;
}

/* With t->lock held and a free slot guaranteed available (free_head set), or
 * grows under the lock via ensure_free_locked for the rare-path callers. */
static status_t insert_locked(struct handle_table *t, struct kobject *obj, rights_t rights,
                              handle_t *out)
{
    status_t st = ensure_free_locked(t);
    if (st != OK)
        return st;
    uint32_t idx = t->free_head - 1;
    struct handle_slot *s = &t->slots[idx];
    t->free_head = s->next_free;
    if (!t->free_head)
        t->free_tail = 0;
    s->obj = obj;
    s->rights = rights;
    s->intransit = false;
    t->used++;
    *out = encode(idx, s->gen);
    return OK;
}

/* Free the slot (append to the FIFO free list); returns what was in it.
 * Bumping the generation makes old copies of this handle value stale. */
static struct khandle free_slot_locked(struct handle_table *t, struct handle_slot *s)
{
    struct khandle kh = { s->obj, s->rights };
    s->obj = NULL;
    s->gen++;
    s->intransit = false;
    freelist_push(t, (uint32_t)(s - t->slots));
    t->used--;
    return kh;
}

/* Reserve the slot for an in-flight send: empty it and bump the generation
 * (so the old handle value is stale), but do NOT return it to the free list.
 * It stays reserved until handle_untake restores it (failed send) or
 * handle_commit frees it (successful send), so a failed send can always put
 * the handle back and never has to fall back to a fresh insert that could
 * fail and lose it. */
static struct khandle reserve_slot_locked(struct handle_table *t, struct handle_slot *s)
{
    struct khandle kh = { s->obj, s->rights };
    s->obj = NULL;
    s->gen++;
    s->intransit = true;
    t->used--;
    return kh;
}

status_t handle_insert(struct handle_table *t, struct khandle *kh, handle_t *out)
{
    if (!kh->obj)
        return ERR_INVALID_ARGS;
    status_t cst = charge(t, 1);
    if (cst != OK)
        return cst;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&t->lock);
        if (t->free_head) {
            uint32_t idx = t->free_head - 1;
            struct handle_slot *s = &t->slots[idx];
            t->free_head = s->next_free;
            if (!t->free_head)
                t->free_tail = 0;
            s->obj = kh->obj;
            s->rights = kh->rights;
            s->intransit = false;
            t->used++;
            t->charged++;
            *out = encode(idx, s->gen);
            spin_unlock_irqrestore(&t->lock, f);
            kh->obj = NULL;   /* consumed */
            return OK;
        }
        uint32_t oldcap = t->capacity;
        spin_unlock_irqrestore(&t->lock, f);
        status_t st = grow_table(t, oldcap);   /* allocate outside the lock */
        if (st != OK) {
            uncharge(t, 1);
            return st;
        }
    }
}

static status_t check(const struct handle_slot *s, enum obj_type type, rights_t need)
{
    if (!s)
        return ERR_BAD_HANDLE;
    if (type != OBJ_NONE && s->obj->type != type)
        return ERR_WRONG_TYPE;
    if ((s->rights & need) != need)
        return ERR_ACCESS_DENIED;
    return OK;
}

status_t handle_get(struct handle_table *t, handle_t h, enum obj_type type, rights_t need,
                    struct kobject **obj, rights_t *rights)
{
    uint64_t f = spin_lock_irqsave(&t->lock);
    struct handle_slot *s = decode(t, h);
    status_t st = check(s, type, need);
    if (st == OK) {
        kobject_ref(s->obj);
        *obj = s->obj;
        if (rights)
            *rights = s->rights;
    }
    spin_unlock_irqrestore(&t->lock, f);
    return st;
}

status_t handle_remove(struct handle_table *t, handle_t h, struct khandle *out)
{
    uint64_t f = spin_lock_irqsave(&t->lock);
    struct handle_slot *s = decode(t, h);
    if (!s) {
        spin_unlock_irqrestore(&t->lock, f);
        return ERR_BAD_HANDLE;
    }
    *out = free_slot_locked(t, s);
    t->charged--;
    uncharge(t, 1);
    spin_unlock_irqrestore(&t->lock, f);
    return OK;
}

status_t handle_close(struct handle_table *t, handle_t h)
{
    struct khandle kh;
    status_t st = handle_remove(t, h, &kh);
    if (st == OK)
        khandle_release(&kh);   /* outside the lock: may run on_zero_handles */
    return st;
}

/* t->lock held, s checked: a new handle to s's object with `rights` (a
 * subset of s's, or RIGHT_SAME). */
static status_t duplicate_locked(struct handle_table *t, const struct handle_slot *s,
                                 rights_t rights, handle_t *out)
{
    rights_t r = rights == RIGHT_SAME ? s->rights : rights;
    if ((r & s->rights) != r)
        return ERR_INVALID_ARGS;   /* can't gain rights */
    struct kobject *obj = s->obj;
    status_t st = charge(t, 1);
    if (st != OK)
        return st;
    st = insert_locked(t, obj, r, out);   /* may move slots: s is stale now */
    if (st != OK) {
        uncharge(t, 1);
        return st;
    }
    t->charged++;
    kobject_ref(obj);
    kobject_handle_gain(obj);
    return OK;
}

status_t handle_duplicate(struct handle_table *t, handle_t h, rights_t rights, handle_t *out)
{
    uint64_t f = spin_lock_irqsave(&t->lock);
    struct handle_slot *s = decode(t, h);
    status_t st = check(s, OBJ_NONE, RIGHT_DUPLICATE);
    if (st == OK)
        st = duplicate_locked(t, s, rights, out);
    spin_unlock_irqrestore(&t->lock, f);
    return st;
}

status_t handle_take(struct handle_table *t, handle_t h, struct khandle *out)
{
    uint64_t f = spin_lock_irqsave(&t->lock);
    struct handle_slot *s = decode(t, h);
    status_t st = check(s, OBJ_NONE, RIGHT_TRANSFER);
    if (st == OK)
        *out = reserve_slot_locked(t, s);   /* reserved until commit/untake */
    spin_unlock_irqrestore(&t->lock, f);
    return st;
}

status_t handle_replace(struct handle_table *t, handle_t h, rights_t rights, handle_t *out)
{
    uint64_t f = spin_lock_irqsave(&t->lock);
    struct handle_slot *s = decode(t, h);
    status_t st = s ? OK : ERR_BAD_HANDLE;
    if (st == OK) {
        rights_t r = rights == RIGHT_SAME ? s->rights : rights;
        if ((r & s->rights) != r) {
            st = ERR_INVALID_ARGS;
        } else {
            /* One slot out, one in: the charge stays as it is. */
            struct khandle kh = free_slot_locked(t, s);
            st = insert_locked(t, kh.obj, r, out);   /* free_slot left one free */
        }
    }
    spin_unlock_irqrestore(&t->lock, f);
    return st;
}

#ifndef JAM_NO_KTESTS
uint32_t handle_table_rights(struct handle_table *t, enum obj_type type, rights_t *out,
                             uint32_t cap)
{
    uint32_t n = 0;
    uint64_t f = spin_lock_irqsave(&t->lock);
    for (uint32_t i = 0; i < t->capacity; i++) {
        struct handle_slot *s = &t->slots[i];
        if (s->obj && s->obj->type == type) {
            if (n < cap)
                out[n] = s->rights;
            n++;
        }
    }
    spin_unlock_irqrestore(&t->lock, f);
    return n;
}

status_t handle_table_find(struct handle_table *t, enum obj_type type, struct kobject **out)
{
    struct kobject *o = NULL;
    uint64_t f = spin_lock_irqsave(&t->lock);
    for (uint32_t i = 0; i < t->capacity && !o; i++)
        if (t->slots[i].obj && t->slots[i].obj->type == type)
            o = t->slots[i].obj;
    if (o)
        kobject_ref(o);   /* the handle's reference keeps it alive until here */
    spin_unlock_irqrestore(&t->lock, f);
    if (!o)
        return ERR_NOT_FOUND;
    *out = o;
    return OK;
}
#endif

/* The reserved slot for an in-transit handle h, if h names one that was taken
 * and not yet committed or restored. */
static struct handle_slot *intransit_slot(const struct handle_table *t, handle_t h)
{
    uint32_t idx = h >> GEN_BITS;
    if (!idx || idx > t->capacity)
        return NULL;
    struct handle_slot *s = &t->slots[idx - 1];
    /* reserve_slot_locked bumped the generation once and left the slot empty
     * and reserved; nothing can have reused it (it never went on the free
     * list). */
    if (s->intransit && !s->obj && ((s->gen - 1) & GEN_MASK) == (h & GEN_MASK))
        return s;
    return NULL;
}

status_t handle_untake(struct handle_table *t, handle_t h, struct khandle *kh, handle_t *out)
{
    if (!kh->obj)
        return ERR_INVALID_ARGS;
    uint64_t f = spin_lock_irqsave(&t->lock);
    struct handle_slot *s = intransit_slot(t, h);
    /* The slot was reserved by handle_take and held for us, so putting the
     * handle back is O(1) and can never fail: a failed send never loses a
     * handle. */
    status_t st;
    if (s) {
        s->gen--;             /* restore the original handle value */
        s->obj = kh->obj;
        s->rights = kh->rights;
        s->intransit = false;
        t->used++;
        *out = h;
        st = OK;
        kh->obj = NULL;
    } else {
        st = ERR_INTERNAL;    /* h was not a live in-transit handle */
    }
    spin_unlock_irqrestore(&t->lock, f);
    return st;
}

status_t handle_reserve(struct handle_table *t, uint32_t n, handle_t *out)
{
    uint32_t got = 0;
    status_t cst = charge(t, n);   /* each reserved slot costs a unit until committed */
    if (cst != OK)
        return cst;
    while (got < n) {
        uint64_t f = spin_lock_irqsave(&t->lock);
        while (got < n && t->free_head) {
            uint32_t idx = t->free_head - 1;
            struct handle_slot *s = &t->slots[idx];
            t->free_head = s->next_free;
            if (!t->free_head)
                t->free_tail = 0;
            /* Looks exactly like a slot handle_take reserved for this value,
             * so handle_untake fills it and handle_commit frees it. */
            out[got++] = encode(idx, s->gen);
            s->gen++;
            s->intransit = true;
            t->charged++;
        }
        uint32_t oldcap = t->capacity;
        spin_unlock_irqrestore(&t->lock, f);
        if (got == n)
            break;
        status_t st = grow_table(t, oldcap);   /* free list was empty */
        if (st != OK) {
            uncharge(t, n - got);   /* the ones never reserved */
            while (got)
                handle_commit(t, out[--got]);   /* credits one each */
            return st;
        }
    }
    return OK;
}

status_t handle_commit(struct handle_table *t, handle_t h)
{
    uint64_t f = spin_lock_irqsave(&t->lock);
    struct handle_slot *s = intransit_slot(t, h);
    status_t st = ERR_INTERNAL;
    if (s) {
        s->intransit = false;
        freelist_push(t, (h >> GEN_BITS) - 1);   /* the send took it for good */
        t->charged--;
        uncharge(t, 1);
        st = OK;
    }
    spin_unlock_irqrestore(&t->lock, f);
    return st;
}
