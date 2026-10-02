/* The update fetcher's window (<updfetch.h>): which requests are in
 * flight, which are due again, and whether a reply answers one of them.
 *
 * A request lives in one of UPDFETCH_WINDOW slots from its first send
 * until a reply that matches it exactly is stored; requests are issued
 * in file order, so "below the cursor and in no slot" means stored, and
 * no bitmap is needed. A reply is believed only as far as the slot it
 * matches: a server (or anyone else on the network) can't make the
 * fetcher store bytes it didn't ask for, at an offset it didn't ask for,
 * or past the size the manifest gave. */
#include <updfetch.h>

static void fail(struct updfetch *f, status_t why)
{
    f->state = UPDFETCH_FAILED;
    f->why = why;
    memset(f->slots, 0, sizeof(f->slots));
}

static void send_slot(struct updfetch *f, struct updfetch_slot *s, uint64_t now)
{
    struct updwire_req r = {
        .file = s->file, .snapshot = s->file == UPDWIRE_MANIFEST ? 0 : f->snapshot,
        .offset = s->offset, .length = s->length,
    };
    uint8_t d[UPDWIRE_REQ_SIZE];
    if (updwire_req_encode(&r, d) == OK)
        (void)f->io.send(f->io.ctx, d, sizeof(d));   /* a failed send is a lost datagram */
    if (s->tries)
        f->resent++;
    f->sent++;
    s->tries++;
    s->sent_at = now;
}

/* Ask for the manifest (a new snapshot): every slot free but this one. */
static void ask_manifest(struct updfetch *f)
{
    memset(f->slots, 0, sizeof(f->slots));
    f->state = UPDFETCH_MANIFEST;
    f->snapshot = 0;
    f->slots[0] = (struct updfetch_slot){
        .used = true, .file = UPDWIRE_MANIFEST, .offset = 0, .length = UPDWIRE_CHUNK_MAX,
    };
}

void updfetch_start(struct updfetch *f, const struct updfetch_io *io)
{
    memset(f, 0, sizeof(*f));
    f->io = *io;
    ask_manifest(f);
}

/* Free slots get the next pieces of the files, in order. */
static void fill(struct updfetch *f, uint64_t now)
{
    for (unsigned i = 0; i < UPDFETCH_WINDOW && f->file < UPDATE_FILES; i++) {
        struct updfetch_slot *s = &f->slots[i];
        if (s->used)
            continue;
        uint64_t size = f->m.file[f->file].size;
        uint64_t left = size - f->next;
        uint16_t len = (uint16_t)(left < UPDWIRE_CHUNK_MAX ? left : UPDWIRE_CHUNK_MAX);
        *s = (struct updfetch_slot){
            .used = true, .file = (uint8_t)(f->file + 1), .offset = (uint32_t)f->next,
            .length = len,
        };
        send_slot(f, s, now);
        f->next += len;
        if (f->next == size) {
            f->file++;
            f->next = 0;
        }
    }
}

uint64_t updfetch_poll(struct updfetch *f, uint64_t now)
{
    if (f->state == UPDFETCH_DONE || f->state == UPDFETCH_FAILED)
        return UINT64_MAX;
    for (unsigned i = 0; i < UPDFETCH_WINDOW; i++) {
        struct updfetch_slot *s = &f->slots[i];
        if (!s->used || (s->tries && now - s->sent_at < UPDFETCH_RETRY))
            continue;
        if (s->tries >= UPDFETCH_TRIES) {
            fail(f, ERR_TIMED_OUT);
            return UINT64_MAX;
        }
        send_slot(f, s, now);
    }
    if (f->state == UPDFETCH_FILES)
        fill(f, now);
    uint64_t next = UINT64_MAX;
    for (unsigned i = 0; i < UPDFETCH_WINDOW; i++)
        if (f->slots[i].used && f->slots[i].sent_at + UPDFETCH_RETRY < next)
            next = f->slots[i].sent_at + UPDFETCH_RETRY;
    return next;
}

/* The slot a reply answers: same file and offset, the snapshot ours (any,
 * for the manifest's) and the file the manifest's size, and as many bytes
 * as the request could get. Anything else is someone else's datagram, or
 * a broken server's: ignored, so the request times out. */
static struct updfetch_slot *match(struct updfetch *f, const struct updwire_rep *r)
{
    if (r->file != UPDWIRE_MANIFEST &&
        (r->snapshot != f->snapshot ||
         (r->status == UPDWIRE_OK && r->file_size != f->m.file[r->file - 1].size)))
        return NULL;
    for (unsigned i = 0; i < UPDFETCH_WINDOW; i++) {
        struct updfetch_slot *s = &f->slots[i];
        if (!s->used || s->file != r->file || s->offset != r->offset)
            continue;
        if (r->status != UPDWIRE_OK)
            return s;
        uint32_t room = r->file_size - s->offset;   /* rep_decode: offset <= file_size */
        if (r->length == (room < s->length ? room : s->length))
            return s;
    }
    return NULL;
}

static void took_manifest(struct updfetch *f, const struct updwire_rep *r)
{
    struct update_manifest m;
    if (r->length != r->file_size) {
        fail(f, ERR_OUT_OF_RANGE);   /* a manifest is one datagram */
        return;
    }
    status_t st = update_manifest_parse(r->data, r->length, &m);
    if (st == OK)
        st = f->io.begin(f->io.ctx, &m, r->data, r->length);
    if (st != OK) {
        fail(f, st);
        return;
    }
    memset(f->slots, 0, sizeof(f->slots));
    f->m = m;
    f->snapshot = r->snapshot;
    f->state = UPDFETCH_FILES;
    f->file = UPDATE_KERNEL;
    f->next = f->stored = 0;
    f->total = m.file[UPDATE_KERNEL].size + m.file[UPDATE_BOOTFS].size;
}

static void took_piece(struct updfetch *f, struct updfetch_slot *s, const struct updwire_rep *r)
{
    status_t st = f->io.store(f->io.ctx, (unsigned)r->file - 1, r->offset, r->data, r->length);
    if (st != OK) {
        fail(f, st);
        return;
    }
    s->used = false;
    f->stored += r->length;
    if (f->stored == f->total)
        f->state = UPDFETCH_DONE;
}

void updfetch_reply(struct updfetch *f, const void *dgram, size_t len, uint64_t now)
{
    (void)now;
    struct updwire_rep r;
    if ((f->state != UPDFETCH_MANIFEST && f->state != UPDFETCH_FILES) ||
        updwire_rep_decode(dgram, len, &r) != OK) {
        f->ignored++;
        return;
    }
    f->replies++;
    struct updfetch_slot *s = match(f, &r);
    if (!s) {
        f->ignored++;   /* a repeat, a stale snapshot's, or nothing we asked for */
        return;
    }
    if (r.status == UPDWIRE_GONE && f->state == UPDFETCH_FILES) {
        if (++f->restarts > UPDFETCH_RESTARTS)
            fail(f, ERR_NOT_FOUND);
        else
            ask_manifest(f);
        return;
    }
    if (r.status != UPDWIRE_OK)
        fail(f, ERR_IO);
    else if (f->state == UPDFETCH_MANIFEST)
        took_manifest(f, &r);
    else
        took_piece(f, s, &r);
}
