/* A socket's rings (<sockring.h>, which has the model): the counts, the
 * wake flags and the two framings, shared by netstack and programs.
 *
 * Everything the peer can write is read with atomic loads, once: a count
 * is clamped to the ring before it is used, a datagram record's header is
 * loaded as two words and checked on the copy, and bytes are copied out
 * before anyone looks at them. This side's own count lives only in its
 * private sockring_end and is never read back from the shared page, so a
 * peer that scribbles on the page confuses only itself. */
#include <sockring.h>

#define ALIGN_UP(x) (((x) + SOCKRING_ALIGN - 1) & ~(SOCKRING_ALIGN - 1))

bool sockring_size_ok(uint32_t size)
{
    return size >= SOCKRING_MIN && size <= SOCKRING_MAX && (size & (size - 1)) == 0;
}

uint64_t sockring_bytes(uint32_t tx_size, uint32_t rx_size)
{
    return (uint64_t)SOCKRING_HDR + tx_size + rx_size;
}

static bool framing_ok(uint32_t framing)
{
    return framing == SOCKRING_DGRAM || framing == SOCKRING_STREAM;
}

/* Point both ends at map's rings; this side is netstack (tx consumer, rx
 * producer) or the program (the other way). */
static void ends_set(struct sockring *r, void *map, uint32_t framing, uint32_t tx_size,
                     uint32_t rx_size, bool stack)
{
    struct sockring_page *pg = map;
    uint8_t *base = map;
    r->page = pg;
    r->tx = (struct sockring_end){ .prod = &pg->tx_prod, .cons = &pg->tx_cons,
                                   .data = base + SOCKRING_HDR, .size = tx_size,
                                   .framing = framing, .producer = !stack };
    r->rx = (struct sockring_end){ .prod = &pg->rx_prod, .cons = &pg->rx_cons,
                                   .data = base + SOCKRING_HDR + tx_size, .size = rx_size,
                                   .framing = framing, .producer = stack };
}

status_t sockring_make(struct sockring *r, void *map, uint32_t framing, uint32_t tx_size,
                       uint32_t rx_size)
{
    if (!framing_ok(framing) || !sockring_size_ok(tx_size) || !sockring_size_ok(rx_size))
        return ERR_INVALID_ARGS;
    struct sockring_page *pg = map;
    pg->info.magic = SOCKRING_MAGIC;
    pg->info.framing = framing;
    pg->info.tx_size = tx_size;
    pg->info.rx_size = rx_size;
    pg->status.state = SOCKRING_STATE_OPEN;
    ends_set(r, map, framing, tx_size, rx_size, true);
    return OK;
}

status_t sockring_attach(struct sockring *r, void *map, uint64_t map_len, uint32_t framing,
                         uint32_t tx_size, uint32_t rx_size)
{
    struct sockring_info *in = &((struct sockring_page *)map)->info;
    if (!framing_ok(framing) || !sockring_size_ok(tx_size) || !sockring_size_ok(rx_size) ||
        sockring_bytes(tx_size, rx_size) > map_len)
        return ERR_BAD_STATE;
    if (__atomic_load_n(&in->magic, __ATOMIC_RELAXED) != SOCKRING_MAGIC ||
        __atomic_load_n(&in->framing, __ATOMIC_RELAXED) != framing ||
        __atomic_load_n(&in->tx_size, __ATOMIC_RELAXED) != tx_size ||
        __atomic_load_n(&in->rx_size, __ATOMIC_RELAXED) != rx_size)
        return ERR_BAD_STATE;
    ends_set(r, map, framing, tx_size, rx_size, false);
    return OK;
}

/* ---- the counts ---------------------------------------------------------------- */

uint32_t sockring_ready_n(uint64_t produced, uint64_t consumed, uint32_t size, bool *bad)
{
    if (produced < consumed) {
        *bad = true;
        return 0;
    }
    if (produced - consumed > size) {
        *bad = true;
        return size;
    }
    return (uint32_t)(produced - consumed);
}

uint32_t sockring_room_n(uint64_t produced, uint64_t consumed, uint32_t size, bool *bad)
{
    if (consumed > produced || produced - consumed > size) {
        *bad = true;
        return 0;
    }
    return size - (uint32_t)(produced - consumed);
}

