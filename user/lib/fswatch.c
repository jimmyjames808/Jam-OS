/* fswatch: file watching in a file service (<fswatch.h>).
 *
 * The index: watches on one inode hang in one bucket's chain (slot + 1 in
 * bucket[] and chain[], 0 ending it), made again from the table by a
 * successor. A queue is a list of events, each allocated as it comes, at
 * most FSWATCH_QUEUE_BYTES of records (the overflow's own record may pass
 * that by its 14 bytes); a channel's one IN_Q_OVERFLOW lives in its struct
 * fsw_chan, so running out of memory still says events were lost. */
#include <fs_idl.h>
#include <fswatch.h>

#define SLOT_MASK   ((1u << FSW_SLOT_BITS) - 1)
#define EVENTS      FSWATCH_IN_ALL_EVENTS
/* Every bit of Linux's inotify (ALL_INOTIFY_BITS): a mask with none of them
 * is EINVAL there, other bits are ignored. */
#define ALL_BITS    (EVENTS | FSWATCH_IN_UNMOUNT | FSWATCH_IN_Q_OVERFLOW | FSWATCH_IN_IGNORED | \
                     FSWATCH_IN_ONLYDIR | FSWATCH_IN_DONT_FOLLOW | FSWATCH_IN_EXCL_UNLINK | \
                     FSWATCH_IN_MASK_CREATE | FSWATCH_IN_MASK_ADD | FSWATCH_IN_ISDIR | \
                     FSWATCH_IN_ONESHOT)
/* What a watch's mask keeps: its events and the flags that act later. */
#define WATCH_BITS  (EVENTS | FSWATCH_IN_ONESHOT | FSWATCH_IN_EXCL_UNLINK)

static uint32_t rec_bytes(uint32_t len)
{
    return FSWATCH_RECORD_FIXED + len;
}

static bool same(const struct fsw_watch_rec *r, const struct fsw_key *k)
{
    return r->chan && r->ino == k->ino && r->gen == k->gen;
}

static uint32_t hash(uint64_t ino, uint32_t gen)
{
    uint64_t x = (ino ^ ((uint64_t)gen << 40)) * 0x9e3779b97f4a7c15ull;
    return (uint32_t)(x >> 40) & (FSW_HASH - 1);
}

static bool live(const struct fsw *w, uint32_t c)
{
    return c < FSW_CHANNELS && w->t->chans[c].live;
}

/* ---- The index --------------------------------------------------------------- */

static void index_add(struct fsw *w, uint32_t slot)
{
    const struct fsw_watch_rec *r = &w->t->w[slot];
    uint32_t b = hash(r->ino, r->gen);
    w->chain[slot] = w->bucket[b];
    w->bucket[b] = (uint16_t)(slot + 1);
}

static void index_del(struct fsw *w, uint32_t slot)
{
    const struct fsw_watch_rec *r = &w->t->w[slot];
    uint16_t *p = &w->bucket[hash(r->ino, r->gen)];
    while (*p && *p != slot + 1)
        p = &w->chain[*p - 1];
    if (*p)
        *p = w->chain[slot];
    w->chain[slot] = 0;
}

/* The watch of channel c on inode key: its slot, or -1. */
static int find(const struct fsw *w, const struct fsw_key *key, uint32_t c)
{
    for (uint16_t s = w->bucket[hash(key->ino, key->gen)]; s; s = w->chain[s - 1])
        if (same(&w->t->w[s - 1], key) && w->t->w[s - 1].chan == c + 1)
            return s - 1;
    return -1;
}

/* ---- Queues ------------------------------------------------------------------ */

static void ring(const struct fsw_chan *q)
{
    if (q->bell != HANDLE_INVALID)
        (void)jam_event_signal(q->bell, 0, FSWATCH_BELL);
}

static void append(struct fsw_chan *q, struct fsw_event *e)
{
    bool was_empty = !q->head;
    e->next = NULL;
    if (q->tail)
        q->tail->next = e;
    else
        q->head = e;
    q->tail = e;
    q->bytes += rec_bytes(e->len);
    if (was_empty)
        ring(q);
}

static void free_event(struct fsw_chan *q, struct fsw_event *e)
{
    if (e == &q->over)
        q->over_queued = false;
    else
        free(e);
}

/* Queue the channel's one IN_Q_OVERFLOW, unless it is queued already. */
static void overflow(struct fsw_chan *q)
{
    if (q->over_queued)
        return;
    q->over = (struct fsw_event){ .mask = FSWATCH_IN_Q_OVERFLOW };
    q->over_queued = true;
    append(q, &q->over);
}

