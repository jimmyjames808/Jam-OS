/* The keep channel, both sides (<keep.h> has the rules, abi/idl/keepch.idl
 * the messages: IDL events both ways, the generated code reads and checks
 * their format).
 *
 * The service's side duplicates and sends (keep_put, keep_drop) and, in a
 * successor, reads the keeper's restore (keep_restore). The keeper's side
 * (devmgr's and init's) checks every message before it keeps anything and
 * holds its duplicates in a fixed table of KEEP_MAX_SLOTS entries; a
 * restore hands out duplicates of those, so the keeper still holds every
 * slot if the successor dies too.
 *
 * Nothing here trusts the other side: the keeper checks what a service
 * sends (it may have been compromised before it died), and a successor
 * checks what the keeper sends. Every handle a message carried ends up
 * kept, handed on or closed, whatever the message said. */
#include <keep.h>
#include <os.h>

#define LOG_REFUSALS 8      /* refusals logged per keeper; the rest are only counted */

static void close_all(const handle_t *hs, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        jam_handle_close(hs[i]);
}

/* ---- the service's side --------------------------------------------------------- */

status_t keep_put(handle_t keep, uint32_t slot, const handle_t *hs, unsigned n)
{
    if (!hs || n == 0 || n > KEEP_SLOT_HANDLES)
        return ERR_INVALID_ARGS;
    handle_t d[KEEP_SLOT_HANDLES];
    unsigned made = 0;
    status_t st = OK;
    while (st == OK && made < n) {
        st = jam_handle_duplicate(hs[made], RIGHT_SAME, &d[made]);
        if (st == OK)
            made++;
    }
    if (st != OK) {
        close_all(d, made);
        return st;
    }
    /* The duplicates are gone whatever the write's status (a send's handles
     * are always moved): never closed here again, as their values may
     * already be another thread's new handles. */
    return keepch_put(keep, slot, 0, d, n);
}

status_t keep_drop(handle_t keep, uint32_t slot)
{
    return keepch_drop(keep, slot);
}

/* A successor's restore in progress. */
struct restore_run {
    handle_t             keep;                    /* the keep channel */
    keep_take_fn         take;                    /* the state's answer, slot by slot */
    void                *ctx;                     /* take's */
    uint32_t             seen[KEEP_MAX_SLOTS];    /* slot numbers so far */
    unsigned             nseen;                   /* entries of seen */
    uint32_t             handles;                 /* handles so far, taken or not */
    bool                 done;                    /* the keeper's done came, with the right totals */
    bool                 bad;                     /* a restore or done that breaks the rules */
    struct keep_restored got;                     /* what the caller gets */
};

static bool seen_before(const struct restore_run *r, uint32_t slot)
{
    for (unsigned i = 0; i < r->nseen; i++)
        if (r->seen[i] == slot)
            return true;
    return false;
}

/* A restore of ns slots (their handle counts, nh handles in all) that fits
 * what came before it: counts for every slot and none else, each 1 to
 * KEEP_SLOT_HANDLES, summing to the handles, no slot twice, no more than a
 * keeper may hold. (The generated reader has checked the arrays' sizes.) */
static bool restore_ok(const struct restore_run *r, const uint32_t *slots, uint32_t ns,
                       const uint8_t *counts, uint32_t nc, uint32_t nh)
{
    if (ns < 1 || ns > KEEP_BATCH || nc != ns)
        return false;
    if (ns > KEEP_MAX_SLOTS - r->nseen)
        return false;
    uint32_t sum = 0;
    for (unsigned i = 0; i < ns; i++) {
        uint32_t c = counts[i];
        if (c < 1 || c > KEEP_SLOT_HANDLES || seen_before(r, slots[i]))
            return false;
        for (unsigned j = 0; j < i; j++)
            if (slots[j] == slots[i])
                return false;
        sum += c;
    }
    return sum == nh && nh <= KEEP_MAX_HANDLES - r->handles;
}