/* Consumer: the producer's count, read once after its flags (the producer
 * stores the count before SOCKRING_END, both with release, so a count read
 * after the flag is at least the final one). The first END seen fixes the
 * end at the count then (clamped to the ring); a count past it later is the
 * producer adding bytes after its end: ignored and counted. */
static uint64_t produced_of(struct sockring_end *e, bool *bad)
{
    uint32_t flags = __atomic_load_n(&e->prod->flags, __ATOMIC_ACQUIRE);
    uint64_t p = __atomic_load_n(&e->prod->count, __ATOMIC_ACQUIRE);
    if (flags & ~SOCKRING_END)
        *bad = true;
    if (!e->ended && (flags & SOCKRING_END)) {
        bool ignored = false;   /* a bad count is reported by the caller's own clamp */
        e->ended = true;
        e->end_at = e->count + sockring_ready_n(p, e->count, e->size, &ignored);
        if (e->framing == SOCKRING_DGRAM)
            e->end_at &= ~(uint64_t)(SOCKRING_ALIGN - 1);
    }
    if (e->ended && p != e->end_at) {   /* an honest producer's count stays at its end */
        *bad = true;
        p = e->end_at;
    }
    return p;
}

uint32_t sockring_ready(struct sockring_end *e)
{
    bool bad = false;
    uint64_t p = produced_of(e, &bad);
    uint32_t n = sockring_ready_n(p, e->count, e->size, &bad);
    if (e->framing == SOCKRING_DGRAM && (n & (SOCKRING_ALIGN - 1))) {
        bad = true;
        n &= ~(SOCKRING_ALIGN - 1);
    }
    e->errors += bad;
    return n;
}

uint32_t sockring_room(struct sockring_end *e)
{
    bool bad = false;
    uint64_t c = __atomic_load_n(&e->cons->count, __ATOMIC_ACQUIRE);
    uint32_t n = sockring_room_n(e->count, c, e->size, &bad);
    e->errors += bad;
    return n;
}