/* Queue a record on channel c, by Linux's rules: an event equal to the
 * last one queued is dropped; one past the queue's room becomes the
 * overflow, or is dropped once the overflow is queued. */
static void queue(struct fsw *w, uint32_t c, uint32_t id, uint32_t mask, uint32_t cookie,
                  const char *name, uint32_t len)
{
    struct fsw_chan *q = &w->chans[c];
    const struct fsw_event *t = q->tail;
    if (t && t->id == id && t->mask == mask && t->cookie == cookie && t->len == len &&
        !memcmp(t + 1, name, len))
        return;
    if (len > FS_NAME_LEN || q->bytes + rec_bytes(len) > FSWATCH_QUEUE_BYTES) {
        overflow(q);
        return;
    }
    struct fsw_event *e = malloc(sizeof(*e) + len);
    if (!e) {
        overflow(q);
        return;
    }
    *e = (struct fsw_event){ .id = id, .mask = mask, .cookie = cookie, .len = (uint16_t)len };
    memcpy(e + 1, name, len);
    append(q, e);
}

static void drop_all(struct fsw_chan *q)
{
    while (q->head) {
        struct fsw_event *e = q->head;
        q->head = e->next;
        free_event(q, e);
    }
    q->tail = NULL;
    q->bytes = 0;
}

/* ---- Watches ----------------------------------------------------------------- */

/* Free the watch in slot, silently. */
static void unwatch(struct fsw *w, uint32_t slot)
{
    struct fsw_watch_rec *r = &w->t->w[slot];
    index_del(w, slot);
    w->t->chans[r->chan - 1].watches--;
    w->t->watches--;
    *r = (struct fsw_watch_rec){ .id = r->id };   /* the count stays */
}

/* End the watch in slot: IN_IGNORED queued for it (after `first`, if not 0
 * and asked for), then freed. `first` (IN_DELETE_SELF, IN_MOVE_SELF) never
 * carries IN_ISDIR: Linux's inotify masks it off those two. */
static void end_watch(struct fsw *w, uint32_t slot, uint32_t first)
{
    const struct fsw_watch_rec *r = &w->t->w[slot];
    uint32_t c = r->chan - 1;
    if (first & r->mask)
        queue(w, c, r->id, first, 0, NULL, 0);
    queue(w, c, r->id, FSWATCH_IN_IGNORED, 0, NULL, 0);
    unwatch(w, slot);
}

/* Queue an event for the watch in slot; IN_ONESHOT ends it then. */
static void deliver(struct fsw *w, uint32_t slot, uint32_t mask, uint32_t cookie,
                    const char *name, uint32_t len)
{
    const struct fsw_watch_rec *r = &w->t->w[slot];
    queue(w, r->chan - 1, r->id, mask, cookie, name, len);
    if (r->mask & FSWATCH_IN_ONESHOT)
        end_watch(w, slot, 0);
}

/* ---- Set up ------------------------------------------------------------------ */

static void fresh_parts(struct fsw *w, struct fsw_table *t)
{
    w->t = t;
    w->cookie = os_random_u32();
    memset(w->bucket, 0, sizeof(w->bucket));
    memset(w->chain, 0, sizeof(w->chain));
    for (uint32_t c = 0; c < FSW_CHANNELS; c++)
        w->chans[c] = (struct fsw_chan){ .ch = HANDLE_INVALID, .bell = HANDLE_INVALID };
}

void fsw_init(struct fsw *w, struct fsw_table *t)
{
    memset(t, 0, sizeof(*t));
    t->version = FSW_TABLE_VERSION;
    fresh_parts(w, t);
}

/* Is the dead instance's table whole: every count and index inside? */
static bool table_ok(const struct fsw_table *t)
{
    uint32_t per[FSW_CHANNELS] = { 0 }, total = 0;
    if (t->version != FSW_TABLE_VERSION || t->next >= FSW_WATCHES)
        return false;
    for (uint32_t c = 0; c < FSW_CHANNELS; c++)
        if (t->chans[c].live > 1 || t->chans[c].belled > 1)
            return false;
    for (uint32_t s = 0; s < FSW_WATCHES; s++) {
        const struct fsw_watch_rec *r = &t->w[s];
        if (r->id && (r->id & SLOT_MASK) != s)   /* 0: a slot never used */
            return false;
        if (!r->chan)
            continue;
        if (r->chan > FSW_CHANNELS || !t->chans[r->chan - 1].live || !(r->id >> FSW_SLOT_BITS) ||
            (r->mask & ~WATCH_BITS) || (r->flags & ~FSW_W_DIR))
            return false;
        per[r->chan - 1]++;
        total++;
    }
    for (uint32_t c = 0; c < FSW_CHANNELS; c++)
        if (per[c] != (t->chans[c].live ? t->chans[c].watches : 0) || per[c] > FSW_PER_CHANNEL)
            return false;
    return total == t->watches;
}