/* Hand each slot of a checked restore to take(); close the refused. */
static void hand_slots(struct restore_run *r, const uint32_t *slots, const uint8_t *counts,
                       uint32_t ns, const handle_t *hs)
{
    unsigned off = 0;
    for (unsigned i = 0; i < ns; i++) {
        uint32_t slot = slots[i], c = counts[i];
        r->seen[r->nseen++] = slot;
        r->handles += c;
        r->got.slots++;
        if (r->take(r->ctx, slot, &hs[off], c)) {
            r->got.taken++;
            r->got.handles += c;
        } else {
            close_all(&hs[off], c);
            /* The keeper would hold the slot until the service is given up
             * on: its clients would wait on a slot nobody serves. */
            status_t st = keep_drop(r->keep, slot);
            if (st != OK)
                printf("keep: can't drop slot %u (%s)\n", slot, status_str(st));
        }
        off += c;
    }
}

/* keepch's events from the keeper. Both own their handles. */
static void on_restore(void *ctx, const uint32_t *slots, uint32_t ns, const uint8_t *counts,
                       uint32_t nc, const handle_t *hs, uint32_t nh)
{
    struct restore_run *r = ctx;
    if (r->done || r->bad || !restore_ok(r, slots, ns, counts, nc, nh)) {
        close_all(hs, nh);
        r->bad = true;
        return;
    }
    hand_slots(r, slots, counts, ns, hs);
}

static void on_done(void *ctx, uint32_t slots, uint32_t handles)
{
    struct restore_run *r = ctx;
    r->done = !r->bad && slots == r->got.slots && handles == r->handles;
    r->bad = !r->done;
}

status_t keep_restore(handle_t keep, uint64_t deadline_ns, keep_take_fn take, void *ctx,
                      struct keep_restored *out)
{
    static const struct keepch_events events = { .restore = on_restore, .done = on_done };
    struct restore_run *r = jam_calloc(1, sizeof(*r));
    if (!r)
        return ERR_NO_MEMORY;
    r->keep = keep;
    r->take = take;
    r->ctx = ctx;
    status_t st;
    for (;;) {
        st = keepch_read_event(keep, &events, r);
        if (st == ERR_SHOULD_WAIT) {
            signals_t seen = 0;
            st = jam_object_wait_one(keep, SIG_READABLE | SIG_PEER_CLOSED, deadline_ns, &seen);
            if (st == OK)
                continue;
        } else if (st == OK) {
            if (r->bad || r->done) {
                st = r->done ? OK : ERR_INVALID_ARGS;
                break;
            }
            continue;
        } else if (st != ERR_PEER_CLOSED && st != ERR_NO_MEMORY) {
            st = ERR_INVALID_ARGS;   /* no event of the keeper's: dropped, its handles closed */
        }
        break;
    }
    if (out)
        *out = r->got;
    jam_free(r);
    return st;
}

/* ---- the keeper's side ----------------------------------------------------------- */

void keeper_init(struct keeper *k)
{
    memset(k, 0, sizeof(*k));
    k->ch = HANDLE_INVALID;
}

static struct keep_slot *find_slot(struct keeper *k, uint32_t id)
{
    for (unsigned i = 0; i < KEEP_MAX_SLOTS; i++)
        if (k->slot[i].n && k->slot[i].id == id)
            return &k->slot[i];
    return NULL;
}

static struct keep_slot *free_slot(struct keeper *k)
{
    for (unsigned i = 0; i < KEEP_MAX_SLOTS; i++)
        if (!k->slot[i].n)
            return &k->slot[i];
    return NULL;
}

/* Close a slot's handles and free its entry. */
static void forget(struct keeper *k, struct keep_slot *s)
{
    close_all(s->h, s->n);
    k->nhandles -= s->n;
    k->nslots--;
    s->n = 0;
}

/* A channel, VMO or event (handle_info), and one we can duplicate: a
 * restore hands out duplicates. */
