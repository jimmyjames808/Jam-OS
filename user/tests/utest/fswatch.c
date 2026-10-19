/* utest: file watching's library (<fswatch.h>, user/lib/fswatch.c) in a
 * fake service in this process: inodes from a table, the watcher channels
 * real ones, requests through the generated fswatch code
 * (fswatch_protocol), events said by the test. docs/INOTIFY-PLAN.md, IP. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fswatch.h>
#include <os.h>
#include "utest.h"

#define IN(x) FSWATCH_IN_##x

/* Shared by the cases (big: kept out of the stack). */
static struct fsw_table tab, tab2;
static struct fsw w, w2;
static uint8_t takebuf[FSWATCH_TAKE_BYTES];

/* The fake volume's inodes. */
static const struct fsw_key K_ROOT = { 2, 0 }, K_D = { 10, 1 }, K_F = { 11, 1 }, K_E = { 12, 1 },
                            K_G = { 13, 1 };
static const struct {
    const char    *path;
    struct fsw_key key;
    bool           dir;
} inodes[] = {
    { "/", { 2, 0 }, true },    { "/d", { 10, 1 }, true }, { "/d/f", { 11, 1 }, false },
    { "/e", { 12, 1 }, true },  { "/g", { 13, 1 }, false },
};

/* A record taken. */
struct rec {
    uint32_t id, mask, cookie, len;
    char     name[64];
};

/* ---- Helpers ----------------------------------------------------------------- */

/* A watcher channel of process pid on f through a view with these flags,
 * its bell given (the client's own event in *bell): *client, *c. */
static bool watcher(struct fsw *f, uint32_t pid, uint32_t view, handle_t *client, handle_t *bell,
                    uint32_t *c)
{
    handle_t srv, give;
    CHECK_ST(jam_channel_create(client, &srv), OK);
    CHECK_ST(fsw_chan_new(f, pid, view, &K_ROOT, srv, c), OK);
    CHECK_ST(jam_event_create(bell), OK);
    CHECK_ST(jam_handle_duplicate(*bell, RIGHT_SIGNAL | RIGHT_TRANSFER, &give), OK);
    CHECK_ST(fswatch_bell_give(*client, give), OK);
    CHECK_ST(fsw_chan_bell(f, *c), OK);
    CHECK(fsw_belled(f, *c));
    return true;
}

static void unwatcher(struct fsw *f, uint32_t c, handle_t client, handle_t bell)
{
    fsw_chan_close(f, c);
    jam_handle_close(client);
    jam_handle_close(bell);
}

static bool rung(handle_t bell)
{
    return jam_object_wait_one(bell, FSWATCH_BELL, 0, NULL) == OK;
}

static void unring(handle_t bell)
{
    (void)jam_event_signal(bell, FSWATCH_BELL, 0);
}

/* Decode take's n bytes of records into r (room for max): how many, or
 * -1 if they are malformed. */
static int decode(const uint8_t *b, uint32_t n, struct rec *r, int max)
{
    int k = 0;
    for (uint32_t o = 0; o < n; k++) {
        if (n - o < FSWATCH_RECORD_FIXED || k == max)
            return -1;
        uint16_t len;
        memcpy(&r[k].id, b + o, 4);
        memcpy(&r[k].mask, b + o + 4, 4);
        memcpy(&r[k].cookie, b + o + 8, 4);
        memcpy(&len, b + o + 12, 2);
        o += FSWATCH_RECORD_FIXED;
        if (len >= sizeof(r[k].name) || n - o < len)
            return -1;
        r[k].len = len;
        memcpy(r[k].name, b + o, len);
        r[k].name[len] = 0;
        o += len;
    }
    return k;
}

/* Every record queued on channel c (takes until `more` is 0): how many in
 * *n, or false. */
static bool take_all(struct fsw *f, uint32_t c, struct rec *r, int max, int *n)
{
    uint8_t more = 1;
    *n = 0;
    while (more) {
        uint32_t len = 0;
        CHECK_ST(fsw_take(f, c, FSWATCH_TAKE_BYTES, &more, takebuf, &len), OK);
        int k = decode(takebuf, len, r + *n, max - *n);
        CHECK(k >= 0);
        CHECK(k > 0 || !more);
        *n += k;
    }
    return true;
}

static bool is(const struct rec *r, uint32_t id, uint32_t mask, const char *name)
{
    return r->id == id && r->mask == mask && r->len == strlen(name) && !strcmp(r->name, name);
}

static uint32_t add(struct fsw *f, uint32_t c, const struct fsw_key *k, bool dir, uint32_t mask)
{
    uint32_t id = 0;
    uint8_t existed;
    return fsw_add(f, c, k, dir, mask, &id, &existed) == OK ? id : 0;
}

/* ---- The fake service's fswatch handlers ------------------------------------- */

struct fake {
    struct fsw *f;
    uint32_t    c;
};

static status_t op_add(void *ctx, const char *path, uint32_t path_len, uint32_t mask,
                       uint32_t *out_id, uint8_t *out_existed)
{
    struct fake *s = ctx;
    status_t st = fsw_mask_check(mask);
    if (st != OK)
        return st;
    for (unsigned i = 0; i < sizeof(inodes) / sizeof(inodes[0]); i++)
        if (strlen(inodes[i].path) == path_len && !memcmp(inodes[i].path, path, path_len))
            return fsw_add(s->f, s->c, &inodes[i].key, inodes[i].dir, mask, out_id, out_existed);
    return ERR_NOT_FOUND;
}

static status_t op_remove(void *ctx, uint32_t id)
{
    struct fake *s = ctx;
    return fsw_remove(s->f, s->c, id);
}

