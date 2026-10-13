/* wl_subcompositor and wl_subsurface (comp.h): subsurface trees.
 *
 * A surface given the subsurface role is drawn in its parent's tree, never
 * as a window of its own: a main surface (a toplevel's, or a test
 * window's) and every subsurface under it are one window, placed, framed,
 * hit and stacked as one (scene.c walks the tree; paint.c draws it). Each
 * parent keeps its children in a struct comp_tree: the order drawn (cur)
 * and the order its next commit applies (pend), itself among them, so
 * place_above and place_below may name it.
 *
 * Double-buffered on the parent, as the protocol says: a new child's place
 * in the order, set_position, place_above and place_below take effect when
 * the parent's state is next applied (subsurface_applied, from surface.c).
 * A synchronized child's commits are cached (surface.c) and applied with
 * its parent's state; a desynchronized one's apply at once, unless a
 * parent above it is synchronized (subsurface_synced: the protocol's
 * "effectively synchronized"). set_desync applies a cache left waiting
 * at once, as other compositors do. A wl_subsurface's destroy takes its
 * surface out of the tree at once; its cache is dropped.
 *
 * Lifetimes. A child's wl_surface destroyed before its wl_subsurface is
 * the protocol's defunct_role_object error, as xdg-shell's roles are here.
 * A parent's wl_surface destroyed first leaves its children with no parent:
 * shown nowhere, their wl_subsurface objects inert (requests on them do
 * nothing) until destroyed. The role stays the surface's for its life (a
 * new wl_subsurface may be made for it, never another role).
 *
 * Caps, each refused with no_memory as the compositor's other caps are:
 * COMP_SUBS_MAX wl_subsurface objects a client, COMP_SUB_CHILDREN_MAX
 * children a parent, COMP_SUB_DEPTH_MAX subsurfaces nested below a main
 * surface (a subtree brought under a parent counts whole). Positions are
 * held to +-COMP_SUB_POS_MAX, so a tree's place on the output never
 * overflows. Cycles (a surface under itself) are bad_parent. */
#include <jwl/wayland.h>
#include "comp.h"

/* ---- the trees ------------------------------------------------------------------------- */

/* s's index in list (n entries), or -1. */
static int index_in(struct comp_surface *const *list, uint32_t n, const struct comp_surface *s)
{
    for (uint32_t i = 0; i < n; i++)
        if (list[i] == s)
            return (int)i;
    return -1;
}

static void list_remove(struct comp_surface **list, uint32_t *n, const struct comp_surface *s)
{
    int i = index_in(list, *n, s);
    if (i < 0)
        return;
    memmove(list + i, list + i + 1, (*n - (uint32_t)i - 1) * sizeof(list[0]));
    (*n)--;
}

/* s into list at index `at` (room checked by the caller: the children cap). */
static void list_insert(struct comp_surface **list, uint32_t *n, uint32_t at, struct comp_surface *s)
{
    memmove(list + at + 1, list + at, (*n - at) * sizeof(list[0]));
    list[at] = s;
    (*n)++;
}

/* parent's tree, made with only itself in it the first time; NULL: no memory. */
static struct comp_tree *tree_of(struct comp_surface *parent)
{
    if (!parent->tree) {
        struct comp_tree *t = calloc(1, sizeof(*t));
        if (!t)
            return NULL;
        t->cur[0] = t->pend[0] = parent;
        t->ncur = t->npend = 1;
        parent->tree = t;
    }
    return parent->tree;
}

/* sub's surface out of its parent's tree at once, what it drew damaged. */
static void leave_parent(struct comp_sub *sub)
{
    struct comp_surface *p = sub->parent;
    if (!p)
        return;
    struct comp_window *w = surface_tree_window(p);
    if (w && sub->added)
        window_damage(w);   /* (the frame may shrink too: all of it, before) */
    struct comp_tree *t = p->tree;
    list_remove(t->pend, &t->npend, sub->surface);
    list_remove(t->cur, &t->ncur, sub->surface);
    sub->parent = NULL;
    sub->added = false;
    if (t->npend == 1 && t->ncur == 1) {
        free(t);
        p->tree = NULL;
    }
}

