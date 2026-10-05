/* wl_shm, wl_shm_pool and wl_buffer (comp.h): a client's pixels, in its
 * own memory, read by us without trusting it.
 *
 * A pool is a VMO of the client's made with VMO_KEEP_PAGES: every page
 * committed and charged to the client, and no holder can ever decommit a
 * page or shrink it. We map it with VMAR_KEPT_ONLY, read-only, every page
 * present before the map returns, so nothing the client does can make a
 * read of ours fault (the kernel refuses any other VMO: invalid_fd). We
 * keep its handle, because wl_shm_pool.resize ("only bigger") maps it
 * again to reach the pages the client added; the old mapping goes as soon
 * as the new one is there (no paint runs between requests).
 *
 * A buffer is checked against its pool when it is made, with no
 * arithmetic that can overflow: a known format, width and height 1 to
 * COMP_BUFFER_SIDE_MAX, a stride that is a multiple of 4 and at least
 * width * 4, offset a multiple of 4, offset + stride * height within the
 * pool. Pools never shrink, so a buffer stays inside its pool for good.
 * Nothing in the pool's memory is ever read as anything but pixels.
 *
 * Lifetimes, as the protocol says: a pool lives while its wl_shm_pool or
 * any buffer made from it does; a buffer lives while its wl_buffer or any
 * surface state holds it. wl_buffer.release goes when the last surface
 * showing a buffer stops showing it (a newer commit, or the surface
 * going), if the client hasn't destroyed the wl_buffer. */
#include <jwl/wayland.h>
#include "comp.h"

/* ---- pools ---------------------------------------------------------------------------- */

