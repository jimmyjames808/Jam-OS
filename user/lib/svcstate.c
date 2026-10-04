/* A service's state VMO and a spare's promotion (<svcstate.h> has the
 * model; the spare's own side is in start.c).
 *
 * The layout is computed from what the service expects (struct
 * svcstate_layout), never read from the VMO: a header page, slot 0's
 * request and reply areas, slot 1's, the service's own area, each on a
 * page. So a corrupted header can't point anything elsewhere; it can only
 * fail a check, which sets the state up empty.
 *
 * The words a successor reads to know where a request stands are written
 * with released stores in the order the header comment gives (a slot's
 * length before its number; the reply's length before the commit word;
 * the commit before the sent mark). The process dies between two of its
 * instructions, never inside one, and its successor reads the state only
 * after the death, so a word is either old or new and the order holds. */
#include <os.h>
#include <svcstate.h>

_Static_assert(sizeof(struct svcstate_slot) == 32, "no padding in a slot");
_Static_assert(sizeof(struct svcstate_header) == 160, "no padding in the header");
_Static_assert(sizeof(struct svcstate_header) <= PAGE_SIZE, "the header fits its page");

static uint64_t page_up(uint64_t n)
{
    return (n + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

/* Where each area starts (the caller checked the layout: svcstate_size). */
static uint64_t req_off(const struct svcstate_header *h, unsigned slot)
{
    return PAGE_SIZE + slot * (page_up(h->req_cap) + page_up(h->rep_cap));
}

static uint64_t rep_off(const struct svcstate_header *h, unsigned slot)
{
    return req_off(h, slot) + page_up(h->req_cap);
}

static uint64_t user_off(uint32_t req_cap, uint32_t rep_cap)
{
    return PAGE_SIZE + 2 * (page_up(req_cap) + page_up(rep_cap));
}

uint64_t svcstate_size(const struct svcstate_layout *l)
{
    if (l->req_cap < 4 || l->req_cap > SVCSTATE_REQ_MAX || l->rep_cap < 4 ||
        l->rep_cap > SVCSTATE_REQ_MAX || l->user_size > SVCSTATE_MAX_SIZE)
        return 0;
    uint64_t size = user_off(l->req_cap, l->rep_cap) + page_up(l->user_size);
    return size <= SVCSTATE_MAX_SIZE ? size : 0;
}

status_t svcstate_create(uint64_t size, handle_t *out)
{
    if (!size || size > SVCSTATE_MAX_SIZE)
        return ERR_OUT_OF_RANGE;
    return jam_vmo_create(page_up(size), 0, HANDLE_INVALID, out);
}

status_t svcstate_give(handle_t state, handle_t *out)
{
    return jam_handle_duplicate(state, SVCSTATE_SERVICE_RIGHTS | RIGHT_TRANSFER, out);
}

/* ---- checks ----------------------------------------------------------------------- */

/* A request is wholly in the slot: the kernel's read wrote a length that
 * fits (a read that found the request too big writes its length too, and
 * leaves it queued). */
static bool taken(const struct svcstate_header *h, const struct svcstate_slot *s)
{
    return s->len && s->len <= h->req_cap && s->nhandles <= SVCSTATE_SLOT_HANDLES;
}

/* The slots and the commit word make sense together, or why not. */
static const char *check_slots(const struct svcstate_header *h)
{
    for (unsigned i = 0; i < 2; i++) {
        const struct svcstate_slot *s = &h->slot[i];
        if (s->seq && (s->seq & 1) != i)
            return "a request in the wrong slot";
        if (s->phase != SVCSTATE_RUN && s->phase != SVCSTATE_SENT)
            return "a slot in no known phase";
        if (s->reply_len > h->rep_cap)
            return "a reply longer than its area";
        if (s->phase == SVCSTATE_SENT && s->seq > h->commit)
            return "a request sent before its commit";
    }
    uint64_t a = h->slot[0].seq, b = h->slot[1].seq;
    uint64_t hi = a > b ? a : b, lo = a > b ? b : a;
    if (hi && hi - lo != 1)
        return "slots out of step";
    /* The other slot was answered (so committed) before this one was set up. */
    if (h->commit > hi || (hi > 1 && h->commit < hi - 1))
        return "a commit word out of step";
    return NULL;
}

/* Everything the header says matches what the service expects, or why not. */
static const char *check_header(const struct svcstate_header *h, const struct svcstate_layout *w)
{
    if (h->magic != SVCSTATE_MAGIC)
        return "not a state";
    if (h->version != SVCSTATE_VERSION || h->header_size != sizeof(*h))
        return "another header version";
    if (h->kind != w->kind)
        return "another service's";
    if (h->layout != w->layout)
        return "another layout version";
    if (memcmp(h->binding, w->binding, SVCSTATE_BINDING))
        return "bound to something else";
    if (h->req_cap != w->req_cap || h->rep_cap != w->rep_cap || h->user_size != w->user_size ||
        h->size != svcstate_size(w))
        return "another layout";
    if (h->adopted == UINT64_MAX)
        return "adopted too often";
    return check_slots(h);
}

static bool all_zero(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        if (b[i])
            return false;
    return true;
}

/* An empty state for w. Written header field by field: the reserved
 * rest of the page is left as it is. */
static void set_up(struct svcstate_header *h, const struct svcstate_layout *w)
{
    memset(h, 0, sizeof(*h));
    h->version = SVCSTATE_VERSION;
    h->header_size = sizeof(*h);
    h->kind = w->kind;
    h->layout = w->layout;
    h->size = svcstate_size(w);
    memcpy(h->binding, w->binding, SVCSTATE_BINDING);
    h->req_cap = w->req_cap;
    h->rep_cap = w->rep_cap;
    h->user_size = w->user_size;
    __atomic_store_n(&h->magic, SVCSTATE_MAGIC, __ATOMIC_RELEASE);   /* last: a state once whole */
}

/* The number the next take gives: the higher slot's again if it holds no
 * request (it was set up, the read found nothing), else one more. */
static uint64_t next_seq(const struct svcstate_header *h)
{
    const struct svcstate_slot *a = &h->slot[0], *b = &h->slot[1];
    const struct svcstate_slot *cur = a->seq > b->seq ? a : b;
    if (!cur->seq)
        return 1;
    return taken(h, cur) ? cur->seq + 1 : cur->seq;
}

/* ---- the service's side ------------------------------------------------------------ */

/* Commit the header's and slots' pages and write to each, so the page
 * tables are there: a read into a slot then never faults. */
static status_t prepare_pages(handle_t vmo, uint8_t *base, uint64_t len)
{
    status_t st = jam_vmo_commit(vmo, 0, len);
    for (uint64_t off = 0; st == OK && off < len; off += PAGE_SIZE) {
        volatile uint8_t *p = base + off;
        *p = *p;
    }
    return st;
}

/* Check the state just mapped at s->h against w, and set it up empty if
 * it is new or fails a check. */
static enum svcstate_start examine(struct svcstate *s, const struct svcstate_layout *w)
{
    struct svcstate_header *h = s->h;
    if (all_zero(h, sizeof(*h))) {
        set_up(h, w);
        return SVCSTATE_FRESH;
    }
    s->why = check_header(h, w);
    if (s->why) {
        printf("svcstate: the state of service %x is refused (%s): starting fresh\n", w->kind,
               s->why);
        set_up(h, w);
        return SVCSTATE_REFUSED;
    }
    h->adopted++;
    return SVCSTATE_ADOPTED;
}

status_t svcstate_open(handle_t vmo, const struct svcstate_layout *want, struct svcstate *out,
                       enum svcstate_start *how)
{
    uint64_t need = svcstate_size(want), size = 0;
    if (!need)
        return ERR_OUT_OF_RANGE;
    status_t st = jam_vmo_get_size(vmo, &size);
    if (st != OK)
        return st;
    if (size < need || size > SVCSTATE_MAX_SIZE)
        return ERR_OUT_OF_RANGE;
    uint64_t addr = SVCSTATE_ADDR;
    st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, size,
                      VMAR_READ | VMAR_WRITE | VMAR_FIXED, &addr);
    if (st != OK)
        return st;
    uint8_t *base = (uint8_t *)(uintptr_t)addr;
    st = prepare_pages(vmo, base, user_off(want->req_cap, want->rep_cap));
    if (st != OK) {
        (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), addr, size);   /* ours: can't fail */
        return st;
    }
    struct svcstate s = { .h = (struct svcstate_header *)base, .mapped = size };
    enum svcstate_start start = examine(&s, want);
    s.next_seq = next_seq(s.h);
    *out = s;
    *how = start;
    return OK;
}

