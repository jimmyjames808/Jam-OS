/* libjwl's client: shared-memory pools and buffers (<jwl_client.h>).
 *
 * A pool is a VMO whose pages stay (VMO_KEEP_PAGES): every page is
 * committed when it is made and charged to our job, and no holder can
 * shrink it or take a page away, so the compositor can map it
 * (VMAR_KEPT_ONLY) without ever faulting on it. We map it read-write; the
 * compositor gets a duplicate with RIGHT_READ and RIGHT_MAP only (and the
 * RIGHT_TRANSFER a handle needs to travel), so it can't write it, resize
 * it or pass it on. Growing (wl_shm_pool.resize, "only bigger") is
 * vmo_set_size on our handle, then a new mapping of the whole pool here.
 *
 * Pools and buffers outlive the connection: a lost one zeroes their ids
 * and clears every buffer's busy flag (the compositor that held them is
 * gone), and the next ready connection makes them again with the same
 * VMO, so what was drawn is still there. */
#include <jwl_client.h>
#include <os.h>
#include "jwlc.h"

#define POOL_RIGHTS (RIGHT_READ | RIGHT_MAP | RIGHT_TRANSFER)   /* what the compositor gets */

static uint64_t page_up(uint64_t n)
{
    return (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

/* ---- pools ----------------------------------------------------------------------------- */

status_t jwlc_pool_make(struct jwl_pool *p)
{
    struct jwl_client *c = p->c;
    if (!jwlc_live(c))
        return OK;   /* made when the client is ready */
    handle_t dup;
    status_t st = jam_handle_duplicate(p->vmo, POOL_RIGHTS, &dup);
    if (st != OK)
        return st;
    uint32_t id;
    st = jwlc_make(c, &jwl_wl_shm_pool_interface, 1, p, &id);
    if (st != OK) {
        jam_handle_close(dup);
        return st;
    }
    /* the send consumes dup whatever happens */
    st = jwlc_made(c, jwl_wl_shm_create_pool(c->conn, c->global[JWLC_SHM], id, dup,
                                             (int32_t)p->size), id);
    if (st == OK)
        p->id = id;
    return st;
}

status_t jwl_pool_create(struct jwl_client *c, uint64_t size, struct jwl_pool **out)
{
    if (c->state == JWLC_DEAD)
        return c->why;
    if (!size || size > (INT32_MAX & ~(PAGE_SIZE - 1)))
        return ERR_INVALID_ARGS;
    size = page_up(size);
    struct jwl_pool *p = calloc(1, sizeof(*p));
    if (!p)
        return ERR_NO_MEMORY;
    uint64_t addr = 0;
    status_t st = jam_vmo_create(size, VMO_KEEP_PAGES, HANDLE_INVALID, &p->vmo);
    if (st == OK) {
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), p->vmo, 0, size, VMAR_READ | VMAR_WRITE,
                          &addr);
        if (st != OK)
            jam_handle_close(p->vmo);
    }
    if (st != OK) {
        free(p);
        return st;
    }
    p->c = c;
    p->size = size;
    p->data = (uint8_t *)(uintptr_t)addr;
    p->next = c->pools;
    c->pools = p;
    st = jwlc_pool_make(p);
    if (st != OK && c->conn && c->conn->status == OK) {
        jwl_pool_destroy(p);   /* refused by us, not lost with the connection */
        return st;
    }
    *out = p;
    return OK;
}

status_t jwl_pool_grow(struct jwl_pool *p, uint64_t size)
{
    if (size <= p->size || size > (INT32_MAX & ~(PAGE_SIZE - 1)))
        return ERR_INVALID_ARGS;
    size = page_up(size);
    status_t st = jam_vmo_set_size(p->vmo, size);
    uint64_t addr = 0;
    if (st == OK)
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), p->vmo, 0, size, VMAR_READ | VMAR_WRITE,
                          &addr);
    if (st != OK)
        return st;   /* the VMO may have grown: harmless, the old mapping still works */
    (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)p->data, p->size);
    p->data = (uint8_t *)(uintptr_t)addr;
    p->size = size;
    if (p->id && jwlc_live(p->c))
        return jwl_wl_shm_pool_resize(p->c->conn, p->id, (int32_t)size);
    return OK;
}

void jwl_pool_destroy(struct jwl_pool *p)
{
    if (!p)
        return;
    while (p->buffers)
        jwl_buffer_destroy(p->buffers);
    struct jwl_client *c = p->c;
    if (p->id && c->conn)
        (void)jwl_wl_shm_pool_destroy(c->conn, p->id);   /* a dead connection: nothing to tell */
    for (struct jwl_pool **pp = &c->pools; *pp; pp = &(*pp)->next)
        if (*pp == p) {
            *pp = p->next;
            break;
        }
    (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)p->data, p->size);
    jam_handle_close(p->vmo);
    free(p);
}