static status_t op_take(void *ctx, uint32_t room, uint8_t *out_more, uint8_t *out_events,
                        uint32_t *out_events_len)
{
    struct fake *s = ctx;
    return fsw_take(s->f, s->c, room, out_more, out_events, out_events_len);
}

static const struct fswatch_ops fake_ops = { .add = op_add, .remove = op_remove, .take = op_take };
static uint32_t txid;
static uint8_t  rep[FSWATCH_REP_MAX];

/* The request just sent on client: the fake service serves it, then its
 * reply is read into rep (m). */
static status_t serve(struct fake *s, handle_t client, struct idl_msg *m)
{
    status_t st = fswatch_serve_one(s->f->chans[s->c].ch, &fake_ops, s);
    return st == OK ? idl_reply_read(client, rep, sizeof(rep), m) : st;
}

static status_t c_add(struct fake *s, handle_t client, const char *path, uint32_t mask,
                      uint32_t *id, uint8_t *existed)
{
    struct idl_msg m;
    status_t st = fswatch_add_send(client, idl_txid_next(&txid), path, (uint32_t)strlen(path), mask);
    if (st == OK)
        st = serve(s, client, &m);
    return st == OK ? fswatch_add_result(rep, &m, id, existed) : st;
}

static status_t c_remove(struct fake *s, handle_t client, uint32_t id)
{
    struct idl_msg m;
    status_t st = fswatch_remove_send(client, idl_txid_next(&txid), id);
    if (st == OK)
        st = serve(s, client, &m);
    return st == OK ? fswatch_remove_result(rep, &m) : st;
}

static status_t c_take(struct fake *s, handle_t client, uint32_t room, uint8_t *more, uint8_t *out,
                       uint32_t *len)
{
    struct idl_msg m;
    status_t st = fswatch_take_send(client, idl_txid_next(&txid), room);
    if (st == OK)
        st = serve(s, client, &m);
    return st == OK ? fswatch_take_result(rep, &m, more, out, len) : st;
}

/* ---- Cases ------------------------------------------------------------------- */

/* The protocol end to end: add, the bell, take's records, remove. */
bool t_fswatch_protocol(void)
{
    static uint8_t out[FSWATCH_TAKE_BYTES];
    struct fake s = { .f = &w };
    struct rec r[8];
    handle_t client, bell;
    uint32_t idd, idf, again, len;
    uint8_t existed, more;
    fsw_init(&w, &tab);
    CHECK(!fsw_watching(&w));
    CHECK(watcher(&w, 7, 0, &client, &bell, &s.c));
    CHECK_ST(c_add(&s, client, "/d", IN(ALL_EVENTS), &idd, &existed), OK);
    CHECK(idd && !existed);
    CHECK_ST(c_add(&s, client, "/d/f", IN(MODIFY) | IN(CLOSE_WRITE), &idf, &existed), OK);
    CHECK(idf && idf != idd && !existed);
    CHECK_ST(c_add(&s, client, "/d", IN(CREATE), &again, &existed), OK);
    CHECK(again == idd && existed);
    CHECK_ST(c_add(&s, client, "/d", IN(ALL_EVENTS), &again, &existed), OK);
    CHECK(fsw_watching(&w));
    CHECK_ST(c_add(&s, client, "/nothing", IN(MODIFY), &again, &existed), ERR_NOT_FOUND);
    CHECK_ST(c_add(&s, client, "/d", 0, &again, &existed), ERR_INVALID_ARGS);
    CHECK_ST(c_add(&s, client, "/d/f", IN(MODIFY) | IN(ONLYDIR), &again, &existed), ERR_NOT_DIR);

    /* A write to /d/f: the directory's record (with the name), then the
     * file's own; the bell rung once. */
    CHECK(!rung(bell));
    fsw_event(&w, &K_D, "f", 1, &K_F, IN(MODIFY), 0);
    CHECK(rung(bell));
    unring(bell);
    CHECK_ST(c_take(&s, client, FSWATCH_TAKE_BYTES, &more, out, &len), OK);
    CHECK_EQ(decode(out, len, r, 8), 2);
    CHECK(is(&r[0], idd, IN(MODIFY), "f") && r[0].cookie == 0);
    CHECK(is(&r[1], idf, IN(MODIFY), ""));
    CHECK_EQ(more, 0);
    CHECK_ST(c_take(&s, client, FSWATCH_TAKE_BYTES, &more, out, &len), OK);
    CHECK(len == 0 && !more && !rung(bell));

    /* A room too small for the first record, and one for exactly one. */
    fsw_event(&w, &K_D, "f", 1, &K_F, IN(MODIFY), 0);
    CHECK(rung(bell));
    unring(bell);
    CHECK_ST(c_take(&s, client, FSWATCH_RECORD_FIXED, &more, out, &len), ERR_BUFFER_TOO_SMALL);
    CHECK_ST(c_take(&s, client, FSWATCH_RECORD_FIXED + 1, &more, out, &len), OK);
    CHECK(len == FSWATCH_RECORD_FIXED + 1 && more);
    CHECK(!rung(bell));   /* the queue never went empty */

    /* remove: the watch's queued events kept (Linux's), IN_IGNORED last;
     * nothing of it after that. */
    fsw_self_event(&w, &K_F, IN(CLOSE_WRITE), 0);
    fsw_event(&w, &K_D, "f", 1, &K_F, IN(ATTRIB), 0);   /* only /d asked for it */
    CHECK_ST(c_remove(&s, client, idf), OK);
    fsw_self_event(&w, &K_F, IN(CLOSE_WRITE), 0);
    CHECK_ST(c_take(&s, client, FSWATCH_TAKE_BYTES, &more, out, &len), OK);
    CHECK_EQ(decode(out, len, r, 8), 4);
    CHECK(is(&r[0], idf, IN(MODIFY), ""));   /* the one a too small room left */
    CHECK(is(&r[1], idf, IN(CLOSE_WRITE), ""));
    CHECK(is(&r[2], idd, IN(ATTRIB), "f"));
    CHECK(is(&r[3], idf, IN(IGNORED), ""));
    CHECK_ST(c_remove(&s, client, idf), OK);   /* gone: nothing done */
    CHECK_ST(c_remove(&s, client, 0), OK);
    CHECK_ST(c_take(&s, client, FSWATCH_TAKE_BYTES, &more, out, &len), OK);
    CHECK_EQ(len, 0);
    CHECK_EQ(tab.watches, 1);

    unwatcher(&w, s.c, client, bell);
    CHECK_EQ(tab.watches, 0);
    CHECK(!fsw_watching(&w));
    return true;
}