static void sub_free(struct comp_sub *sub)
{
    struct comp_client *cl = sub->client;
    for (struct comp_sub **p = &cl->subs; *p; p = &(*p)->next) {
        if (*p == sub) {
            *p = sub->next;
            break;
        }
    }
    cl->nsubs--;
    free(sub);
}

void subsurface_parent_gone(struct comp_surface *s)
{
    struct comp_tree *t = s->tree;
    if (!t)
        return;
    struct comp_window *w = surface_tree_window(s);
    if (w)
        window_damage(w);
    for (uint32_t i = 0; i < t->npend; i++) {
        struct comp_surface *c = t->pend[i];
        if (c != s) {
            c->sub->parent = NULL;
            c->sub->added = false;
        }
    }
    free(t);
    s->tree = NULL;
}

void subsurfaces_teardown(struct comp_client *cl)
{
    while (cl->subs)
        sub_free(cl->subs);
}

bool subsurface_synced(const struct comp_surface *s)
{
    for (unsigned d = 0; d <= COMP_SUB_DEPTH_MAX && s->sub && s->sub->parent; d++) {
        if (s->sub->sync)
            return true;
        s = s->sub->parent;
    }
    return false;
}

status_t subsurface_applied(struct comp_surface *s, bool parent_sync, bool *reshaped)
{
    struct comp_tree *t = s->tree;
    if (!t)
        return OK;
    if (t->pend_changed) {
        if (t->ncur != t->npend || memcmp(t->cur, t->pend, t->npend * sizeof(t->cur[0])))
            *reshaped = true;
        memcpy(t->cur, t->pend, t->npend * sizeof(t->cur[0]));
        t->ncur = t->npend;
        t->pend_changed = false;
        for (uint32_t i = 0; i < t->ncur; i++)
            if (t->cur[i] != s)
                t->cur[i]->sub->added = true;
    }
    for (uint32_t i = 0; i < t->ncur; i++) {
        struct comp_surface *c = t->cur[i];
        if (c == s)
            continue;
        struct comp_sub *sub = c->sub;
        if (sub->pos_set) {
            *reshaped |= sub->x != sub->pend_x || sub->y != sub->pend_y;
            sub->x = sub->pend_x;
            sub->y = sub->pend_y;
            sub->pos_set = false;
        }
        if (c->has_cache && (sub->sync || parent_sync)) {
            status_t st = surface_apply_cached(c);   /* and its children, as synchronized */
            if (st != OK)
                return st;
        }
    }
    return OK;
}

/* ---- the role -------------------------------------------------------------------------- */

/* The wl_surface went with its wl_subsurface alive (defunct_role_object),
 * or its client went (dead: nothing sent). */
static void sub_gone(struct comp_surface *s, bool dead)
{
    struct comp_sub *sub = s->sub;
    if (!sub)
        return;   /* its wl_subsurface went first, as it should */
    if (!dead)
        (void)comp_error(s->client, s->id, JWL_WL_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                         "wl_surface %u destroyed before its wl_subsurface", s->id);
    leave_parent(sub);
    sub->surface = NULL;
    s->sub = NULL;
}

/* No commit hook: what a commit does for a subsurface is surface.c's
 * (cached or applied) and the tree's (its parent's apply). */
static const struct comp_role_ops sub_role = { "wl_subsurface", NULL, sub_gone };

/* A new subsurface state for child under parent, on top of its pending
 * order; NULL: no memory. */
static struct comp_sub *sub_make(struct comp_surface *child, struct comp_surface *parent,
                                 uint32_t id)
{
    struct comp_client *cl = child->client;
    struct comp_sub *sub = calloc(1, sizeof(*sub));
    struct comp_tree *t = sub ? tree_of(parent) : NULL;
    if (!t) {
        free(sub);
        return NULL;
    }
    sub->client = cl;
    sub->id = id;
    sub->surface = child;
    sub->parent = parent;
    sub->sync = true;   /* the protocol's default */
    sub->next = cl->subs;
    cl->subs = sub;
    cl->nsubs++;
    child->role = COMP_ROLE_SUBSURFACE;
    child->role_ops = &sub_role;
    child->role_data = sub;
    child->sub = sub;
    list_insert(t->pend, &t->npend, t->npend, child);
    t->pend_changed = true;
    return sub;
}

