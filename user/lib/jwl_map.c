/* libjwl's object map (<jwl.h>): a connection's ids, on either side.
 *
 * Two ranges, each a growing array indexed from its first id: the
 * client's ids from 1 (index = id - 1; wl_display is entry 0) and the
 * compositor's from JWL_SERVER_ID_BASE. "Our own" range is the one this
 * side makes ids in; the other is the peer's, whose ids arrive in new_id
 * arguments and are only checked and entered here.
 *
 * Wayland's id rules, as kept here:
 *   - a peer's new id is the next one (the range's length) or the id of an
 *     entry that is free again; anything else is a protocol error, so a
 *     range never grows faster than the peer's real objects;
 *   - our own new ids come from a free list first (the ids freed most
 *     recently, as libwayland's allocator), so the peer sees the same;
 *     each is pending until the message that names it is sent, and one
 *     destroyed while pending is free at once (the peer never knew it);
 *   - a client's id, once destroyed by the client, is a zombie until the
 *     compositor's delete_id, and only then free (the compositor frees it
 *     at once and says delete_id); a compositor's id is free at once on
 *     the compositor and a zombie on the client until it is made again.
 *
 * Each range holds at most JWL_OBJECTS_MAX entries, so a connection's map
 * is at most 2 * 4096 * 32 bytes, whatever the peer does. */
#include <jwl.h>
#include <os.h>

static struct jwl_range *own(struct jwl_map *m)
{
    return &m->r[m->side == JWL_CLIENT ? 0 : 1];
}

/* Which range id is in and its index there; false for 0 and for the ids
 * between the ranges' caps and the next range. */
static bool locate(uint32_t id, unsigned *range, uint32_t *idx)
{
    if (id >= 1 && id <= JWL_OBJECTS_MAX) {
        *range = 0;
        *idx = id - 1;
        return true;
    }
    if (id >= JWL_SERVER_ID_BASE && id - JWL_SERVER_ID_BASE < JWL_OBJECTS_MAX) {
        *range = 1;
        *idx = id - JWL_SERVER_ID_BASE;
        return true;
    }
    return false;
}

static uint32_t id_of(unsigned range, uint32_t idx)
{
    return range == 0 ? idx + 1 : JWL_SERVER_ID_BASE + idx;
}

/* Room for one more entry at r->n. ERR_NO_RESOURCES at the cap. */
static status_t grow(struct jwl_range *r)
{
    if (r->n < r->cap)
        return OK;
    if (r->cap >= JWL_OBJECTS_MAX)
        return ERR_NO_RESOURCES;
    uint32_t cap = r->cap ? r->cap * 2 : 16;
    if (cap > JWL_OBJECTS_MAX)
        cap = JWL_OBJECTS_MAX;
    struct jwl_object *v = calloc(cap, sizeof(*v));
    if (!v)
        return ERR_NO_MEMORY;
    if (r->n)
        memcpy(v, r->v, r->n * sizeof(*v));
    free(r->v);
    r->v = v;
    r->cap = cap;
    return OK;
}

static void set_entry(struct jwl_object *o, const struct jwl_interface *iface, uint32_t version,
                      enum jwl_state state)
{
    memset(o, 0, sizeof(*o));
    o->iface = iface;
    o->version = version;
    o->state = (uint8_t)state;
}

/* An entry of our own range becomes free and goes on the free list. */
static void free_own(struct jwl_range *r, uint32_t idx)
{
    struct jwl_object *o = &r->v[idx];
    memset(o, 0, sizeof(*o));
    o->next_free = r->free_head;
    r->free_head = idx + 1;
}

status_t jwl_map_init(struct jwl_map *m, enum jwl_side side, const struct jwl_interface *display,
                      const struct jwl_interface *const *known, unsigned nknown)
{
    memset(m, 0, sizeof(*m));
    m->side = side;
    m->known = known;
    m->nknown = nknown;
    status_t st = grow(&m->r[0]);
    if (st != OK)
        return st;
    set_entry(&m->r[0].v[0], display, 1, JWL_LIVE);
    m->r[0].n = 1;
    m->live = 1;
    return OK;
}

void jwl_map_free(struct jwl_map *m)
{
    free(m->r[0].v);
    free(m->r[1].v);
    memset(m, 0, sizeof(*m));
}

struct jwl_object *jwl_map_entry(const struct jwl_map *m, uint32_t id)
{
    unsigned range;
    uint32_t idx;
    if (!locate(id, &range, &idx) || idx >= m->r[range].n)
        return NULL;
    return &m->r[range].v[idx];
}