/* The bell is the channel's first message, or the channel ends. */
bool t_fswatch_bell(void)
{
    handle_t client, srv, ev, give, other, end;
    uint32_t c;
    fsw_init(&w, &tab);

    /* Nothing yet; then a call first (too big for a give). */
    CHECK_ST(jam_channel_create(&client, &srv), OK);
    CHECK_ST(fsw_chan_new(&w, 1, 0, &K_ROOT, srv, &c), OK);
    CHECK(!fsw_belled(&w, c));
    CHECK_ST(fsw_chan_bell(&w, c), ERR_SHOULD_WAIT);
    CHECK_ST(fswatch_take_send(client, 1, FSWATCH_TAKE_BYTES), OK);
    CHECK_ST(fsw_chan_bell(&w, c), ERR_INVALID_ARGS);
    fsw_chan_close(&w, c);
    CHECK_ST(jam_object_wait_one(client, SIG_PEER_CLOSED, 0, NULL), OK);
    jam_handle_close(client);

    /* A give's bytes with a txid: a call, not the give. */
    CHECK_ST(jam_channel_create(&client, &srv), OK);
    CHECK_ST(fsw_chan_new(&w, 1, 0, &K_ROOT, srv, &c), OK);
    struct idl_req_hdr h = { .txid = 5, .ordinal = FSWATCH_BELL_GIVE };
    CHECK_ST(jam_event_create(&ev), OK);
    CHECK_ST(drv_channel_write(client, &h, sizeof(h), &ev, 1), OK);
    CHECK_ST(fsw_chan_bell(&w, c), ERR_INVALID_ARGS);
    fsw_chan_close(&w, c);
    jam_handle_close(client);

    /* Not an event. */
    CHECK_ST(jam_channel_create(&client, &srv), OK);
    CHECK_ST(fsw_chan_new(&w, 1, 0, &K_ROOT, srv, &c), OK);
    CHECK_ST(jam_channel_create(&other, &end), OK);
    CHECK_ST(fswatch_bell_give(client, other), OK);
    CHECK_ST(fsw_chan_bell(&w, c), ERR_WRONG_TYPE);
    CHECK_ST(jam_object_wait_one(end, SIG_PEER_CLOSED, 0, NULL), OK);   /* closed by the library */
    jam_handle_close(end);
    fsw_chan_close(&w, c);
    jam_handle_close(client);

    /* An event the service may not ring. */
    CHECK_ST(jam_channel_create(&client, &srv), OK);
    CHECK_ST(fsw_chan_new(&w, 1, 0, &K_ROOT, srv, &c), OK);
    CHECK_ST(jam_event_create(&ev), OK);
    CHECK_ST(jam_handle_duplicate(ev, RIGHT_WAIT | RIGHT_TRANSFER, &give), OK);
    CHECK_ST(fswatch_bell_give(client, give), OK);
    CHECK_ST(fsw_chan_bell(&w, c), ERR_ACCESS_DENIED);
    CHECK(!fsw_belled(&w, c));
    fsw_chan_close(&w, c);
    jam_handle_close(client);

    /* The client gone before its bell. */
    CHECK_ST(jam_channel_create(&client, &srv), OK);
    CHECK_ST(fsw_chan_new(&w, 1, 0, &K_ROOT, srv, &c), OK);
    jam_handle_close(client);
    CHECK_ST(fsw_chan_bell(&w, c), ERR_PEER_CLOSED);
    fsw_chan_close(&w, c);

    /* A good one, and a second give: the channel has its bell already. */
    CHECK(watcher(&w, 1, 0, &client, &give, &c));
    CHECK_ST(fsw_chan_bell(&w, c), ERR_BAD_STATE);
    CHECK_EQ(tab.chans[c].belled, 1);
    unwatcher(&w, c, client, give);
    jam_handle_close(ev);
    for (uint32_t i = 0; i < FSW_CHANNELS; i++)
        CHECK(!tab.chans[i].live);
    return true;
}