static uint64_t round_page(uint64_t n)
{
    return (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

/* Map bytes [0, size) of vmo, kept and read-only: OK with *addr, or why not. */
static status_t map_pool(handle_t vmo, uint32_t size, uint64_t *addr)
{
    uint64_t vsize = 0;
    status_t st = jam_vmo_get_size(vmo, &vsize);
    if (st == OK && round_page(size) > vsize)
        st = ERR_OUT_OF_RANGE;
    if (st == OK)
        st = jam_vmar_map(comp.vmar, vmo, 0, round_page(size), VMAR_READ | VMAR_KEPT_ONLY, addr);
    return st;
}

static const char *map_refusal(status_t st)
{
    switch (st) {
    case ERR_WRONG_TYPE:
        return "not a VMO made with VMO_KEEP_PAGES";
    case ERR_ACCESS_DENIED:
        return "the handle lacks read or map rights";
    case ERR_OUT_OF_RANGE:
        return "the size is past the VMO's end";
    case ERR_NO_MEMORY:
        return "no memory to map it";
    default:
        return status_str(st);
    }
}

static void pool_unref(struct comp_pool *p)
{
    if (--p->refs)
        return;
    struct comp_client *cl = p->client;
    (void)jam_vmar_unmap(comp.vmar, p->addr, p->mapped);   /* ours: nothing to do if it fails */
    jam_handle_close(p->vmo);
    cl->pool_bytes -= p->mapped;
    for (struct comp_pool **q = &cl->pools; *q; q = &(*q)->next) {
        if (*q == p) {
            *q = p->next;
            break;
        }
    }
    cl->npools--;
    free(p);
}

static status_t on_create_pool(void *data, uint32_t self, uint32_t id, handle_t fd, int32_t size)
{
    struct comp_client *cl = data;
    status_t st = OK;
    if (size <= 0)
        st = comp_error(cl, self, JWL_WL_SHM_ERROR_INVALID_STRIDE, "pool size %d", size);
    else if (cl->npools >= COMP_POOLS_MAX)
        st = comp_no_memory(cl, self, "too many pools (64)");
    else if (round_page((uint32_t)size) > COMP_POOL_BYTES_MAX - cl->pool_bytes)
        st = comp_no_memory(cl, self, "pools over 256 MiB in all");
    struct comp_pool *p = st == OK ? calloc(1, sizeof(*p)) : NULL;
    if (st == OK && !p)
        st = comp_no_memory(cl, self, "no memory for a pool");
    uint64_t addr = 0;
    status_t mst = st == OK ? map_pool(fd, (uint32_t)size, &addr) : st;
    if (st == OK && mst != OK)
        st = comp_error(cl, self, JWL_WL_SHM_ERROR_INVALID_FD, "pool: %s", map_refusal(mst));
    if (st != OK) {
        free(p);
        jam_handle_close(fd);
        return st;
    }
    *p = (struct comp_pool){ .client = cl, .next = cl->pools, .id = id, .refs = 1, .vmo = fd,
                             .addr = addr, .mapped = round_page((uint32_t)size),
                             .size = (uint32_t)size };
    cl->pools = p;
    cl->npools++;
    cl->pool_bytes += p->mapped;
    return jwl_map_set_data(&cl->conn->map, id, p);
}

static status_t shm_release(void *data, uint32_t self)
{
    (void)data;
    (void)self;
    return OK;   /* libjwl freed the id; a wl_shm holds nothing */
}

static const struct jwl_wl_shm_requests shm_ops = {
    .create_pool = on_create_pool, .release = shm_release,
};

status_t shm_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;
    return jwl_wl_shm_dispatch_request(&shm_ops, m->data, m->id, m->opcode, m->args);
}

status_t shm_bind(struct comp_client *cl, uint32_t id, uint32_t version)
{
    (void)version;
    status_t st = jwl_map_set_data(&cl->conn->map, id, cl);
    if (st == OK)
        st = jwl_wl_shm_send_format(cl->conn, id, JWL_WL_SHM_FORMAT_ARGB8888);
    if (st == OK)
        st = jwl_wl_shm_send_format(cl->conn, id, JWL_WL_SHM_FORMAT_XRGB8888);
    return st;
}

/* ---- wl_shm_pool ---------------------------------------------------------------------- */

/* create_buffer's numbers, checked: NULL, or why they don't fit. */
static const char *buffer_misfit(const struct comp_pool *p, int32_t offset, int32_t w, int32_t h,
                                 int32_t stride)
{
    if (w < 1 || h < 1 || w > COMP_BUFFER_SIDE_MAX || h > COMP_BUFFER_SIDE_MAX)
        return "width or height outside 1 to 8192";
    if (stride < 0 || (uint32_t)stride < (uint32_t)w * 4 || stride % 4)
        return "a stride under width * 4, or not a multiple of 4";
    if (offset < 0 || offset % 4)
        return "a negative offset, or not a multiple of 4";
    uint64_t end = (uint64_t)(uint32_t)offset + (uint64_t)(uint32_t)stride * (uint32_t)h;
    return end > p->size ? "past the pool's end" : NULL;
}

static status_t on_create_buffer(void *data, uint32_t self, uint32_t id, int32_t offset,
                                 int32_t width, int32_t height, int32_t stride, uint32_t format)
{
    struct comp_pool *p = data;
    struct comp_client *cl = p->client;
    if (format != JWL_WL_SHM_FORMAT_ARGB8888 && format != JWL_WL_SHM_FORMAT_XRGB8888)
        return comp_error(cl, self, JWL_WL_SHM_ERROR_INVALID_FORMAT, "format 0x%x", format);
    const char *why = buffer_misfit(p, offset, width, height, stride);
    if (why)
        return comp_error(cl, self, JWL_WL_SHM_ERROR_INVALID_STRIDE, "buffer: %s", why);
    if (cl->nbuffers >= COMP_BUFFERS_MAX)
        return comp_no_memory(cl, self, "too many buffers (256)");
    struct comp_buffer *b = calloc(1, sizeof(*b));
    if (!b)
        return comp_no_memory(cl, self, "no memory for a buffer");
    *b = (struct comp_buffer){ .client = cl, .next = cl->buffers, .pool = p, .id = id, .refs = 1,
                               .offset = (uint32_t)offset, .width = width, .height = height,
                               .stride = (uint32_t)stride, .format = format };
    p->refs++;
    cl->buffers = b;
    cl->nbuffers++;
    return jwl_map_set_data(&cl->conn->map, id, b);
}

static status_t on_pool_destroy(void *data, uint32_t self)
{
    struct comp_pool *p = data;
    (void)self;
    p->id = 0;   /* libjwl freed it; buffers may keep the pool */
    pool_unref(p);
    return OK;
}

static status_t on_resize(void *data, uint32_t self, int32_t size)
{
    struct comp_pool *p = data;
    struct comp_client *cl = p->client;
    if (size < 0 || (uint32_t)size < p->size)
        return comp_error(cl, self, JWL_WL_SHM_ERROR_INVALID_STRIDE,
                          "pool resize to %d: pools only grow (now %u)", size, p->size);
    uint64_t grow = round_page((uint32_t)size) - p->mapped;
    if (grow > COMP_POOL_BYTES_MAX - cl->pool_bytes)
        return comp_no_memory(cl, self, "pools over 256 MiB in all");
    if (!grow) {
        p->size = (uint32_t)size;
        return OK;
    }
    uint64_t addr = 0;
    status_t st = map_pool(p->vmo, (uint32_t)size, &addr);
    if (st != OK)
        return comp_error(cl, self, JWL_WL_SHM_ERROR_INVALID_FD, "pool resize: %s",
                          map_refusal(st));
    (void)jam_vmar_unmap(comp.vmar, p->addr, p->mapped);
    p->addr = addr;
    p->mapped += grow;
    p->size = (uint32_t)size;
    cl->pool_bytes += grow;
    return OK;
}

static const struct jwl_wl_shm_pool_requests pool_ops = {
    .create_buffer = on_create_buffer, .destroy = on_pool_destroy, .resize = on_resize,
};

status_t pool_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;
    return jwl_wl_shm_pool_dispatch_request(&pool_ops, m->data, m->id, m->opcode, m->args);
}