/* ---- wl_subcompositor ------------------------------------------------------------------ */

status_t subcompositor_bind(struct comp_client *cl, uint32_t id, uint32_t version)
{
    (void)version;
    return jwl_map_set_data(&cl->conn->map, id, cl);
}

static status_t on_subcompositor_destroy(void *data, uint32_t self)
{
    (void)data, (void)self;
    return OK;   /* its subsurfaces stay, as the protocol says */
}

/* The generations of children under s (0: none). */
static unsigned height_of(const struct comp_surface *s, unsigned depth)
{
    const struct comp_tree *t = s->tree;
    unsigned h = 0;
    if (!t || depth > COMP_SUB_DEPTH_MAX)
        return 0;
    for (uint32_t i = 0; i < t->npend; i++) {
        unsigned c = t->pend[i] == s ? 0 : 1 + height_of(t->pend[i], depth + 1);
        h = c > h ? c : h;
    }
    return h;
}

/* The subsurface links from s up to the top of its tree; *cycle set if
 * self is s or one of the parents on the way. */
static unsigned depth_of(const struct comp_surface *s, const struct comp_surface *self,
                         bool *cycle)
{
    unsigned d = 0;
    for (; d <= COMP_SUB_DEPTH_MAX; d++) {
        if (s == self)
            *cycle = true;
        if (!s->sub || !s->sub->parent)
            break;
        s = s->sub->parent;
    }
    return d;
}

static status_t on_get_subsurface(void *data, uint32_t self, uint32_t id, uint32_t surface,
                                  uint32_t parent)
{
    struct comp_client *cl = data;
    struct comp_surface *s = comp_object(cl, surface, &jwl_wl_surface_interface);
    struct comp_surface *p = comp_object(cl, parent, &jwl_wl_surface_interface);
    bool free_to = s && !s->sub &&
                   ((s->role == COMP_ROLE_NONE && !s->role_ops) || s->role == COMP_ROLE_SUBSURFACE);
    if (!free_to)   /* (a role, even one whose object went, is for good) */
        return comp_error(cl, self, JWL_WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                          "wl_surface %u has another role or a wl_subsurface", surface);
    bool cycle = false;
    unsigned depth = p ? depth_of(p, s, &cycle) : 0;
    if (!p || cycle)
        return comp_error(cl, self, JWL_WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
                          "wl_surface %u can't be %u's parent: it is %u or under it", parent,
                          surface, surface);
    if (cl->nsubs >= COMP_SUBS_MAX)
        return comp_no_memory(cl, self, "too many subsurfaces (48)");
    if (p->tree && p->tree->npend > COMP_SUB_CHILDREN_MAX)
        return comp_no_memory(cl, self, "a surface with 32 subsurfaces already");
    if (depth + 1 + height_of(s, 0) > COMP_SUB_DEPTH_MAX)
        return comp_no_memory(cl, self, "subsurfaces nested deeper than 8");
    struct comp_sub *sub = sub_make(s, p, id);
    if (!sub)
        return comp_no_memory(cl, self, "no memory for a subsurface");
    return jwl_map_set_data(&cl->conn->map, id, sub);
}

static const struct jwl_wl_subcompositor_requests subcompositor_ops = {
    .destroy = on_subcompositor_destroy,
    .get_subsurface = on_get_subsurface,
};

status_t subcompositor_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;   /* the object's data is the client */
    return jwl_wl_subcompositor_dispatch_request(&subcompositor_ops, m->data, m->id, m->opcode,
                                                 m->args);
}

/* ---- wl_subsurface ---------------------------------------------------------------------- */

static status_t on_sub_destroy(void *data, uint32_t self)
{
    struct comp_sub *sub = data;
    struct comp_surface *s = sub->surface;
    (void)self;
    if (s) {
        leave_parent(sub);   /* unmapped at once, its place and order forgotten */
        surface_drop_cache(s);
        s->sub = NULL;
        s->role_data = NULL;
    }
    sub_free(sub);   /* libjwl freed the id */
    return OK;
}