/* Linux's merge rule, the one overflow, the bell once per empty queue. */
bool t_fswatch_queue(void)
{
    static struct rec r[1200];
    handle_t client, bell;
    uint32_t c;
    int n;
    char name[8];
    fsw_init(&w, &tab);
    CHECK(watcher(&w, 1, 0, &client, &bell, &c));
    uint32_t id = add(&w, c, &K_D, true, IN(ALL_EVENTS));
    CHECK(id);

    /* Equal to the last one queued: dropped. Only the last is compared. */
    fsw_dir_event(&w, &K_D, "a", 1, IN(MODIFY), 0, 0);
    fsw_dir_event(&w, &K_D, "a", 1, IN(MODIFY), 0, 0);
    fsw_dir_event(&w, &K_D, "b", 1, IN(MODIFY), 0, 0);
    fsw_dir_event(&w, &K_D, "a", 1, IN(MODIFY), 0, 0);
    fsw_dir_event(&w, &K_D, "a", 1, IN(ACCESS), 0, 0);
    fsw_dir_event(&w, &K_D, "a", 1, IN(ACCESS), 7, 0);   /* another cookie */
    fsw_dir_event(&w, &K_D, "a", 1, IN(ACCESS), 7, 0);
    CHECK(take_all(&w, c, r, 1200, &n));
    CHECK_EQ(n, 5);
    CHECK(is(&r[0], id, IN(MODIFY), "a") && is(&r[1], id, IN(MODIFY), "b"));
    CHECK(is(&r[2], id, IN(MODIFY), "a") && is(&r[3], id, IN(ACCESS), "a"));
    CHECK(is(&r[4], id, IN(ACCESS), "a") && r[4].cookie == 7 && r[3].cookie == 0);

    /* The bell: rung when the empty queue got its first event, not again
     * while it held events. */
    unring(bell);
    fsw_dir_event(&w, &K_D, "a", 1, IN(MODIFY), 0, 0);
    CHECK(rung(bell));
    unring(bell);
    fsw_dir_event(&w, &K_D, "b", 1, IN(MODIFY), 0, 0);
    CHECK(!rung(bell));
    CHECK(take_all(&w, c, r, 1200, &n));
    CHECK(!rung(bell));
    fsw_dir_event(&w, &K_D, "c", 1, IN(MODIFY), 0, 0);
    CHECK(rung(bell));
    CHECK(take_all(&w, c, r, 1200, &n));

    /* Records of 19 bytes: 862 fit in FSWATCH_QUEUE_BYTES; the next is the
     * one overflow, the rest dropped. */
    uint32_t fit = FSWATCH_QUEUE_BYTES / (FSWATCH_RECORD_FIXED + 5);
    for (uint32_t i = 0; i < fit + 100; i++) {
        snprintf(name, sizeof(name), "n%04u", i);
        fsw_dir_event(&w, &K_D, name, 5, IN(CREATE), 0, 0);
    }
    CHECK_EQ(w.chans[c].bytes, fit * (FSWATCH_RECORD_FIXED + 5) + FSWATCH_RECORD_FIXED);
    uint32_t len = 0;
    uint8_t more;
    CHECK_ST(fsw_take(&w, c, 100 * (FSWATCH_RECORD_FIXED + 5), &more, takebuf, &len), OK);
    CHECK(more && len == 100 * (FSWATCH_RECORD_FIXED + 5));
    /* Room again: events come behind the overflow (its record counts),
     * which is not queued twice when the queue fills again. */
    uint32_t more_fit = (FSWATCH_QUEUE_BYTES - (fit - 100) * 19 - FSWATCH_RECORD_FIXED) / 19;
    for (uint32_t i = 0; i < 200; i++) {
        snprintf(name, sizeof(name), "m%04u", i);
        fsw_dir_event(&w, &K_D, name, 5, IN(DELETE), 0, 0);
    }
    CHECK(take_all(&w, c, r, 1200, &n));
    CHECK_EQ(n, (int)(fit - 100 + 1 + more_fit));
    CHECK(is(&r[0], id, IN(CREATE), "n0100"));
    snprintf(name, sizeof(name), "n%04u", fit - 1);
    CHECK(is(&r[fit - 101], id, IN(CREATE), name));
    CHECK(is(&r[fit - 100], 0, IN(Q_OVERFLOW), "") && r[fit - 100].cookie == 0);
    CHECK(is(&r[fit - 99], id, IN(DELETE), "m0000"));
    snprintf(name, sizeof(name), "m%04u", more_fit - 1);
    CHECK(is(&r[n - 1], id, IN(DELETE), name));
    for (int i = 0; i < n; i++)
        CHECK(i == (int)(fit - 100) || r[i].mask != IN(Q_OVERFLOW));
    CHECK_EQ(w.chans[c].bytes, 0);
    /* Taken, the overflow may come again. */
    for (uint32_t i = 0; i < fit + 1; i++) {
        snprintf(name, sizeof(name), "o%04u", i);
        fsw_dir_event(&w, &K_D, name, 5, IN(CREATE), 0, 0);
    }
    CHECK(take_all(&w, c, r, 1200, &n));
    CHECK_EQ(n, (int)fit + 1);
    CHECK(is(&r[fit], 0, IN(Q_OVERFLOW), ""));

    /* take's room: never past FSWATCH_TAKE_BYTES, whole records only. */
    for (uint32_t i = 0; i < 600; i++) {
        snprintf(name, sizeof(name), "p%04u", i);
        fsw_dir_event(&w, &K_D, name, 5, IN(CREATE), 0, 0);
    }
    CHECK_ST(fsw_take(&w, c, 1u << 20, &more, takebuf, &len), OK);
    CHECK(more && len == (FSWATCH_TAKE_BYTES / 19) * 19);
    CHECK_ST(fsw_take(&w, c, 18, &more, takebuf, &len), ERR_BUFFER_TOO_SMALL);
    CHECK_ST(fsw_take(&w, c, 0, &more, takebuf, &len), ERR_BUFFER_TOO_SMALL);
    CHECK(take_all(&w, c, r, 1200, &n));
    CHECK_EQ(n, 600 - (int)(FSWATCH_TAKE_BYTES / 19));
    unwatcher(&w, c, client, bell);
    return true;
}