void svcstate_close(struct svcstate *s)
{
    if (s->h)
        (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)s->h,
                             s->mapped);   /* our own mapping: can't fail */
    s->h = NULL;
}

void *svcstate_user(const struct svcstate *s)
{
    return (uint8_t *)s->h + user_off(s->h->req_cap, s->h->rep_cap);
}

void *svcstate_request(const struct svcstate *s, unsigned slot, uint32_t *len)
{
    slot &= 1;
    if (len)
        *len = s->h->slot[slot].len;
    return (uint8_t *)s->h + req_off(s->h, slot);
}

void *svcstate_reply_area(const struct svcstate *s, unsigned slot)
{
    return (uint8_t *)s->h + rep_off(s->h, slot & 1);
}

/* Set slot i up for request number seq read from channel key: its length
 * zeroed first, its number last, so a successor never takes the stale
 * bytes of an older request for this one. */
static void set_up_slot(struct svcstate_slot *sl, uint64_t seq, uint32_t key)
{
    __atomic_store_n(&sl->len, 0, __ATOMIC_RELEASE);
    sl->nhandles = 0;
    sl->phase = SVCSTATE_RUN;
    sl->reply_len = 0;
    sl->runs = 0;
    sl->channel = key;
    __atomic_store_n(&sl->seq, seq, __ATOMIC_RELEASE);
}