static bool keepable(handle_t h)
{
    struct handle_info i;
    if (jam_handle_info(h, &i) != OK)
        return false;   /* not a handle at all */
    return (i.type == OBJ_CHANNEL || i.type == OBJ_VMO || i.type == OBJ_EVENT) &&
           (i.rights & RIGHT_DUPLICATE);
}

/* Keep hs[0..n) as slot id (replacing what it held), or say why not. On
 * success the handles are the keeper's. */
static const char *put_slot(struct keeper *k, uint32_t id, const handle_t *hs, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (!keepable(hs[i]))
            return "a handle of a kind it doesn't keep";
    struct keep_slot *s = find_slot(k, id);
    unsigned old = s ? s->n : 0;
    if (!s && k->nslots >= KEEP_MAX_SLOTS)
        return "too many slots";
    if (k->nhandles - old + n > KEEP_MAX_HANDLES)
        return "too many handles";
    if (s) {
        forget(k, s);
        k->replaced++;
    } else {
        s = free_slot(k);   /* there is one: nslots < KEEP_MAX_SLOTS */
    }
    s->id = id;
    s->n = n;
    for (unsigned i = 0; i < n; i++)
        s->h[i] = hs[i];
    k->nslots++;
    k->nhandles += n;
    k->puts++;
    return NULL;
}

static void refuse(struct keeper *k, const char *why, const handle_t *hs, unsigned n)
{
    close_all(hs, n);
    if (++k->refused <= LOG_REFUSALS)
        printf("keep: refused a message from the service: %s\n", why);
}

/* keepch's events from the service (the generated server has checked the
 * format: a put of 0 to KEEP_SLOT_HANDLES handles, a drop with none; what
 * it refuses never gets here, keeper_take counts it). Both count
 * themselves handled. */
static void on_put(void *ctx, uint32_t slot, uint32_t ring, const handle_t *hs, uint32_t n)
{
    struct keeper *k = ctx;
    (void)ring;
    k->handled++;
    const char *why = n < 1 ? "a put of no handles" : put_slot(k, slot, hs, n);
    if (why)
        refuse(k, why, hs, n);
}

static void on_drop(void *ctx, uint32_t slot)
{
    struct keeper *k = ctx;
    k->handled++;
    /* A drop of a slot we never kept (its put was refused) is no error. */
    struct keep_slot *s = find_slot(k, slot);
    if (s) {
        forget(k, s);
        k->drops++;
    }
}

status_t keeper_take(struct keeper *k)
{
    static const struct keepch_ops ops = { .put = on_put, .drop = on_drop };
    if (k->ch == HANDLE_INVALID)
        return ERR_BAD_STATE;
    uint32_t before = k->handled;
    status_t st = keepch_serve_one(k->ch, &ops, k);
    /* A message was taken off the queue and no handler ran: the generated
     * server refused it (malformed, too big, a txid, the wrong direction)
     * and closed its handles. */
    if (st == OK && k->handled == before)
        refuse(k, "malformed", NULL, 0);
    return st;
}

bool keeper_drain_upto(struct keeper *k, unsigned max)
{
    for (unsigned i = 0; i < max; i++)
        if (keeper_take(k) != OK)
            return false;   /* empty (or the service gone and its queue read) */
    signals_t seen = 0;
    return k->ch != HANDLE_INVALID &&
           jam_object_wait_one(k->ch, SIG_READABLE, 0, &seen) == OK;
}

bool keeper_drain(struct keeper *k)
{
    return keeper_drain_upto(k, CHANNEL_MAX_QUEUED);
}

status_t keeper_attach(struct keeper *k, handle_t *service_end)
{
    if (k->ch != HANDLE_INVALID) {
        keeper_drain(k);
        jam_handle_close(k->ch);
        k->ch = HANDLE_INVALID;
    }
    handle_t ours, theirs;
    status_t st = jam_channel_create(&ours, &theirs);
    if (st != OK)
        return st;
    k->ch = ours;
    *service_end = theirs;
    return OK;
}