struct jwl_object *jwl_map_get(const struct jwl_map *m, uint32_t id)
{
    struct jwl_object *o = jwl_map_entry(m, id);
    return o && o->state == JWL_LIVE ? o : NULL;
}

status_t jwl_map_new(struct jwl_map *m, const struct jwl_interface *iface, uint32_t version,
                     void *data, uint32_t *out)
{
    struct jwl_range *r = own(m);
    uint32_t idx;
    if (r->free_head) {
        idx = r->free_head - 1;
        r->free_head = r->v[idx].next_free;
    } else {
        status_t st = grow(r);
        if (st != OK)
            return st;
        idx = r->n++;
    }
    set_entry(&r->v[idx], iface, version, JWL_LIVE);
    r->v[idx].data = data;
    r->v[idx].pending = true;
    m->live++;
    *out = id_of(m->side == JWL_CLIENT ? 0 : 1, idx);
    return OK;
}

status_t jwl_map_check_new(const struct jwl_map *m, uint32_t id)
{
    unsigned peer = m->side == JWL_CLIENT ? 1 : 0, range;
    uint32_t idx;
    if (!locate(id, &range, &idx)) {
        /* past the cap but inside the peer's range: a full map, not a lie */
        bool in_peer = peer == 0 ? id >= 1 && id <= JWL_CLIENT_ID_MAX : id >= JWL_SERVER_ID_BASE;
        return in_peer && m->r[peer].n == JWL_OBJECTS_MAX ? ERR_NO_RESOURCES : ERR_INVALID_ARGS;
    }
    if (range != peer)
        return ERR_INVALID_ARGS;
    const struct jwl_range *r = &m->r[peer];
    if (idx < r->n) {
        uint8_t state = r->v[idx].state;
        /* a compositor id the client destroyed is a zombie until reused */
        bool reusable = state == JWL_FREE || (m->side == JWL_CLIENT && state == JWL_ZOMBIE);
        return reusable ? OK : ERR_INVALID_ARGS;
    }
    return idx == r->n ? OK : ERR_INVALID_ARGS;
}

status_t jwl_map_insert(struct jwl_map *m, uint32_t id, const struct jwl_interface *iface,
                        uint32_t version, enum jwl_state state)
{
    unsigned range;
    uint32_t idx;
    if (!locate(id, &range, &idx))
        return ERR_INVALID_ARGS;
    struct jwl_range *r = &m->r[range];
    if (idx > r->n)
        return ERR_INVALID_ARGS;
    if (idx == r->n) {
        status_t st = grow(r);
        if (st != OK)
            return st;
        r->n++;
    } else if (r->v[idx].state == JWL_LIVE) {
        return ERR_INVALID_ARGS;
    }
    set_entry(&r->v[idx], iface, version, state);
    if (state == JWL_LIVE)
        m->live++;
    return OK;
}

status_t jwl_map_remove(struct jwl_map *m, uint32_t id)
{
    if (id == JWL_DISPLAY_ID)
        return ERR_INVALID_ARGS;
    struct jwl_object *o = jwl_map_get(m, id);
    if (!o)
        return ERR_NOT_FOUND;
    unsigned range;
    uint32_t idx;
    (void)locate(id, &range, &idx);   /* true: jwl_map_get found it */
    m->live--;
    bool ours = range == (m->side == JWL_CLIENT ? 0u : 1u);
    if (o->pending) {
        free_own(&m->r[range], idx);   /* never announced: nobody else knows it */
    } else if (m->side == JWL_SERVER) {
        /* our ids free at once; a client's is free now and delete_id follows */
        if (ours)
            free_own(&m->r[range], idx);
        else
            set_entry(o, NULL, 0, JWL_FREE);
    } else if (ours && o->deleted) {
        free_own(&m->r[range], idx);
    } else {
        /* events may still be on their way to it */
        o->state = JWL_ZOMBIE;
        o->data = NULL;
    }
    return OK;
}

status_t jwl_map_delete_id(struct jwl_map *m, uint32_t id)
{
    if (m->side != JWL_CLIENT || id == JWL_DISPLAY_ID || id > JWL_OBJECTS_MAX)
        return ERR_INVALID_ARGS;
    struct jwl_object *o = jwl_map_entry(m, id);
    if (!o || o->state == JWL_FREE || o->deleted || o->pending)
        return ERR_INVALID_ARGS;
    if (o->state == JWL_LIVE)
        o->deleted = true;   /* free when we destroy it */
    else
        free_own(&m->r[0], id - 1);
    return OK;
}

status_t jwl_map_set_data(struct jwl_map *m, uint32_t id, void *data)
{
    struct jwl_object *o = jwl_map_get(m, id);
    if (!o)
        return ERR_NOT_FOUND;
    o->data = data;
    return OK;
}
