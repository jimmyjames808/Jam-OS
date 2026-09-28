#include <jam/handle.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/string.h>

struct handle_slot {
    struct kobject *obj;        /* NULL: free */
    rights_t        rights;
    uint32_t        gen;
    uint32_t        next_free;  /* slot index + 1, 0 = end */
};

#define GEN_BITS 8
#define GEN_MASK ((1u << GEN_BITS) - 1)

static handle_t encode(uint32_t slot, uint32_t gen)
{
    return (slot + 1) << GEN_BITS | (gen & GEN_MASK);
}

/* Returns the slot for h if it is live and current, else NULL. */
static struct handle_slot *decode(struct handle_table *t, handle_t h)
{
    uint32_t idx = h >> GEN_BITS;
    if (idx == 0 || idx > t->capacity)
        return NULL;
    struct handle_slot *s = &t->slots[idx - 1];
    if (!s->obj || (s->gen & GEN_MASK) != (h & GEN_MASK))
        return NULL;
    return s;
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
    t->capacity = t->used = t->free_head = 0;
}

void handle_table_destroy(struct handle_table *t)
{
    for (;;) {
        struct khandle kh = { 0 };
        uint64_t f = spin_lock_irqsave(&t->lock);
        for (uint32_t i = 0; i < t->capacity && !kh.obj; i++) {
            if (t->slots[i].obj) {
                kh.obj = t->slots[i].obj;
                kh.rights = t->slots[i].rights;
                t->slots[i].obj = NULL;
                t->used--;
            }
        }
        spin_unlock_irqrestore(&t->lock, f);
        if (!kh.obj)
            break;
        khandle_release(&kh);   /* outside the lock: may run on_zero_handles */
    }
    kfree(t->slots);
    t->slots = NULL;
    t->capacity = t->free_head = 0;
}

/* With t->lock held: make sure there is a free slot. */
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
    t->slots = slots;
    t->capacity = cap;
    return OK;
}

static status_t insert_locked(struct handle_table *t, struct kobject *obj, rights_t rights,
                              handle_t *out)
{
    status_t st = ensure_free_locked(t);
    if (st != OK)
        return st;
    uint32_t idx = t->free_head - 1;
    struct handle_slot *s = &t->slots[idx];
    t->free_head = s->next_free;
    s->obj = obj;
    s->rights = rights;
    t->used++;
    *out = encode(idx, s->gen);
    return OK;
}

/* Free the slot; returns what was in it. Bumping the generation makes old
 * copies of this handle value stale. */
static struct khandle remove_locked(struct handle_table *t, struct handle_slot *s)
{
    struct khandle kh = { s->obj, s->rights };
    s->obj = NULL;
    s->gen++;
    s->next_free = t->free_head;
    t->free_head = (uint32_t)(s - t->slots) + 1;
    t->used--;
    return kh;
}

status_t handle_insert(struct handle_table *t, struct khandle *kh, handle_t *out)
{
    if (!kh->obj)
        return ERR_INVALID_ARGS;
    uint64_t f = spin_lock_irqsave(&t->lock);
    status_t st = insert_locked(t, kh->obj, kh->rights, out);
    spin_unlock_irqrestore(&t->lock, f);
    if (st == OK)
        kh->obj = NULL;   /* consumed */
    return st;
}

static status_t check(struct handle_slot *s, enum obj_type type, rights_t need)
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

status_t handle_close(struct handle_table *t, handle_t h)
{
    uint64_t f = spin_lock_irqsave(&t->lock);
    struct handle_slot *s = decode(t, h);
    if (!s) {
        spin_unlock_irqrestore(&t->lock, f);
        return ERR_BAD_HANDLE;
    }
    struct khandle kh = remove_locked(t, s);
    spin_unlock_irqrestore(&t->lock, f);
    khandle_release(&kh);
    return OK;
}

status_t handle_duplicate(struct handle_table *t, handle_t h, rights_t rights, handle_t *out)
{
    uint64_t f = spin_lock_irqsave(&t->lock);
    struct handle_slot *s = decode(t, h);
    status_t st = check(s, OBJ_NONE, RIGHT_DUPLICATE);
    if (st == OK) {
        rights_t r = rights == RIGHT_SAME ? s->rights : rights;
        if ((r & s->rights) != r) {
            st = ERR_INVALID_ARGS;   /* can't gain rights */
        } else {
            struct kobject *obj = s->obj;
            st = insert_locked(t, obj, r, out);   /* may move slots: s is stale now */
            if (st == OK) {
                kobject_ref(obj);
                kobject_handle_gain(obj);
            }
        }
    }
    spin_unlock_irqrestore(&t->lock, f);
    return st;
}

status_t handle_take(struct handle_table *t, handle_t h, struct khandle *out)
{
    uint64_t f = spin_lock_irqsave(&t->lock);
    struct handle_slot *s = decode(t, h);
    status_t st = check(s, OBJ_NONE, RIGHT_TRANSFER);
    if (st == OK)
        *out = remove_locked(t, s);
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
            struct khandle kh = remove_locked(t, s);
            st = insert_locked(t, kh.obj, r, out);   /* reuses the slot just freed */
        }
    }
    spin_unlock_irqrestore(&t->lock, f);
    return st;
}

status_t handle_untake(struct handle_table *t, handle_t h, struct khandle *kh, handle_t *out)
{
    if (!kh->obj)
        return ERR_INVALID_ARGS;
    uint64_t f = spin_lock_irqsave(&t->lock);
    status_t st;
    uint32_t idx = h >> GEN_BITS;
    struct handle_slot *s = idx && idx <= t->capacity ? &t->slots[idx - 1] : NULL;
    /* remove_locked bumped the generation once; nothing reused it since. */
    if (s && !s->obj && ((s->gen - 1) & GEN_MASK) == (h & GEN_MASK)) {
        uint32_t *link = &t->free_head;
        while (*link && *link != idx)
            link = &t->slots[*link - 1].next_free;
        if (*link == idx) {
            *link = s->next_free;
            s->gen--;
            s->obj = kh->obj;
            s->rights = kh->rights;
            t->used++;
            *out = h;
            st = OK;
            goto done;
        }
    }
    st = insert_locked(t, kh->obj, kh->rights, out);
done:
    spin_unlock_irqrestore(&t->lock, f);
    if (st == OK)
        kh->obj = NULL;
    return st;
}