/* ids: one per inode on a channel, add's flags, never given again soon. */
bool t_fswatch_ids(void)
{
    static struct rec r[16];
    static uint32_t seen[FSW_WATCHES];
    handle_t client, bell, client2, bell2;
    uint32_t c, c2, id, id2;
    uint8_t existed;
    int n;
    fsw_init(&w, &tab);
    CHECK(watcher(&w, 1, 0, &client, &bell, &c));
    CHECK(watcher(&w, 1, 0, &client2, &bell2, &c2));

    /* The same inode: one id per channel. */
    id = add(&w, c, &K_D, true, IN(CREATE));
    id2 = add(&w, c2, &K_D, true, IN(CREATE));
    CHECK(id && id2 && id != id2);
    CHECK_ST(fsw_add(&w, c, &K_D, true, IN(DELETE) | IN(MASK_ADD), &id2, &existed), OK);
    CHECK(id2 == id && existed);
    fsw_dir_event(&w, &K_D, "x", 1, IN(CREATE), 0, 0);
    fsw_dir_event(&w, &K_D, "x", 1, IN(DELETE), 0, 0);
    CHECK(take_all(&w, c, r, 16, &n));
    CHECK_EQ(n, 2);   /* both: the mask was added to */
    CHECK_ST(fsw_add(&w, c, &K_D, true, IN(MODIFY), &id2, &existed), OK);
    fsw_dir_event(&w, &K_D, "y", 1, IN(CREATE), 0, 0);
    CHECK(take_all(&w, c, r, 16, &n));
    CHECK_EQ(n, 0);   /* replaced */
    CHECK(take_all(&w, c2, r, 16, &n));
    CHECK_EQ(n, 2);   /* the other channel's watch kept its own mask */
    CHECK(is(&r[0], add(&w, c2, &K_D, true, IN(CREATE)), IN(CREATE), "x"));
    CHECK(is(&r[1], r[0].id, IN(CREATE), "y"));

    /* add's checks. */
    CHECK_ST(fsw_add(&w, c, &K_D, true, IN(CREATE) | IN(MASK_CREATE), &id2, &existed),
             ERR_ALREADY_EXISTS);
    CHECK_ST(fsw_add(&w, c, &K_E, true, IN(CREATE) | IN(MASK_CREATE), &id2, &existed), OK);
    CHECK(!existed);
    CHECK_ST(fsw_add(&w, c, &K_G, false, IN(MODIFY) | IN(ONLYDIR), &id2, &existed), ERR_NOT_DIR);
    CHECK_ST(fsw_add(&w, c, &K_G, false, 0, &id2, &existed), ERR_INVALID_ARGS);
    CHECK_ST(fsw_add(&w, c, &K_G, false, 1u << 12, &id2, &existed), ERR_INVALID_ARGS);
    CHECK_ST(fsw_add(&w, c, &K_G, false, IN(MODIFY) | IN(MASK_ADD) | IN(MASK_CREATE), &id2,
                     &existed), ERR_INVALID_ARGS);
    CHECK_ST(fsw_add(&w, c, &K_E, true, IN(ONLYDIR), &id2, &existed), OK);   /* Linux takes it */
    CHECK_ST(fsw_mask_check(IN(MODIFY) | (1u << 12)), OK);   /* other bits ignored */

    /* An ended watch's id isn't given again: 3 x FSW_WATCHES adds and
     * removes, each id new, every slot used in turn. */
    fsw_chan_close(&w, c2);
    jam_handle_close(client2);
    jam_handle_close(bell2);
    memset(seen, 0, sizeof(seen));
    for (uint32_t i = 0; i < 3 * FSW_WATCHES; i++) {
        struct fsw_key k = { 1000 + i, 1 };
        id = add(&w, c, &k, false, IN(MODIFY));
        CHECK(id);
        uint32_t slot = id & ((1u << FSW_SLOT_BITS) - 1);
        CHECK(id != seen[slot]);
        CHECK(!seen[slot] || (id >> FSW_SLOT_BITS) == (seen[slot] >> FSW_SLOT_BITS) + 1);
        seen[slot] = id;
        CHECK_ST(fsw_remove(&w, c, id), OK);
        CHECK_ST(fsw_remove(&w, c, id), OK);
    }
    /* A slot's count wraps past FSW_COUNT_MAX to 1, never 0. */
    uint32_t slot = tab.next;
    while (tab.w[slot].chan)
        slot = (slot + 1) % FSW_WATCHES;
    tab.w[slot].id = slot | FSW_COUNT_MAX << FSW_SLOT_BITS;
    struct fsw_key k = { 99, 9 };
    CHECK_EQ(add(&w, c, &k, false, IN(MODIFY)), slot | 1u << FSW_SLOT_BITS);
    unwatcher(&w, c, client, bell);
    return true;
}

/* The limits: channels per volume and per process, watches per process,
 * per channel and per volume. */