bool fsw_adopt(struct fsw *w, struct fsw_table *t)
{
    if (!table_ok(t)) {
        fsw_init(w, t);
        return false;
    }
    fresh_parts(w, t);
    for (uint32_t s = 0; s < FSW_WATCHES; s++)
        if (t->w[s].chan)
            index_add(w, s);
    for (uint32_t c = 0; c < FSW_CHANNELS; c++)
        if (t->chans[c].live)
            overflow(&w->chans[c]);   /* no bell yet: rung by fsw_chan_handles */
    return true;
}

/* ---- Channels ---------------------------------------------------------------- */

status_t fsw_chan_new(struct fsw *w, uint32_t pid, uint32_t view, const struct fsw_key *root,
                      handle_t ch, uint32_t *out)
{
    uint32_t mine = 0, c = FSW_CHANNELS;
    for (uint32_t i = 0; i < FSW_CHANNELS; i++) {
        if (!w->t->chans[i].live) {
            if (c == FSW_CHANNELS)
                c = i;
        } else if (pid && w->t->chans[i].pid == pid) {
            mine++;
        }
    }
    if (c == FSW_CHANNELS || mine >= FSW_PER_PROCESS) {
        jam_handle_close(ch);
        return ERR_NO_RESOURCES;
    }
    w->t->chans[c] = (struct fsw_chan_rec){ .live = 1, .pid = pid, .view = view,
                                            .root_gen = root->gen, .root = root->ino };
    w->chans[c] = (struct fsw_chan){ .ch = ch, .bell = HANDLE_INVALID };
    *out = c;
    return OK;
}

void fsw_chan_handles(struct fsw *w, uint32_t c, handle_t ch, handle_t bell)
{
    struct fsw_chan *q = &w->chans[c];
    q->ch = ch;
    q->bell = bell;
    if (q->head)
        ring(q);
}

static void got_bell(void *ctx, handle_t bell)
{
    *(handle_t *)ctx = bell;
}

status_t fsw_chan_bell(struct fsw *w, uint32_t c)
{
    static const struct fswatch_bell_ops ops = { .give = got_bell };
    if (!live(w, c) || fsw_belled(w, c))
        return ERR_BAD_STATE;
    struct fsw_chan *q = &w->chans[c];
    uint8_t msg[FSWATCH_BELL_REQ_MAX];
    handle_t hs[1], bell = HANDLE_INVALID;
    uint32_t n = 0, nh = 0;
    status_t st = drv_channel_read(q->ch, msg, sizeof(msg), &n, hs, 1, &nh);
    if (st == ERR_BUFFER_TOO_SMALL)
        return ERR_INVALID_ARGS;   /* bigger than a give: another message (left queued) */
    if (st != OK)
        return st;
    if (n < sizeof(struct idl_req_hdr) || ((const struct idl_req_hdr *)msg)->txid) {
        idl_close_all(hs, nh);
        return ERR_INVALID_ARGS;   /* a call, not the give */
    }
    st = fswatch_bell_event_in(&ops, &bell, msg, n, hs, nh);
    if (st != OK)
        return st;
    struct handle_info info;
    st = jam_handle_info(bell, &info);
    if (st == OK && !(info.rights & RIGHT_SIGNAL))
        st = ERR_ACCESS_DENIED;
    if (st != OK) {
        jam_handle_close(bell);
        return st;
    }
    q->bell = bell;
    w->t->chans[c].belled = 1;
    if (q->head)
        ring(q);
    return OK;
}

void fsw_chan_close(struct fsw *w, uint32_t c)
{
    if (!live(w, c))
        return;
    for (uint32_t s = 0; s < FSW_WATCHES && w->t->chans[c].watches; s++)
        if (w->t->w[s].chan == c + 1)
            unwatch(w, s);
    struct fsw_chan *q = &w->chans[c];
    drop_all(q);
    if (q->ch != HANDLE_INVALID)
        jam_handle_close(q->ch);
    if (q->bell != HANDLE_INVALID)
        jam_handle_close(q->bell);
    *q = (struct fsw_chan){ .ch = HANDLE_INVALID, .bell = HANDLE_INVALID };
    w->t->chans[c] = (struct fsw_chan_rec){ 0 };
}