uint8_t *jwl_pool_data(const struct jwl_pool *p)
{
    return p->data;
}

uint64_t jwl_pool_size(const struct jwl_pool *p)
{
    return p->size;
}

/* ---- buffers --------------------------------------------------------------------------- */

status_t jwlc_buffer_make(struct jwl_buffer *b)
{
    struct jwl_client *c = b->pool->c;
    if (!jwlc_live(c) || !b->pool->id)
        return OK;   /* made with its pool */
    uint32_t id;
    status_t st = jwlc_make(c, &jwl_wl_buffer_interface, 1, b, &id);
    if (st == OK)
        st = jwlc_made(c, jwl_wl_shm_pool_create_buffer(c->conn, b->pool->id, id,
                                                        (int32_t)b->offset, b->width, b->height,
                                                        b->stride, b->format), id);
    if (st == OK)
        b->id = id;
    return st;
}

/* The format is one we may use: offered, or (before the compositor said)
 * one every compositor must take. */
static bool format_ok(const struct jwl_client *c, uint32_t format)
{
    if (format != JWL_WL_SHM_FORMAT_XRGB8888 && format != JWL_WL_SHM_FORMAT_ARGB8888)
        return false;
    return c->state != JWLC_READY || (c->info.shm_formats >> format & 1);
}

status_t jwl_buffer_create(struct jwl_pool *p, uint64_t offset, int32_t width, int32_t height,
                           int32_t stride, uint32_t format, struct jwl_buffer **out)
{
    if (width < 1 || height < 1 || width > JWL_SIZE_MAX || height > JWL_SIZE_MAX)
        return ERR_INVALID_ARGS;
    /* width * 4 and stride * height fit: both sides are at most 8192 * 4 * 8192 */
    if (stride < width * 4 || stride % 4 || stride > JWL_SIZE_MAX * 16)
        return ERR_INVALID_ARGS;
    uint64_t bytes = (uint64_t)stride * (uint64_t)height;
    if (offset > p->size || bytes > p->size - offset || !format_ok(p->c, format))
        return ERR_INVALID_ARGS;
    struct jwl_buffer *b = calloc(1, sizeof(*b));
    if (!b)
        return ERR_NO_MEMORY;
    b->pool = p;
    b->offset = offset;
    b->width = width;
    b->height = height;
    b->stride = stride;
    b->format = format;
    b->next = p->buffers;
    p->buffers = b;
    status_t st = jwlc_buffer_make(b);
    struct jwl_conn *conn = p->c->conn;
    if (st != OK && conn && conn->status == OK) {
        jwl_buffer_destroy(b);
        return st;
    }
    *out = b;
    return OK;
}

void jwl_buffer_destroy(struct jwl_buffer *b)
{
    if (!b)
        return;
    struct jwl_client *c = b->pool->c;
    if (b->id && c->conn)
        (void)jwl_wl_buffer_destroy(c->conn, b->id);   /* a dead connection: nothing to tell */
    for (struct jwl_buffer **pb = &b->pool->buffers; *pb; pb = &(*pb)->next)
        if (*pb == b) {
            *pb = b->next;
            break;
        }
    free(b);
}

uint32_t *jwl_buffer_pixels(const struct jwl_buffer *b)
{
    return (uint32_t *)(void *)(b->pool->data + b->offset);
}

bool jwl_buffer_busy(const struct jwl_buffer *b)
{
    return b->busy;
}

uint32_t jwl_buffer_id(const struct jwl_buffer *b)
{
    return b->id;
}

void jwl_buffer_attached(struct jwl_buffer *b)
{
    b->busy = true;
}

/* ---- the connection's side ------------------------------------------------------------- */

void jwlc_shm_lost(struct jwl_client *c)
{
    for (struct jwl_pool *p = c->pools; p; p = p->next) {
        p->id = 0;
        for (struct jwl_buffer *b = p->buffers; b; b = b->next) {
            b->id = 0;
            b->busy = false;
        }
    }
}

status_t jwlc_shm_event(struct jwl_client *c, struct jwl_msg *m)
{
    (void)c;
    if (m->iface != &jwl_wl_buffer_interface || m->opcode != JWL_WL_BUFFER_EV_RELEASE)
        return ERR_NOT_SUPPORTED;
    struct jwl_buffer *b = m->data;
    if (b)
        b->busy = false;
    return OK;
}