bool t_fswatch_caps(void)
{
    static handle_t clients[FSW_CHANNELS], bells[FSW_CHANNELS];
    static uint32_t cs[FSW_CHANNELS];
    handle_t client, srv;
    uint32_t c, id;
    uint8_t existed;
    fsw_init(&w, &tab);
    for (uint32_t i = 0; i < FSW_PER_PROCESS; i++)
        CHECK(watcher(&w, 5, 0, &clients[i], &bells[i], &cs[i]));
    CHECK_ST(jam_channel_create(&client, &srv), OK);
    CHECK_ST(fsw_chan_new(&w, 5, 0, &K_ROOT, srv, &c), ERR_NO_RESOURCES);
    CHECK_ST(jam_object_wait_one(client, SIG_PEER_CLOSED, 0, NULL), OK);   /* srv closed */
    jam_handle_close(client);
    for (uint32_t i = FSW_PER_PROCESS; i < FSW_CHANNELS; i++)
        CHECK(watcher(&w, 100 + i / FSW_PER_PROCESS, 0, &clients[i], &bells[i], &cs[i]));
    CHECK_ST(jam_channel_create(&client, &srv), OK);
    CHECK_ST(fsw_chan_new(&w, 99, 0, &K_ROOT, srv, &c), ERR_NO_RESOURCES);
    jam_handle_close(client);

    /* A process with its share closed one: room for another. */
    unwatcher(&w, cs[0], clients[0], bells[0]);
    CHECK(watcher(&w, 5, 0, &clients[0], &bells[0], &cs[0]));
    for (uint32_t i = 0; i < FSW_CHANNELS; i++)
        unwatcher(&w, cs[i], clients[i], bells[i]);

    /* Watches: FSW_PROCESS_WATCHES on all of a process's channels (IR's 5:
     * the first two of pid 5 share it), FSW_PER_CHANNEL on one, FSW_WATCHES
     * on the volume (a channel of each of 8 more processes fills it). */
    const uint32_t share = FSW_WATCHES / FSW_PROCESS_WATCHES;
    _Static_assert(FSW_WATCHES / FSW_PROCESS_WATCHES + 3 <= FSW_CHANNELS, "channels for each");
    for (uint32_t i = 0; i < share + 3; i++)
        CHECK(watcher(&w, i < 2 ? 5 : i < 3 ? 0 : 200 + i, 0, &clients[i], &bells[i], &cs[i]));
    struct fsw_key k = { 1, 1 };
    uint32_t id1 = 0;
    for (uint32_t j = 0; j < FSW_PROCESS_WATCHES; j++) {
        struct fsw_key kj = { 5000 + j, 1 };
        CHECK_ST(fsw_add(&w, cs[j & 1], &kj, false, IN(MODIFY), &id, &existed), OK);
        if (j == 1)
            id1 = id;
    }
    CHECK_ST(fsw_add(&w, cs[0], &k, false, IN(MODIFY), &id, &existed), ERR_NO_SPACE);
    CHECK_ST(fsw_add(&w, cs[1], &k, false, IN(MODIFY), &id, &existed), ERR_NO_SPACE);
    /* An existing watch's mask may still change. */
    struct fsw_key first = { 5000, 1 };
    CHECK_ST(fsw_add(&w, cs[0], &first, false, IN(ATTRIB), &id, &existed), OK);
    CHECK(existed);
    /* pid 0 (not known): its channel's FSW_PER_CHANNEL alone. */
    for (uint32_t j = 0; j < FSW_PER_CHANNEL; j++) {
        struct fsw_key kj = { 5000 + j, 1 };
        CHECK_ST(fsw_add(&w, cs[2], &kj, false, IN(MODIFY), &id, &existed), OK);
    }
    CHECK_ST(fsw_add(&w, cs[2], &k, false, IN(MODIFY), &id, &existed), ERR_NO_SPACE);
    unwatcher(&w, cs[2], clients[2], bells[2]);
    for (uint32_t i = 3; i < share + 2; i++) {
        for (uint32_t j = 0; j < FSW_PROCESS_WATCHES; j++) {
            struct fsw_key kj = { 5000 + i * FSW_PROCESS_WATCHES + j, 1 };
            CHECK_ST(fsw_add(&w, cs[i], &kj, false, IN(MODIFY), &id, &existed), OK);
        }
    }
    CHECK_EQ(tab.watches, FSW_WATCHES);
    CHECK_ST(fsw_add(&w, cs[share + 2], &k, false, IN(MODIFY), &id, &existed), ERR_NO_SPACE);
    /* A watch removed: room for one on the volume, and in pid 5's share. */
    CHECK_ST(fsw_remove(&w, cs[1], id1), OK);
    CHECK_ST(fsw_add(&w, cs[0], &k, false, IN(MODIFY), &id, &existed), OK);
    CHECK(!existed);
    CHECK_EQ(tab.watches, FSW_WATCHES);
    for (uint32_t i = 0; i < share + 3; i++)
        if (i != 2)
            unwatcher(&w, cs[i], clients[i], bells[i]);
    CHECK_EQ(tab.watches, 0);

    /* pid 0 (not known) is no process's share. */
    for (uint32_t i = 0; i < FSW_PER_PROCESS + 2; i++)
        CHECK(watcher(&w, 0, 0, &clients[i], &bells[i], &cs[i]));
    for (uint32_t i = 0; i < FSW_PER_PROCESS + 2; i++)
        unwatcher(&w, cs[i], clients[i], bells[i]);
    return true;
}

static bool hide_secret(void *ctx, const struct fsw_chan_rec *rec, const struct fsw_key *dir,
                        const char *name, uint32_t len)
{
    (void)ctx;
    (void)dir;
    return (rec->view & 4) && len == 6 && !memcmp(name, "secret", 6);
}

static void count_one(void *ctx, uint32_t slot)
{
    (void)slot;
    (*(int *)ctx)++;
}

/* The event rules: masks, IN_ISDIR, the order, IN_ONESHOT, IN_EXCL_UNLINK,
 * an inode gone, a watch ended, cookies, the hidden-name filter. */