bool sockring_publish(struct sockring_end *e)
{
    struct sockring_line *mine = e->producer ? e->prod : e->cons;
    struct sockring_line *peer = e->producer ? e->cons : e->prod;
    __atomic_store_n(&mine->count, e->count, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return __atomic_load_n(&peer->waits, __ATOMIC_RELAXED) != 0;
}

bool sockring_sleep(struct sockring_end *e, uint32_t need)
{
    uint32_t *flag = e->producer ? &e->prod->waits : &e->cons->waits;
    if (!need)
        need = 1;
    __atomic_store_n(flag, 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    bool idle = e->producer ? sockring_room(e) < need : sockring_ready(e) < need && !e->ended;
    if (idle)
        return true;
    __atomic_store_n(flag, 0, __ATOMIC_RELAXED);
    return false;
}

void sockring_awake(struct sockring_end *e)
{
    __atomic_store_n(e->producer ? &e->prod->waits : &e->cons->waits, 0, __ATOMIC_RELAXED);
}

bool sockring_finish(struct sockring_end *e)
{
    __atomic_store_n(&e->prod->count, e->count, __ATOMIC_RELEASE);
    __atomic_store_n(&e->prod->flags, SOCKRING_END, __ATOMIC_RELEASE);
    e->ended = true;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return __atomic_load_n(&e->cons->waits, __ATOMIC_RELAXED) != 0;
}

bool sockring_at_end(struct sockring_end *e)
{
    return sockring_ready(e) == 0 && e->ended && e->count == e->end_at;
}

/* ---- copies in and out, across the ring's end ------------------------------------- */

static void ring_in(struct sockring_end *e, uint64_t at, const void *src, uint32_t n)
{
    uint32_t off = (uint32_t)(at & (e->size - 1)), first = e->size - off;
    if (first > n)
        first = n;
    memcpy(e->data + off, src, first);
    memcpy(e->data, (const uint8_t *)src + first, n - first);
}

static void ring_out(const struct sockring_end *e, uint64_t at, void *dst, uint32_t n)
{
    uint32_t off = (uint32_t)(at & (e->size - 1)), first = e->size - off;
    if (first > n)
        first = n;
    memcpy(dst, e->data + off, first);
    memcpy((uint8_t *)dst + first, e->data, n - first);
}

/* ---- datagrams ------------------------------------------------------------------ */

uint32_t sockring_dgram_bytes(uint32_t len)
{
    return SOCKRING_DGRAM_HDR + ALIGN_UP(len);
}

status_t sockring_dgram_put(struct sockring_end *e, const struct sockring_dgram *h,
                            const void *data)
{
    if (h->len > SOCKRING_DGRAM_MAX || h->flags || h->reserved)
        return ERR_INVALID_ARGS;
    if (e->ended)
        return ERR_BAD_STATE;
    uint32_t need = sockring_dgram_bytes(h->len);
    if (sockring_room(e) < need)
        return ERR_SHOULD_WAIT;
    /* e->count is on SOCKRING_ALIGN, so the header is in one piece */
    memcpy(e->data + (e->count & (e->size - 1)), h, SOCKRING_DGRAM_HDR);
    ring_in(e, e->count + SOCKRING_DGRAM_HDR, data, h->len);
    e->count += need;
    return OK;
}

status_t sockring_dgram_take(struct sockring_end *e, struct sockring_dgram *h,
                             uint8_t data[SOCKRING_DGRAM_MAX])
{
    uint32_t n = sockring_ready(e);
    if (!n)
        return ERR_SHOULD_WAIT;
    /* The header, once, as two words: what is checked is what is used. */
    const uint64_t *w = (const uint64_t *)(e->data + (e->count & (e->size - 1)));
    uint64_t w0 = __atomic_load_n(&w[0], __ATOMIC_RELAXED);
    uint64_t w1 = __atomic_load_n(&w[1], __ATOMIC_RELAXED);
    struct sockring_dgram d;
    memcpy(&d, &w0, sizeof(w0));
    memcpy((uint8_t *)&d + sizeof(w0), &w1, sizeof(w1));
    if (d.flags || d.reserved || d.len > SOCKRING_DGRAM_MAX || sockring_dgram_bytes(d.len) > n) {
        e->count += n;   /* the framing is lost: drop what was published */
        e->errors++;
        return ERR_OUT_OF_RANGE;
    }
    ring_out(e, e->count + SOCKRING_DGRAM_HDR, data, d.len);
    e->count += sockring_dgram_bytes(d.len);
    *h = d;
    return OK;
}

/* ---- byte streams --------------------------------------------------------------- */

uint32_t sockring_stream_write(struct sockring_end *e, const void *src, uint32_t n)
{
    if (e->ended)
        return 0;
    uint32_t room = sockring_room(e);
    if (n > room)
        n = room;
    ring_in(e, e->count, src, n);
    e->count += n;
    return n;
}

uint32_t sockring_stream_read(struct sockring_end *e, void *dst, uint32_t n)
{
    uint32_t ready = sockring_ready(e);
    if (n > ready)
        n = ready;
    ring_out(e, e->count, dst, n);
    e->count += n;
    return n;
}

/* ---- the status line ---------------------------------------------------------------- */

void sockring_status_put(struct sockring *r, const struct sockring_status *s)
{
    struct sockring_status *o = &r->page->status;
    __atomic_store_n(&o->state, s->state, __ATOMIC_RELAXED);
    __atomic_store_n(&o->error, s->error, __ATOMIC_RELAXED);
    __atomic_store_n(&o->rx_dropped, s->rx_dropped, __ATOMIC_RELAXED);
    __atomic_store_n(&o->tx_refused, s->tx_refused, __ATOMIC_RELAXED);
    __atomic_store_n(&o->ring_errors, s->ring_errors, __ATOMIC_RELAXED);
    __atomic_store_n(&o->changes, s->changes, __ATOMIC_RELEASE);
}

void sockring_status_get(const struct sockring *r, struct sockring_status *out)
{
    const struct sockring_status *o = &r->page->status;
    *out = (struct sockring_status){ 0 };
    out->changes = __atomic_load_n(&o->changes, __ATOMIC_ACQUIRE);
    out->state = __atomic_load_n(&o->state, __ATOMIC_RELAXED);
    out->error = __atomic_load_n(&o->error, __ATOMIC_RELAXED);
    out->rx_dropped = __atomic_load_n(&o->rx_dropped, __ATOMIC_RELAXED);
    out->tx_refused = __atomic_load_n(&o->tx_refused, __ATOMIC_RELAXED);
    out->ring_errors = __atomic_load_n(&o->ring_errors, __ATOMIC_RELAXED);
}