/* ---- wl_buffer ------------------------------------------------------------------------ */

void buffer_ref(struct comp_buffer *b)
{
    b->refs++;
}

void buffer_unref(struct comp_buffer *b)
{
    if (--b->refs)
        return;
    struct comp_client *cl = b->client;
    for (struct comp_buffer **q = &cl->buffers; *q; q = &(*q)->next) {
        if (*q == b) {
            *q = b->next;
            break;
        }
    }
    cl->nbuffers--;
    pool_unref(b->pool);
    free(b);
}

void buffer_show(struct comp_buffer *b)
{
    b->busy++;
}

void buffer_unshow(struct comp_buffer *b)
{
    struct jwl_conn *c = b->client->conn;
    if (--b->busy == 0 && b->id && c && c->status == OK)
        (void)jwl_wl_buffer_send_release(c, b->id);   /* a dead conn: torn down after */
}

static status_t on_buffer_destroy(void *data, uint32_t self)
{
    struct comp_buffer *b = data;
    (void)self;
    b->id = 0;   /* libjwl freed it; a surface may still show the pixels */
    buffer_unref(b);
    return OK;
}

static const struct jwl_wl_buffer_requests buffer_ops = { .destroy = on_buffer_destroy };

status_t buffer_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;
    return jwl_wl_buffer_dispatch_request(&buffer_ops, m->data, m->id, m->opcode, m->args);
}

/* ---- teardown ------------------------------------------------------------------------- */

void shm_teardown(struct comp_client *cl)
{
    /* Surfaces went first (surfaces_teardown), so only the protocol objects'
     * references are left. */
    while (cl->buffers) {
        struct comp_buffer *b = cl->buffers;
        b->id = 0;
        b->refs = 1;
        buffer_unref(b);
    }
    while (cl->pools) {
        struct comp_pool *p = cl->pools;
        p->refs = 1;
        pool_unref(p);
    }
}
