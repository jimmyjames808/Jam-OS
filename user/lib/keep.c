/* The keep channel, both sides (<keep.h> has the protocol and its rules).
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
#define DRAIN_MAX    1024   /* a channel end holds at most this many messages */
#define CHANNEL_MAX_HANDLES 64   /* handles one channel message carries at most */

static void close_all(const handle_t *hs, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        jam_handle_close(hs[i]);
}

/* One message off ch into buf/hs (OK, or the read's status). */
static status_t read_msg(handle_t ch, void *buf, uint32_t cap, handle_t *hs, uint32_t hcap,
                         uint32_t *nb, uint32_t *nh)
{
    *nb = *nh = 0;
    struct channel_read_args a = {
        .h = ch,
        .bytes_cap = cap,
        .bytes = (uint64_t)(uintptr_t)buf,
        .actual_bytes = (uint64_t)(uintptr_t)nb,
        .handles = (uint64_t)(uintptr_t)hs,
        .handles_cap = hcap,
        .actual_handles = (uint64_t)(uintptr_t)nh,
    };
    return jam_channel_read(&a);
}

/* The message at the head of ch was too big for its reader: read it whole
 * (nb bytes) and throw it away with its handles, so it can't block the
 * queue. ERR_NO_MEMORY: no room to read it into (it stays queued). */
static status_t discard_head(handle_t ch, uint32_t nb)
{
    void *buf = malloc(nb ? nb : 1);
    if (!buf)
        return ERR_NO_MEMORY;
    handle_t hs[CHANNEL_MAX_HANDLES];
    uint32_t got = 0, nh = 0;
    status_t st = read_msg(ch, buf, nb, hs, CHANNEL_MAX_HANDLES, &got, &nh);
    if (st == OK)
        close_all(hs, nh);
    free(buf);
    return st;
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
    struct keep_msg m = { .txid = 0, .kind = KEEP_PUT, .slot = slot, .count = n };
    if (st == OK)
        st = jam_channel_write(keep, &m, sizeof(m), d, n);
    /* A failed write puts the duplicates back in our table. */
    if (st != OK)
        close_all(d, made);
    return st;
}

status_t keep_drop(handle_t keep, uint32_t slot)
{
    struct keep_msg m = { .txid = 0, .kind = KEEP_DROP, .slot = slot, .count = 0 };
    return jam_channel_write(keep, &m, sizeof(m), NULL, 0);
}

/* A successor's restore in progress. */
struct restore_run {
    handle_t             keep;                    /* the keep channel */
    keep_take_fn         take;                    /* the state's answer, slot by slot */
    void                *ctx;                     /* take's */
    uint32_t             seen[KEEP_MAX_SLOTS];    /* slot numbers so far */
    unsigned             nseen;                   /* entries of seen */
    uint32_t             handles;                 /* handles so far, taken or not */
    struct keep_restored got;                     /* what the caller gets */
};

static bool seen_before(const struct restore_run *r, uint32_t slot)
{
    for (unsigned i = 0; i < r->nseen; i++)
        if (r->seen[i] == slot)
            return true;
    return false;
}

/* m (nb bytes, nh handles) is a well-formed KEEP_RESTORE that fits what
 * came before it: sizes, counts, no slot twice, no more than a keeper
 * may hold. */
static bool restore_ok(const struct restore_run *r, const struct keep_restore *m, uint32_t nb,
                       uint32_t nh)
{
    if (nb < KEEP_RESTORE_SIZE(0) || m->txid || m->reserved)
        return false;
    if (m->nslots < 1 || m->nslots > KEEP_BATCH || nb != KEEP_RESTORE_SIZE(m->nslots))
        return false;
    if (m->nslots > KEEP_MAX_SLOTS - r->nseen)
        return false;
    uint32_t sum = 0;
    for (unsigned i = 0; i < m->nslots; i++) {
        uint32_t c = m->slot[i].count;
        if (c < 1 || c > KEEP_SLOT_HANDLES || seen_before(r, m->slot[i].slot))
            return false;
        for (unsigned j = 0; j < i; j++)
            if (m->slot[j].slot == m->slot[i].slot)
                return false;
        sum += c;
    }
    return sum == nh && nh <= KEEP_MAX_HANDLES - r->handles;
}