/* Restore messages being built: whole slots, at most KEEP_BATCH handles. */
struct batch {
    uint32_t            slot[KEEP_BATCH];     /* the message's slot numbers */
    uint8_t             count[KEEP_BATCH];    /* ... and their handle counts */
    struct keep_slot   *s[KEEP_BATCH];     /* its slots, in order */
    handle_t            h[KEEP_BATCH];     /* their duplicates */
    unsigned            nh;                /* handles in it */
    uint32_t            slots, handles;    /* sent so far, all batches (done's) */
};

/* Duplicates of slots s[0..n) into b->h; ERR_* and none made on failure. */
static status_t dup_slots(struct batch *b, struct keep_slot *const *s, unsigned n)
{
    unsigned made = 0;
    status_t st = OK;
    for (unsigned i = 0; st == OK && i < n; i++)
        for (unsigned j = 0; st == OK && j < s[i]->n; j++) {
            st = jam_handle_duplicate(s[i]->h[j], RIGHT_SAME, &b->h[made]);
            if (st == OK)
                made++;
        }
    if (st != OK)
        close_all(b->h, made);
    return st;
}

/* Send slots s[0..n) (nh handles) as one restore event. */
static status_t send_slots(struct keeper *k, struct batch *b, struct keep_slot *const *s,
                           unsigned n, unsigned nh)
{
    for (unsigned i = 0; i < n; i++) {
        b->slot[i] = s[i]->id;
        b->count[i] = (uint8_t)s[i]->n;
    }
    status_t st = dup_slots(b, s, n);
    if (st != OK)
        return st;
    /* The duplicates are gone whatever the write's status (a send's handles
     * are always moved): never closed after it, as their values may already
     * be another thread's new handles. */
    st = keepch_restore(k->ch, b->slot, n, b->count, n, b->h, nh);
    if (st != OK)
        return st;
    b->slots += n;
    b->handles += nh;
    return OK;
}

/* Send the batch; if it fails, slot by slot, losing a slot that can never
 * be sent (no right to send it, or a channel end with channel ends
 * queued). */
static status_t flush(struct keeper *k, struct batch *b, unsigned n)
{
    if (!n || send_slots(k, b, b->s, n, b->nh) == OK)
        return OK;
    for (unsigned i = 0; i < n; i++) {
        status_t st = send_slots(k, b, &b->s[i], 1, b->s[i]->n);
        if (st == ERR_ACCESS_DENIED || st == ERR_NOT_SUPPORTED || st == ERR_WRONG_TYPE) {
            printf("keep: slot %u can't be handed over (%s): dropped\n", b->s[i]->id,
                   status_str(st));
            forget(k, b->s[i]);
            k->lost++;
        } else if (st != OK) {
            return st;
        }
    }
    return OK;
}

status_t keeper_restore(struct keeper *k)
{
    if (k->ch == HANDLE_INVALID)
        return ERR_BAD_STATE;
    struct batch *b = jam_calloc(1, sizeof(*b));
    if (!b)
        return ERR_NO_MEMORY;
    status_t st = OK;
    unsigned n = 0;
    for (unsigned i = 0; st == OK && i < KEEP_MAX_SLOTS; i++) {
        struct keep_slot *s = &k->slot[i];
        if (!s->n)
            continue;
        if (b->nh + s->n > KEEP_BATCH) {
            st = flush(k, b, n);
            n = b->nh = 0;
        }
        b->s[n++] = s;
        b->nh += s->n;
    }
    if (st == OK)
        st = flush(k, b, n);
    if (st == OK)
        st = keepch_done(k->ch, b->slots, b->handles);
    jam_free(b);
    return st;
}

void keeper_release(struct keeper *k)
{
    for (unsigned i = 0; i < KEEP_MAX_SLOTS; i++)
        if (k->slot[i].n)
            forget(k, &k->slot[i]);
    if (k->ch != HANDLE_INVALID)
        jam_handle_close(k->ch);
    k->ch = HANDLE_INVALID;
}