/* ---- Requests ---------------------------------------------------------------- */

status_t fsw_mask_check(uint32_t mask)
{
    if (!(mask & ALL_BITS))
        return ERR_INVALID_ARGS;
    if ((mask & FSWATCH_IN_MASK_ADD) && (mask & FSWATCH_IN_MASK_CREATE))
        return ERR_INVALID_ARGS;
    return OK;
}

status_t fsw_add(struct fsw *w, uint32_t c, const struct fsw_key *key, bool dir, uint32_t mask,
                 uint32_t *out_id, uint8_t *out_existed)
{
    struct fsw_table *t = w->t;
    status_t st = fsw_mask_check(mask);
    if (st != OK)
        return st;
    if (!live(w, c))
        return ERR_BAD_STATE;
    if ((mask & FSWATCH_IN_ONLYDIR) && !dir)
        return ERR_NOT_DIR;
    uint32_t keep = mask & WATCH_BITS;
    uint16_t flags = dir ? FSW_W_DIR : 0;
    int found = find(w, key, c);
    if (found >= 0) {
        struct fsw_watch_rec *r = &t->w[found];
        if (mask & FSWATCH_IN_MASK_CREATE)
            return ERR_ALREADY_EXISTS;
        r->mask = mask & FSWATCH_IN_MASK_ADD ? r->mask | keep : keep;
        r->flags = flags;
        *out_id = r->id;
        *out_existed = 1;
        return OK;
    }
    if (t->watches >= FSW_WATCHES || t->chans[c].watches >= FSW_PER_CHANNEL)
        return ERR_NO_SPACE;
    uint32_t slot = t->next;
    while (t->w[slot].chan)
        slot = (slot + 1) % FSW_WATCHES;
    struct fsw_watch_rec *r = &t->w[slot];
    uint32_t count = (r->id >> FSW_SLOT_BITS) + 1;
    if (count > FSW_COUNT_MAX)
        count = 1;
    *r = (struct fsw_watch_rec){ .ino = key->ino, .gen = key->gen, .mask = keep,
                                 .id = slot | count << FSW_SLOT_BITS,
                                 .chan = (uint16_t)(c + 1), .flags = flags };
    t->watches++;
    t->chans[c].watches++;
    t->next = (slot + 1) % FSW_WATCHES;
    index_add(w, slot);
    *out_id = r->id;
    *out_existed = 0;
    return OK;
}

status_t fsw_remove(struct fsw *w, uint32_t c, uint32_t id)
{
    if (!live(w, c))
        return ERR_BAD_STATE;
    uint32_t slot = id & SLOT_MASK;
    const struct fsw_watch_rec *r = &w->t->w[slot];
    if (!id || r->chan != c + 1 || r->id != id)
        return OK;   /* not a watch of c now: ended already (a request run again), or never */
    end_watch(w, slot, 0);   /* its events queued already stay, before IN_IGNORED (Linux's) */
    return OK;
}

status_t fsw_take(struct fsw *w, uint32_t c, uint32_t room, uint8_t *out_more, uint8_t *out,
                  uint32_t *out_len)
{
    if (!live(w, c))
        return ERR_BAD_STATE;
    struct fsw_chan *q = &w->chans[c];
    uint32_t n = 0;
    if (room > FSWATCH_TAKE_BYTES)
        room = FSWATCH_TAKE_BYTES;
    while (q->head && n + rec_bytes(q->head->len) <= room) {
        struct fsw_event *e = q->head;
        uint8_t *p = out + n;
        memcpy(p, &e->id, 4);
        memcpy(p + 4, &e->mask, 4);
        memcpy(p + 8, &e->cookie, 4);
        memcpy(p + 12, &e->len, 2);
        memcpy(p + FSWATCH_RECORD_FIXED, e + 1, e->len);
        n += rec_bytes(e->len);
        q->bytes -= rec_bytes(e->len);
        q->head = e->next;
        if (!q->head)
            q->tail = NULL;
        free_event(q, e);
    }
    if (!n && q->head)
        return ERR_BUFFER_TOO_SMALL;
    *out_more = q->head != NULL;
    *out_len = n;
    return OK;
}

/* ---- Events ------------------------------------------------------------------ */