void svcstate_slot(struct svcstate *s, unsigned slot, struct idl_slot *out)
{
    struct svcstate_slot *sl = &s->h->slot[slot & 1];
    *out = (struct idl_slot){
        .q = svcstate_request(s, slot, NULL), .qcap = s->h->req_cap, .n = &sl->len,
        .nh = &sl->nhandles, .hs = s->handles, .hcap = SVCSTATE_SLOT_HANDLES,
        .r = svcstate_reply_area(s, slot),
    };
}

void svcstate_prepare(struct svcstate *s, uint32_t key, unsigned *slot, struct idl_slot *out)
{
    unsigned i = (unsigned)(s->next_seq & 1);
    set_up_slot(&s->h->slot[i], s->next_seq, key);
    svcstate_slot(s, i, out);
    *slot = i;
}

bool svcstate_taken(struct svcstate *s, unsigned slot)
{
    struct svcstate_slot *sl = &s->h->slot[slot & 1];
    if (sl->seq != s->next_seq || sl->len < 4 || !taken(s->h, sl)) {
        /* Nothing taken (a read that found the request too big wrote its
         * length): the slot stays set up for the next prepare. */
        __atomic_store_n(&sl->len, 0, __ATOMIC_RELEASE);
        sl->nhandles = 0;
        return false;
    }
    s->next_seq++;
    return true;
}

status_t svcstate_take(struct svcstate *s, uint32_t key, handle_t ch, unsigned *slot)
{
    unsigned i;
    struct idl_slot is;
    svcstate_prepare(s, key, &i, &is);
    status_t st = drv_channel_read(ch, is.q, is.qcap, is.n, is.hs, is.hcap, is.nh);
    if (st == OK && *is.n < 4) {
        for (unsigned j = 0; j < *is.nh; j++)
            jam_handle_close(s->handles[j]);
        st = ERR_INVALID_ARGS;
    }
    /* A failed read left no request to take (a too big one's length is
     * past req_cap, or its handles past the slot's): the slot is set back. */
    if (!svcstate_taken(s, i) || st != OK)
        return st == OK ? ERR_INTERNAL : st;
    *slot = i;
    return OK;
}

status_t svcstate_commit(struct svcstate *s, unsigned slot, uint32_t reply_len)
{
    struct svcstate_slot *sl = &s->h->slot[slot & 1];
    if (reply_len > s->h->rep_cap)
        return ERR_OUT_OF_RANGE;
    sl->reply_len = reply_len;
    __atomic_store_n(&s->h->commit, sl->seq, __ATOMIC_RELEASE);
    return OK;
}