static int32_t clamp_pos(int32_t v)
{
    return v < -COMP_SUB_POS_MAX ? -COMP_SUB_POS_MAX : v > COMP_SUB_POS_MAX ? COMP_SUB_POS_MAX : v;
}

static status_t on_set_position(void *data, uint32_t self, int32_t x, int32_t y)
{
    struct comp_sub *sub = data;
    (void)self;
    sub->pend_x = clamp_pos(x);
    sub->pend_y = clamp_pos(y);
    sub->pos_set = true;   /* (inert without a parent: nothing applies it) */
    return OK;
}

/* place_above (above) or place_below the sibling or the parent `ref`. */
static status_t restack(struct comp_sub *sub, uint32_t self, uint32_t ref, bool above)
{
    struct comp_surface *s = sub->surface, *p = sub->parent;
    if (!s || !p)
        return OK;   /* inert: its parent went */
    struct comp_tree *t = p->tree;
    struct comp_surface *r = comp_object(sub->client, ref, &jwl_wl_surface_interface);
    if (!r || r == s || index_in(t->pend, t->npend, r) < 0)
        return comp_error(sub->client, self, JWL_WL_SUBSURFACE_ERROR_BAD_SURFACE,
                          "wl_surface %u is not a sibling or the parent", ref);
    list_remove(t->pend, &t->npend, s);
    int at = index_in(t->pend, t->npend, r);
    list_insert(t->pend, &t->npend, (uint32_t)at + (above ? 1u : 0u), s);
    t->pend_changed = true;
    return OK;
}

static status_t on_place_above(void *data, uint32_t self, uint32_t sibling)
{
    return restack(data, self, sibling, true);
}

static status_t on_place_below(void *data, uint32_t self, uint32_t sibling)
{
    return restack(data, self, sibling, false);
}

static status_t on_set_sync(void *data, uint32_t self)
{
    struct comp_sub *sub = data;
    (void)self;
    sub->sync = true;
    return OK;
}

static status_t on_set_desync(void *data, uint32_t self)
{
    struct comp_sub *sub = data;
    (void)self;
    sub->sync = false;
    struct comp_surface *s = sub->surface;
    if (s && s->has_cache && !subsurface_synced(s))
        return surface_flush_cache(s);   /* what waited for the parent shows now */
    return OK;
}

static const struct jwl_wl_subsurface_requests subsurface_ops = {
    .destroy = on_sub_destroy,
    .set_position = on_set_position,
    .place_above = on_place_above,
    .place_below = on_place_below,
    .set_sync = on_set_sync,
    .set_desync = on_set_desync,
};

status_t subsurface_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;   /* the object's data knows its client */
    return jwl_wl_subsurface_dispatch_request(&subsurface_ops, m->data, m->id, m->opcode,
                                              m->args);
}

/* ---- the test scene's ------------------------------------------------------------------- */

status_t subsurface_add_now(struct comp_surface *child, struct comp_surface *parent, int32_t x,
                            int32_t y, bool below)
{
    if (child->role != COMP_ROLE_NONE || child->role_ops || child == parent)
        return ERR_INVALID_ARGS;
    struct comp_sub *sub = sub_make(child, parent, 0);
    if (!sub)
        return ERR_NO_MEMORY;
    struct comp_tree *t = parent->tree;
    if (below) {   /* from the top of pend to its bottom */
        list_remove(t->pend, &t->npend, child);
        list_insert(t->pend, &t->npend, 0, child);
    }
    sub->pend_x = clamp_pos(x);
    sub->pend_y = clamp_pos(y);
    sub->pos_set = true;
    bool reshaped = false;
    status_t st = subsurface_applied(parent, false, &reshaped);   /* as the parent's commit */
    struct comp_window *w = surface_tree_window(parent);
    if (w)
        window_damage(w);
    return st;
}

void subsurface_remove_now(struct comp_surface *child)
{
    if (child->sub)
        (void)on_sub_destroy(child->sub, 0);
}