void fsw_dir_event(struct fsw *w, const struct fsw_key *dir, const char *name, uint32_t len,
                   uint32_t mask, uint32_t cookie, uint32_t flags)
{
    if (!fsw_watching(w) || !dir)
        return;
    uint32_t m = (mask & EVENTS) | (flags & FSW_EV_DIR ? FSWATCH_IN_ISDIR : 0);
    uint16_t next;
    for (uint16_t s = w->bucket[hash(dir->ino, dir->gen)]; s; s = next) {
        next = w->chain[s - 1];   /* before deliver: IN_ONESHOT ends the watch */
        const struct fsw_watch_rec *r = &w->t->w[s - 1];
        if (!same(r, dir) || !(r->mask & mask & EVENTS))
            continue;
        if ((flags & FSW_EV_UNLINKED) && (r->mask & FSWATCH_IN_EXCL_UNLINK))
            continue;
        const struct fsw_chan_rec *rec = &w->t->chans[r->chan - 1];
        if (w->hidden && rec->view && w->hidden(w->ctx, rec, dir, name, len))
            continue;
        deliver(w, s - 1, m, cookie, name, len);
    }
}

void fsw_self_event(struct fsw *w, const struct fsw_key *self, uint32_t mask, uint32_t flags)
{
    if (!fsw_watching(w) || !self)
        return;
    uint32_t m = (mask & EVENTS) | (flags & FSW_EV_DIR ? FSWATCH_IN_ISDIR : 0);
    uint16_t next;
    for (uint16_t s = w->bucket[hash(self->ino, self->gen)]; s; s = next) {
        next = w->chain[s - 1];
        const struct fsw_watch_rec *r = &w->t->w[s - 1];
        if (same(r, self) && (r->mask & mask & EVENTS))
            deliver(w, s - 1, m, 0, NULL, 0);
    }
}

void fsw_event(struct fsw *w, const struct fsw_key *dir, const char *name, uint32_t len,
               const struct fsw_key *self, uint32_t mask, uint32_t flags)
{
    fsw_dir_event(w, dir, name, len, mask, 0, flags);
    fsw_self_event(w, self, mask, flags);
}

void fsw_gone(struct fsw *w, const struct fsw_key *key)
{
    if (!fsw_watching(w))
        return;
    uint16_t next;
    for (uint16_t s = w->bucket[hash(key->ino, key->gen)]; s; s = next) {
        next = w->chain[s - 1];
        if (same(&w->t->w[s - 1], key))
            end_watch(w, s - 1, FSWATCH_IN_DELETE_SELF);
    }
}

void fsw_end(struct fsw *w, uint32_t slot, bool moved)
{
    if (slot < FSW_WATCHES && w->t->w[slot].chan)
        end_watch(w, slot, moved ? FSWATCH_IN_MOVE_SELF : 0);
}

uint32_t fsw_cookie(struct fsw *w)
{
    if (++w->cookie == 0)
        w->cookie = 1;
    return w->cookie;
}

void fsw_each(struct fsw *w, const struct fsw_key *key, void (*fn)(void *ctx, uint32_t slot),
              void *ctx)
{
    uint16_t next;
    for (uint16_t s = w->bucket[hash(key->ino, key->gen)]; s; s = next) {
        next = w->chain[s - 1];
        if (same(&w->t->w[s - 1], key))
            fn(ctx, s - 1);
    }
}

/* ---- Order ------------------------------------------------------------------- */

status_t fsw_order_next(struct fsw_order *o, handle_t port, uint64_t deadline,
                        struct port_packet *out)
{
    if (!o->n)
        return jam_port_wait(port, deadline, out);
    *out = o->q[o->head];
    o->head = (o->head + 1) % o->cap;
    o->n--;
    return OK;
}

uint32_t fsw_order_drain(struct fsw_order *o, handle_t port)
{
    uint32_t got = 0;
    while (o->n < o->cap) {
        struct port_packet p;
        if (jam_port_wait(port, 0, &p) != OK)
            break;
        o->q[(o->head + o->n) % o->cap] = p;
        o->n++;
        got++;
    }
    return got;
}

bool fsw_order_pick(struct fsw_order *o, bool (*pick)(void *ctx, const struct port_packet *p),
                    void *ctx, struct port_packet *out)
{
    for (uint32_t i = 0; i < o->n; i++) {
        if (!pick(ctx, &o->q[(o->head + i) % o->cap]))
            continue;
        *out = o->q[(o->head + i) % o->cap];
        for (uint32_t j = i; j + 1 < o->n; j++)
            o->q[(o->head + j) % o->cap] = o->q[(o->head + j + 1) % o->cap];
        o->n--;
        return true;
    }
    return false;
}