bool t_fswatch_rules(void)
{
    static struct rec r[16];
    handle_t client, bell, client2, bell2;
    uint32_t c, c2;
    int n;
    fsw_init(&w, &tab);
    CHECK(watcher(&w, 1, 0, &client, &bell, &c));
    uint32_t idd = add(&w, c, &K_D, true, IN(CREATE) | IN(DELETE) | IN(MODIFY));
    uint32_t ide = add(&w, c, &K_E, true, IN(ALL_EVENTS));
    uint32_t idf = add(&w, c, &K_F, false, IN(MODIFY) | IN(ATTRIB));
    CHECK(idd && ide && idf);

    /* Only watches that asked; IN_ISDIR for a directory, the directory's
     * record before the inode's own. */
    fsw_event(&w, &K_D, "f", 1, &K_F, IN(ACCESS), 0);
    fsw_event(&w, &K_ROOT, "e", 1, &K_E, IN(ATTRIB), FSW_EV_DIR);
    fsw_event(&w, &K_D, "f", 1, &K_F, IN(MODIFY), 0);
    fsw_dir_event(&w, &K_E, "sub", 3, IN(CREATE), 0, FSW_EV_DIR);
    CHECK(take_all(&w, c, r, 16, &n));
    CHECK_EQ(n, 4);
    CHECK(is(&r[0], ide, IN(ATTRIB) | IN(ISDIR), ""));
    CHECK(is(&r[1], idd, IN(MODIFY), "f"));
    CHECK(is(&r[2], idf, IN(MODIFY), ""));
    CHECK(is(&r[3], ide, IN(CREATE) | IN(ISDIR), "sub"));

    /* IN_EXCL_UNLINK: no events from an unlinked open file to its old
     * directory's watch that asked so; others get them. */
    CHECK(watcher(&w, 2, 0, &client2, &bell2, &c2));
    uint32_t idx = add(&w, c2, &K_D, true, IN(MODIFY) | IN(EXCL_UNLINK));
    fsw_event(&w, &K_D, "f", 1, &K_F, IN(MODIFY), FSW_EV_UNLINKED);
    CHECK(take_all(&w, c2, r, 16, &n));
    CHECK_EQ(n, 0);
    CHECK(take_all(&w, c, r, 16, &n));
    CHECK(n == 2 && is(&r[0], idd, IN(MODIFY), "f") && is(&r[1], idf, IN(MODIFY), ""));
    fsw_event(&w, &K_D, "f", 1, &K_F, IN(MODIFY), 0);
    CHECK(take_all(&w, c2, r, 16, &n));
    CHECK(n == 1 && is(&r[0], idx, IN(MODIFY), "f"));
    CHECK(take_all(&w, c, r, 16, &n));

    /* IN_ONESHOT: the first event, then IN_IGNORED, then nothing. */
    uint32_t idg = add(&w, c2, &K_G, false, IN(OPEN) | IN(ONESHOT));
    uint32_t before = tab.watches;
    fsw_self_event(&w, &K_G, IN(OPEN), 0);
    fsw_self_event(&w, &K_G, IN(OPEN), 0);
    CHECK(take_all(&w, c2, r, 16, &n));
    CHECK(n == 2 && is(&r[0], idg, IN(OPEN), "") && is(&r[1], idg, IN(IGNORED), ""));
    CHECK_EQ(tab.watches, before - 1);

    /* An inode gone: IN_DELETE_SELF where asked, IN_IGNORED for all. */
    uint32_t idf2 = add(&w, c2, &K_F, false, IN(DELETE_SELF));
    fsw_gone(&w, &K_F);
    CHECK(take_all(&w, c, r, 16, &n));
    CHECK(n == 1 && is(&r[0], idf, IN(IGNORED), ""));
    CHECK(take_all(&w, c2, r, 16, &n));
    CHECK(n == 2 && is(&r[0], idf2, IN(DELETE_SELF), "") && is(&r[1], idf2, IN(IGNORED), ""));
    fsw_self_event(&w, &K_F, IN(MODIFY), 0);
    CHECK(take_all(&w, c, r, 16, &n));
    CHECK_EQ(n, 0);

    /* A watch the service ends (out of its view's reach): IN_MOVE_SELF
     * (never with IN_ISDIR, as Linux's inotify) if asked, then IN_IGNORED. */
    int on_e = 0;
    fsw_each(&w, &K_E, count_one, &on_e);
    CHECK_EQ(on_e, 1);
    fsw_end(&w, ide & ((1u << FSW_SLOT_BITS) - 1), true);
    fsw_end(&w, ide & ((1u << FSW_SLOT_BITS) - 1), true);   /* ended: nothing */
    CHECK(take_all(&w, c, r, 16, &n));
    CHECK(n == 2 && is(&r[0], ide, IN(MOVE_SELF), "") && is(&r[1], ide, IN(IGNORED), ""));
    on_e = 0;
    fsw_each(&w, &K_E, count_one, &on_e);
    CHECK_EQ(on_e, 0);

    /* Cookies: never 0, one per move. */
    uint32_t k1 = fsw_cookie(&w), k2 = fsw_cookie(&w);
    CHECK(k1 && k2 && k1 != k2);
    w.cookie = UINT32_MAX;
    CHECK_EQ(fsw_cookie(&w), 1);
    fsw_dir_event(&w, &K_D, "a", 1, IN(MOVED_FROM), k1, 0);
    uint32_t idm = add(&w, c, &K_D, true, IN(MOVED_FROM) | IN(MASK_ADD));
    CHECK_EQ(idm, idd);
    fsw_dir_event(&w, &K_D, "a", 1, IN(MOVED_FROM), k1, 0);
    CHECK(take_all(&w, c, r, 16, &n));
    CHECK(n == 1 && is(&r[0], idd, IN(MOVED_FROM), "a") && r[0].cookie == k1);
    CHECK(take_all(&w, c2, r, 16, &n));

    /* The service's filter: a name its view hides never reaches it. */
    handle_t client3, bell3;
    uint32_t c3;
    CHECK(watcher(&w, 3, 4, &client3, &bell3, &c3));
    uint32_t idh = add(&w, c3, &K_D, true, IN(CREATE));
    w.hidden = hide_secret;
    fsw_dir_event(&w, &K_D, "secret", 6, IN(CREATE), 0, 0);
    fsw_dir_event(&w, &K_D, "plain", 5, IN(CREATE), 0, 0);
    w.hidden = NULL;
    CHECK(take_all(&w, c3, r, 16, &n));
    CHECK(n == 1 && is(&r[0], idh, IN(CREATE), "plain"));
    CHECK(take_all(&w, c, r, 16, &n));
    CHECK(n == 2 && is(&r[0], idd, IN(CREATE), "secret"));   /* a view with no flags */
    unwatcher(&w, c3, client3, bell3);
    unwatcher(&w, c2, client2, bell2);
    unwatcher(&w, c, client, bell);
    CHECK_EQ(tab.watches, 0);
    return true;
}