void svcstate_sent(struct svcstate *s, unsigned slot)
{
    __atomic_store_n(&s->h->slot[slot & 1].phase, SVCSTATE_SENT, __ATOMIC_RELEASE);
}

status_t svcstate_reply(struct svcstate *s, unsigned slot, handle_t ch, const handle_t *hs,
                        uint32_t nh)
{
    struct svcstate_slot *sl = &s->h->slot[slot & 1];
    if (s->h->commit < sl->seq) {
        status_t st = svcstate_commit(s, slot, sl->reply_len);
        if (st != OK)
            return st;
    }
    if (sl->phase != SVCSTATE_SENT)
        svcstate_sent(s, slot);
    return jam_channel_write(ch, svcstate_reply_area(s, slot), sl->reply_len, hs, nh);
}

enum svcstate_case svcstate_pending(const struct svcstate *s, unsigned *slot)
{
    const struct svcstate_header *h = s->h;
    unsigned i = h->slot[1].seq > h->slot[0].seq;
    const struct svcstate_slot *cur = &h->slot[i];
    if (!cur->seq || !taken(h, cur))
        return SVCSTATE_IDLE;
    *slot = i;
    if (h->commit < cur->seq)
        return SVCSTATE_RERUN;
    return cur->phase == SVCSTATE_SENT ? SVCSTATE_REPLY : SVCSTATE_RESEND;
}

/* ---- promoting a spare ------------------------------------------------------------------ */

/* The promotion message for a into a new buffer (*out, *len). */
static status_t build_promotion(const struct standby_args *a, uint8_t **out, uint32_t *len)
{
    unsigned envc = 0;
    while (a->envp && a->envp[envc] && envc <= STANDBY_MAX_STRINGS)
        envc++;
    if (a->argc < 0 || a->n > STANDBY_MAX_HANDLES ||
        (unsigned)a->argc + envc > STANDBY_MAX_STRINGS)
        return ERR_OUT_OF_RANGE;
    size_t size = sizeof(struct standby_msg);
    for (int i = 0; i < a->argc; i++)
        size += strlen(a->argv[i]) + 1;
    for (unsigned i = 0; i < envc; i++)
        size += strlen(a->envp[i]) + 1;
    if (size > STANDBY_MAX_BYTES)
        return ERR_OUT_OF_RANGE;
    uint8_t *buf = calloc(1, size);
    if (!buf)
        return ERR_NO_MEMORY;
    struct standby_msg *m = (struct standby_msg *)buf;
    *m = (struct standby_msg){
        .magic = STANDBY_MAGIC, .version = STANDBY_VERSION, .argc = (uint32_t)a->argc,
        .envc = envc, .nhandles = a->n, .kill_ns = a->kill_ns,
        .strings_len = (uint32_t)(size - sizeof(*m)),
    };
    for (unsigned i = 0; i < a->n; i++)
        m->roles[i] = a->hs[i].role;
    char *p = (char *)(m + 1);
    for (unsigned i = 0; i < (unsigned)a->argc + envc; i++) {
        const char *str = i < (unsigned)a->argc ? a->argv[i] : a->envp[i - a->argc];
        size_t l = strlen(str) + 1;
        memcpy(p, str, l);
        p += l;
    }
    *out = buf;
    *len = (uint32_t)size;
    return OK;
}

status_t standby_promote(handle_t ch, const struct standby_args *a)
{
    handle_t hs[STANDBY_MAX_HANDLES];
    rights_t rs[STANDBY_MAX_HANDLES];
    unsigned n = a->n < STANDBY_MAX_HANDLES ? a->n : STANDBY_MAX_HANDLES;
    for (unsigned i = 0; i < n; i++) {
        hs[i] = a->hs[i].h;
        rs[i] = a->rights && a->rights[i] ? a->rights[i] : RIGHT_SAME;
    }
    uint8_t *buf = NULL;
    uint32_t len = 0;
    status_t st = build_promotion(a, &buf, &len);
    if (st == OK)
        st = jam_channel_write_rights(ch, buf, len, hs, rs, n);
    free(buf);
    /* Consumed whatever happens: a failed write left them with us. */
    for (unsigned i = 0; st != OK && i < a->n; i++)
        jam_handle_close(a->hs[i].h);
    return st;
}