/* Hand each slot of a checked KEEP_RESTORE to take(); close the refused. */
static void hand_slots(struct restore_run *r, const struct keep_restore *m, handle_t *hs)
{
    unsigned off = 0;
    for (unsigned i = 0; i < m->nslots; i++) {
        uint32_t slot = m->slot[i].slot, c = m->slot[i].count;
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

/* Wait (until deadline_ns) for one message from the keeper. */
static status_t restore_read(handle_t keep, uint64_t deadline_ns, struct keep_restore *m,
                             handle_t *hs, uint32_t *nb, uint32_t *nh)
{
    for (;;) {
        status_t st = read_msg(keep, m, sizeof(*m), hs, KEEP_BATCH, nb, nh);
        if (st == ERR_BUFFER_TOO_SMALL) {
            (void)discard_head(keep, *nb);   /* malformed either way */
            return ERR_INVALID_ARGS;
        }
        if (st != ERR_SHOULD_WAIT)
            return st;
        signals_t seen = 0;
        st = jam_object_wait_one(keep, SIG_READABLE | SIG_PEER_CLOSED, deadline_ns, &seen);
        if (st != OK)
            return st;
    }
}

status_t keep_restore(handle_t keep, uint64_t deadline_ns, keep_take_fn take, void *ctx,
                      struct keep_restored *out)
{
    struct restore_run *r = calloc(1, sizeof(*r));
    if (!r)
        return ERR_NO_MEMORY;
    r->keep = keep;
    r->take = take;
    r->ctx = ctx;
    struct keep_restore m;
    handle_t hs[KEEP_BATCH];
    status_t st;
    for (;;) {
        uint32_t nb = 0, nh = 0;
        st = restore_read(keep, deadline_ns, &m, hs, &nb, &nh);
        if (st != OK)
            break;
        if (nb >= 8 && m.kind == KEEP_RESTORE && restore_ok(r, &m, nb, nh)) {
            hand_slots(r, &m, hs);
            continue;
        }
        const struct keep_msg *d = (const struct keep_msg *)&m;
        bool done = nb == sizeof(*d) && nh == 0 && d->kind == KEEP_DONE && !d->txid &&
                    d->slot == r->got.slots && d->count == r->handles;
        close_all(hs, nh);
        st = done ? OK : ERR_INVALID_ARGS;
        break;
    }
    if (out)
        *out = r->got;
    free(r);
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

/* A channel, VMO or event (<keep.h> says how this is told), and one we can
 * duplicate: a restore hands out duplicates. */
static bool keepable(handle_t h)
{
    uint64_t size;
    bool ok = jam_vmo_get_size(h, &size) == OK;
    if (!ok) {
        /* Sets and clears nothing; ERR_ACCESS_DENIED: an event without
         * RIGHT_SIGNAL (the type is checked before the rights). */
        status_t st = jam_event_signal(h, 0, 0);
        ok = st == OK || st == ERR_ACCESS_DENIED;
        if (st != OK && st != ERR_ACCESS_DENIED && st != ERR_WRONG_TYPE)
            return false;   /* not a handle at all */
    }
    if (!ok) {
        signals_t seen = 0;
        status_t st = jam_object_wait_one(h, SIG_WRITABLE | SIG_PEER_CLOSED, 0, &seen);
        ok = (st == OK || st == ERR_TIMED_OUT) && (seen & (SIG_WRITABLE | SIG_PEER_CLOSED));
    }
    handle_t d;
    if (!ok || jam_handle_duplicate(h, RIGHT_SAME, &d) != OK)
        return false;
    jam_handle_close(d);
    return true;
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

/* Act on one message the service sent (nb bytes, nh handles). */
static void apply(struct keeper *k, const struct keep_msg *m, uint32_t nb, const handle_t *hs,
                  uint32_t nh)
{
    const char *why = NULL;
    if (nb != sizeof(*m) || m->txid)
        why = "malformed";
    else if (m->kind == KEEP_PUT && (m->count < 1 || m->count > KEEP_SLOT_HANDLES))
        why = "a put of no handles, or too many";
    else if (m->kind == KEEP_PUT && m->count != nh)
        why = "a put whose count isn't its handles";
    else if (m->kind == KEEP_DROP && (m->count || nh))
        why = "a drop with handles";
    else if (m->kind != KEEP_PUT && m->kind != KEEP_DROP)
        why = "an unknown kind";
    else if (m->kind == KEEP_PUT)
        why = put_slot(k, m->slot, hs, nh);
    if (why) {
        refuse(k, why, hs, nh);
        return;
    }
    if (m->kind == KEEP_DROP) {
        /* A drop of a slot we never kept (its put was refused) is no error. */
        struct keep_slot *s = find_slot(k, m->slot);
        if (s) {
            forget(k, s);
            k->drops++;
        }
    }
}

status_t keeper_take(struct keeper *k)
{
    if (k->ch == HANDLE_INVALID)
        return ERR_BAD_STATE;
    struct keep_msg m;
    handle_t hs[KEEP_SLOT_HANDLES];
    uint32_t nb = 0, nh = 0;
    status_t st = read_msg(k->ch, &m, sizeof(m), hs, KEEP_SLOT_HANDLES, &nb, &nh);
    if (st == ERR_BUFFER_TOO_SMALL) {
        st = discard_head(k->ch, nb);
        if (st != OK)
            return st;
        refuse(k, "a message too big", NULL, 0);
        return OK;
    }
    if (st == OK)
        apply(k, &m, nb, hs, nh);
    return st;
}

void keeper_drain(struct keeper *k)
{
    for (unsigned i = 0; i < DRAIN_MAX && keeper_take(k) == OK; i++)
        ;
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
    struct keep_restore msg;               /* the message */
    struct keep_slot   *s[KEEP_BATCH];     /* its slots, in order */
    handle_t            h[KEEP_BATCH];     /* their duplicates */
    unsigned            nh;                /* handles in it */
    uint32_t            slots, handles;    /* sent so far, all batches (KEEP_DONE's) */
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

/* Send slots s[0..n) (nh handles) as one KEEP_RESTORE. */
static status_t send_slots(struct keeper *k, struct batch *b, struct keep_slot *const *s,
                           unsigned n, unsigned nh)
{
    b->msg = (struct keep_restore){ .txid = 0, .kind = KEEP_RESTORE, .nslots = n };
    for (unsigned i = 0; i < n; i++) {
        b->msg.slot[i].slot = s[i]->id;
        b->msg.slot[i].count = s[i]->n;
    }
    status_t st = dup_slots(b, s, n);
    if (st != OK)
        return st;
    st = jam_channel_write(k->ch, &b->msg, KEEP_RESTORE_SIZE(n), b->h, nh);
    if (st != OK) {
        close_all(b->h, nh);   /* a failed write leaves them in our table */
        return st;
    }
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
    struct batch *b = calloc(1, sizeof(*b));
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
    struct keep_msg done = { .txid = 0, .kind = KEEP_DONE, .slot = b->slots,
                             .count = b->handles };
    if (st == OK)
        st = jam_channel_write(k->ch, &done, sizeof(done), NULL, 0);
    free(b);
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