/* A successor: the table taken over, the watches found again, the queue
 * starting with IN_Q_OVERFLOW and rung once the bell is back; a broken
 * table refused. */
bool t_fswatch_adopt(void)
{
    static struct rec r[16];
    handle_t client, bell, srv2, client2, bell2, give;
    uint32_t c, id;
    uint8_t existed;
    int n;
    fsw_init(&w, &tab);
    CHECK(watcher(&w, 1, 0, &client, &bell, &c));
    uint32_t idd = add(&w, c, &K_D, true, IN(CREATE));
    uint32_t idf = add(&w, c, &K_F, false, IN(MODIFY));
    fsw_dir_event(&w, &K_D, "lost", 4, IN(CREATE), 0, 0);

    memcpy(&tab2, &tab, sizeof(tab));
    CHECK(fsw_adopt(&w2, &tab2));
    CHECK(fsw_watching(&w2) && !fsw_belled(&w2, c));
    CHECK_ST(jam_channel_create(&client2, &srv2), OK);
    CHECK_ST(jam_event_create(&bell2), OK);
    CHECK_ST(jam_handle_duplicate(bell2, RIGHT_SIGNAL | RIGHT_TRANSFER, &give), OK);
    fsw_chan_handles(&w2, c, srv2, give);
    CHECK(rung(bell2));
    CHECK(take_all(&w2, c, r, 16, &n));
    CHECK(n == 1 && is(&r[0], 0, IN(Q_OVERFLOW), ""));
    fsw_event(&w2, &K_D, "f", 1, &K_F, IN(CREATE) | IN(MODIFY), 0);
    CHECK(take_all(&w2, c, r, 16, &n));
    CHECK(n == 2 && is(&r[0], idd, IN(CREATE) | IN(MODIFY), "f") &&
          is(&r[1], idf, IN(CREATE) | IN(MODIFY), ""));
    CHECK_ST(fsw_add(&w2, c, &K_D, true, IN(CREATE), &id, &existed), OK);   /* run again */
    CHECK(id == idd && existed);
    CHECK_ST(fsw_remove(&w2, c, idf), OK);
    CHECK_ST(fsw_remove(&w2, c, idf), OK);   /* run again: OK */
    fsw_chan_close(&w2, c);
    jam_handle_close(client2);
    jam_handle_close(bell2);

    /* Broken tables: set up empty, false. */
    memcpy(&tab2, &tab, sizeof(tab));
    tab2.version = FSW_TABLE_VERSION + 1;
    CHECK(!fsw_adopt(&w2, &tab2));
    CHECK(!fsw_watching(&w2) && !tab2.chans[c].live && tab2.version == FSW_TABLE_VERSION);
    memcpy(&tab2, &tab, sizeof(tab));
    tab2.chans[c].live = 0;   /* its watches' channel gone */
    CHECK(!fsw_adopt(&w2, &tab2));
    memcpy(&tab2, &tab, sizeof(tab));
    tab2.chans[c].watches++;
    CHECK(!fsw_adopt(&w2, &tab2));
    memcpy(&tab2, &tab, sizeof(tab));
    tab2.w[idd & ((1u << FSW_SLOT_BITS) - 1)].id ^= 1;
    CHECK(!fsw_adopt(&w2, &tab2));
    unwatcher(&w, c, client, bell);
    return true;
}

/* fsw_order: packets kept in order, one picked out ahead of the rest. */
static bool key_is(void *ctx, const struct port_packet *p)
{
    return p->key == *(uint64_t *)ctx;
}

bool t_fswatch_order(void)
{
    struct port_packet room[4], p;
    struct fsw_order o = { .q = room, .cap = 4 };
    handle_t port;
    uint64_t want = 3;
    CHECK_ST(jam_port_create(&port), OK);
    for (uint64_t k = 1; k <= 6; k++) {
        struct port_packet u = { .key = k, .type = PORT_PACKET_USER };
        CHECK_ST(jam_port_queue(port, &u), OK);
    }
    CHECK_EQ(fsw_order_drain(&o, port), 4);   /* cap: 5 and 6 stay on the port */
    CHECK(fsw_order_pick(&o, key_is, &want, &p) && p.key == 3);
    CHECK(!fsw_order_pick(&o, key_is, &want, &p));
    CHECK_EQ(fsw_order_drain(&o, port), 1);
    uint64_t order[] = { 1, 2, 4, 5, 6 };
    for (unsigned i = 0; i < 5; i++) {
        CHECK_ST(fsw_order_next(&o, port, 0, &p), OK);
        CHECK_EQ(p.key, order[i]);
    }
    CHECK_ST(fsw_order_next(&o, port, 0, &p), ERR_TIMED_OUT);
    CHECK_EQ(fsw_order_drain(&o, port), 0);
    jam_handle_close(port);
    return true;
}
