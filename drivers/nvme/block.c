/* nvme: the `block` protocol (abi/idl/block.idl), one channel per opened
 * partition (storage.open_partition, serve.c). Read-only unless opened
 * writable on a Jam OS drive (MODE_JAMOS: serve.c asks for nothing else,
 * and blk_open checks again): a read-only channel says read_only 1 and
 * answers block.write ERR_ACCESS_DENIED before looking at anything, and
 * block.sync OK at once (nothing was written through it). A writable
 * channel's writes go out with FUA (answered: on the medium), and its sync
 * sends a Flush (answered at once by a drive without a volatile cache).
 *
 * Authority: a channel reaches its partition's blocks and nothing else.
 * Every request's range is checked against the partition here
 * (check_range) before the partition's start is added; the table the
 * bounds come from is the one this driver read itself (table.c,
 * <jam/parttable.h>). cmd.c checks a write's blocks once more against the
 * drive's mode and partitions of its own (sqe_write).
 *
 * Data: the buffer of map_buffer is a VMO made here (64 KiB if the client
 * leaves it to us, BLK_BUF_MAX at most), pinned writable once when it is
 * made (the device writes what it reads straight into it, and reads what
 * it writes from it; a pin costs an IOMMU invalidation, so never per
 * transfer) and unpinned once nothing of the channel's is out any more.
 * The driver never maps it. The client's handle may map and read it, and
 * write it only on a writable channel; never RIGHT_RESIZE (the pages under
 * the pin stay).
 *
 * Several requests in flight: up to BLK_DEPTH requests of a channel are
 * out at once (block.info's queue_depth), each an nvme_io of its own
 * (queue.c cuts it into commands); more wait unread on the channel, never
 * refused. Each is answered by its txid when its last command completes,
 * with the first failure: answers come in completion order.
 *
 * The fence: a request whose client has gone is dropped, never started
 * (client_gone, before anything is sent): a write too. A channel whose
 * client has gone (blk_gone) is read no more; its requests not yet sent
 * are taken back (io_cancel), and those already out are waited for.
 *
 * The drain rule: NVMe completes commands in any order, so a request a
 * dead client left out could land after a later one of its successor's
 * (a write could put older data over newer).
 * So a partition whose closed channel still has requests out gets no new
 * request from any channel until they have completed (milliseconds): its
 * channels are not read meanwhile, and a request read in the moment the
 * other client died waits (`held`) until the partition is quiet
 * (part_quiet looks at every busy channel of the partition before any new
 * request goes out: a successor's request comes after its predecessor's
 * death, so the death is seen first). */
#include <idl/block.h>
#include <jam/parttable.h>
#include "nvme.h"

#define BLK_MAX     8                          /* channels open at once */
#define BLK_DEPTH   32                         /* a channel's requests out at once */
#define BLK_BUF     (64u << 10)                /* the buffer when the client leaves it to us */
#define BLK_BUF_MAX (1u << 20)                 /* the largest (block.info's buffer_max) */
#define BLK_PAGES   (BLK_BUF_MAX / NVME_PAGE)
#define BLK_SLOW_STEP (20 * NS_PER_MS)          /* NVME_TEST_SLOW: between a channel's answers */

struct blk;

/* One request of a channel's (a read, a write or a sync's flush), held or out. */
struct blk_req {
    struct nvme_io io;         /* io.ctx: this request; io.kind: what it is */
    struct idl_txn txn;        /* answered on it (unless the client is gone) */
    struct blk    *b;          /* its channel */
    bool           used;       /* held or out */
    bool           held;       /* waits for its partition to drain: not given to nvme_submit */
    bool           late;       /* NVME_TEST_SLOW: done, answered at `due` (counts as out) */
    status_t       st;         /* a late one's status */
    uint64_t       due;
};

/* An open block channel. */
struct blk {
    handle_t ch;               /* our end; HANDLE_INVALID: a free slot */
    uint16_t gen;              /* bumped at every open and free: in its port key */
    uint8_t  part;             /* the partition's index in the table */
    uint64_t start, blocks;    /* its first block and length, in the disk's blocks */
    bool     writable;         /* opened writable (on a MODE_JAMOS drive alone) */
    bool     pending;          /* may have requests queued */
    bool     closed;           /* the client is gone: its requests out are waited for */
    handle_t vmo;              /* the buffer; HANDLE_INVALID: no map_buffer yet */
    uint32_t size;             /* its bytes (whole pages) */
    uint64_t pin;              /* its pin, while pinned */
    bool     pinned;
    uint32_t busy;             /* requests used */
    uint32_t dropped;          /* requests of a gone client never started */
    uint64_t next_due;         /* NVME_TEST_SLOW: its next answer, not before */
    uint64_t pages[BLK_PAGES]; /* the buffer's pages, as the device reaches them */
    struct blk_req reqs[BLK_DEPTH];
};

static struct blk blks[BLK_MAX] = { [0 ... BLK_MAX - 1] = { .ch = HANDLE_INVALID,
                                                            .vmo = HANDLE_INVALID } };
/* Per partition: requests out of channels whose client has gone. */
static uint32_t draining[PT_MAX_PARTS];
static struct nvme *nv;          /* the controller (blk_open's) */

static uint64_t blk_key(const struct blk *b)
{
    return KEY_BLK | (uint64_t)b->gen << 8 | (uint64_t)(b - blks);
}

/* Has b's client closed its end (or died, which closes it)? A wait that
 * fails for any other reason than "not yet" counts as gone: dropping a
 * request is always safe, running one for a client that left may not be. */
static bool client_gone(const struct blk *b)
{
    signals_t seen = 0;
    return drv_object_wait_one(b->ch, SIG_PEER_CLOSED, 0, &seen) != ERR_TIMED_OUT;
}

/* b's slot free again: its buffer unpinned (nothing of b's is out), its
 * channel closed. */
static void blk_free(struct nvme *c, struct blk *b)
{
    if (b->pinned && drv_vmo_unpin(b->vmo, c->dma, b->pin) == OK)
        b->pinned = false;
    if (b->pinned)   /* the kernel keeps it: the pages stay the driver's until it ends */
        drv_log("%s: partition %u: the buffer's pin wasn't released", c->name, b->part + 1);
    if (b->vmo != HANDLE_INVALID)
        drv_handle_close(b->vmo);
    if (b->ch != HANDLE_INVALID) {
        /* A binding holds its channel until unbound: closing alone would
         * leave both on the port. */
        (void)drv_port_unbind(c->port, b->ch, blk_key(b));
        drv_handle_close(b->ch);
    }
    uint16_t gen = (uint16_t)(b->gen + 1);
    __builtin_memset(b, 0, sizeof(*b));
    b->ch = b->vmo = HANDLE_INVALID;
    b->gen = gen;
}

static void req_free(struct blk_req *r)
{
    r->used = r->held = r->late = false;
    r->b->busy--;
}

static void part_resume(struct nvme *c, uint8_t part);

/* b's client is gone: nothing more is read from b; what it has held or
 * waiting is dropped, what is out is waited for (the drain rule). */
static void blk_gone(struct nvme *c, struct blk *b)
{
    if (b->closed)
        return;
    b->closed = true;
    b->pending = false;
    uint32_t out = 0, never = 0;
    for (unsigned i = 0; i < BLK_DEPTH; i++) {
        struct blk_req *r = &b->reqs[i];
        if (!r->used)
            continue;
        if (r->held || (!r->late && io_cancel(c, &r->io))) {
            req_free(r);
            never++;
        } else {
            out++;
        }
    }
    b->dropped += never;
    draining[b->part] += out;
    if (out || b->dropped)
        drv_log("%s: partition %u: its client left with %u request(s) out (waited for: the "
                "partition gets no new command until they complete) and %u never started "
                "(dropped)", c->name, b->part + 1, out, b->dropped);
    if (!b->busy)
        blk_free(c, b);
}

/* Is partition `part` free of requests a gone client left out? Each busy
 * channel of it but `self` (whose client was looked at just now) is looked
 * at first, so a client that has just died is seen before anything new
 * goes out. */
static bool part_quiet(struct nvme *c, uint8_t part, const struct blk *self)
{
    for (unsigned i = 0; i < BLK_MAX; i++) {
        struct blk *b = &blks[i];
        if (b != self && b->ch != HANDLE_INVALID && b->part == part && !b->closed && b->busy &&
            client_gone(b))
            blk_gone(c, b);
    }
    return draining[part] == 0;
}

/* r's answer on its txn (ERR_PEER_CLOSED: the client left meanwhile). */
static void req_reply(uint8_t kind, struct idl_txn txn, status_t st)
{
    if (kind == IO_WRITE)
        (void)block_reply_write(txn, st);
    else if (kind == IO_FLUSH)
        (void)block_reply_sync(txn, st);
    else
        (void)block_reply_read(txn, st);
}

/* Request r is over (with st): answered, or (its client gone) forgotten. */
static void req_finish(struct nvme *c, struct blk_req *r, status_t st)
{
    struct blk *b = r->b;
    struct idl_txn txn = r->txn;
    uint8_t kind = r->io.kind;
    req_free(r);
    if (!b->closed) {
        req_reply(kind, txn, st);
        b->pending = true;   /* room for one more */
        return;
    }
    uint8_t part = b->part;
    draining[part]--;
    if (!b->busy)
        blk_free(c, b);
    if (!draining[part])
        part_resume(c, part);
}

/* A request's last command completed (or the driver is stopping). */
static void req_done(struct nvme *c, struct nvme_io *io, status_t st)
{
    struct blk_req *r = io->ctx;
    struct blk *b = r->b;
    if ((c->test & NVME_TEST_SLOW) && !c->stopping) {
        uint64_t now = drv_clock_ns();
        r->late = true;
        r->st = st;
        r->due = b->next_due > now ? b->next_due : now;
        b->next_due = r->due + BLK_SLOW_STEP;
        return;
    }
    req_finish(c, r, st);
}

void blk_due(struct nvme *c)
{
    uint64_t now = drv_clock_ns();
    for (unsigned i = 0; i < BLK_MAX; i++)
        for (unsigned k = 0; k < BLK_DEPTH && blks[i].ch != HANDLE_INVALID; k++) {
            struct blk_req *r = &blks[i].reqs[k];
            if (r->used && r->late && r->due <= now)
                req_finish(c, r, r->st);
        }
}

uint64_t blk_next_due(void)
{
    uint64_t d = DEADLINE_NEVER;
    for (unsigned i = 0; i < BLK_MAX; i++)
        for (unsigned k = 0; k < BLK_DEPTH && blks[i].ch != HANDLE_INVALID; k++) {
            const struct blk_req *r = &blks[i].reqs[k];
            if (r->used && r->late && r->due < d)
                d = r->due;
        }
    return d;
}

/* r (held, or just read) goes out. Its answer comes to req_done, unless
 * nvme_submit refuses it now: then it is answered here. */
static void req_send(struct nvme *c, struct blk_req *r)
{
    r->held = false;
    status_t st = nvme_submit(c, &r->io);
    if (st != OK) {
        struct idl_txn txn = r->txn;
        uint8_t kind = r->io.kind;
        req_free(r);
        req_reply(kind, txn, st);
    }
}

/* Partition `part` is quiet again: the requests held for it go out, and
 * its channels are read again. */
static void part_resume(struct nvme *c, uint8_t part)
{
    if (c->stopping)
        return;
    drv_log("%s: partition %u: the gone client's requests are done: its channels go on",
            c->name, part + 1);
    for (unsigned i = 0; i < BLK_MAX && !draining[part]; i++) {
        struct blk *b = &blks[i];
        if (b->ch == HANDLE_INVALID || b->part != part || b->closed)
            continue;
        if (client_gone(b)) {
            blk_gone(c, b);
            continue;
        }
        b->pending = true;
        for (unsigned k = 0; k < BLK_DEPTH && !draining[part]; k++)
            if (b->reqs[k].used && b->reqs[k].held)
                req_send(c, &b->reqs[k]);
    }
}

/* ---- the block protocol ------------------------------------------------------------- */

static status_t b_info(void *ctx, uint32_t *block_size, uint64_t *blocks, uint8_t *read_only,
                       uint32_t *max_blocks, uint16_t *queue_depth, uint32_t *buffer_max)
{
    const struct blk *b = ctx;
    *block_size = nv->ns.block_size;
    *blocks = b->blocks;
    *read_only = !b->writable;
    *max_blocks = BLK_BUF_MAX / nv->ns.block_size;
    *queue_depth = BLK_DEPTH;
    *buffer_max = BLK_BUF_MAX;
    return OK;
}

/* The buffer's size for a client that asked for `want` bytes. */
static uint32_t buffer_size(uint32_t want)
{
    if (!want)
        return BLK_BUF;
    if (want > BLK_BUF_MAX)
        return BLK_BUF_MAX;
    return (want + NVME_PAGE - 1) & ~(NVME_PAGE - 1);   /* BLK_BUF_MAX is whole pages */
}

static status_t b_map_buffer(void *ctx, uint32_t want, handle_t *buffer, uint32_t *size)
{
    struct blk *b = ctx;
    if (b->vmo != HANDLE_INVALID)
        return ERR_BAD_STATE;
    uint32_t bytes = buffer_size(want);
    handle_t vmo, theirs = HANDLE_INVALID;
    status_t st = drv_vmo_create(bytes, 0, &vmo);
    if (st != OK)
        return st;
    st = drv_vmo_pin(vmo, nv->dma, 0, bytes, b->pages, &b->pin);
    if (st == OK) {
        b->pinned = true;
        /* To map and read, and write only on a writable channel (see the header). */
        rights_t rights = RIGHT_READ | RIGHT_MAP | RIGHT_TRANSFER;
        st = drv_handle_duplicate(vmo, b->writable ? rights | RIGHT_WRITE : rights, &theirs);
    }
    if (st != OK) {
        if (b->pinned && drv_vmo_unpin(vmo, nv->dma, b->pin) == OK)
            b->pinned = false;
        if (!b->pinned)
            drv_handle_close(vmo);
        else
            b->vmo = vmo;   /* kept with its pin: freed with the channel */
        return st;
    }
    b->vmo = vmo;
    b->size = bytes;
    *buffer = theirs;
    *size = bytes;
    return OK;
}

/* Is [lba, lba + count) inside the partition, and count blocks at
 * `offset` inside the buffer, `offset` a multiple of 4 (NVMe's PRPs)?
 * Written so nothing can wrap. */
static status_t check_range(const struct blk *b, uint64_t lba, uint32_t count, uint32_t offset)
{
    uint32_t bs = nv->ns.block_size;
    if (b->vmo == HANDLE_INVALID || !b->size)
        return ERR_BAD_STATE;   /* no buffer yet */
    if (!count || (offset & 3))
        return ERR_INVALID_ARGS;
    if (count > b->size / bs || offset > b->size || count * bs > b->size - offset)
        return ERR_OUT_OF_RANGE;
    if (lba >= b->blocks || count > b->blocks - lba)
        return ERR_OUT_OF_RANGE;
    return OK;
}

/* A request of b's: fenced, its range checked (not a flush's), then sent,
 * or held while its partition drains. IDL_LATER, or the refusal. */
static status_t req_start(struct blk *b, struct idl_txn txn, const struct nvme_io *io)
{
    if (client_gone(b)) {   /* the fence: never started; the answer would go nowhere */
        b->dropped++;
        return ERR_PEER_CLOSED;
    }
    status_t st = io->kind == IO_FLUSH ? OK : check_range(b, io->lba, io->blocks, io->offset);
    if (st != OK)
        return st;
    struct blk_req *r = NULL;
    for (unsigned i = 0; i < BLK_DEPTH && !r; i++)
        if (!b->reqs[i].used)
            r = &b->reqs[i];
    if (!r)
        return ERR_NO_RESOURCES;   /* not reached: a channel at BLK_DEPTH isn't read */
    *r = (struct blk_req){ .txn = txn, .b = b, .used = true, .io = *io };
    if (io->kind != IO_FLUSH)
        r->io.lba = b->start + io->lba;
    r->io.part = b->part;
    r->io.done = req_done;
    r->io.ctx = r;
    b->busy++;
    if (!part_quiet(nv, b->part, b)) {
        r->held = true;   /* part_resume sends it */
        return IDL_LATER;
    }
    req_send(nv, r);
    return IDL_LATER;
}

/* A read or write's nvme_io, relative to the partition (req_start adds
 * its start). */
static struct nvme_io rw_io(const struct blk *b, uint8_t kind, uint64_t lba, uint32_t count,
                            uint32_t offset)
{
    return (struct nvme_io){ .kind = kind, .lba = lba, .blocks = count, .pages = b->pages,
                             .npages = b->size / NVME_PAGE, .offset = offset };
}

static status_t b_read(void *ctx, struct idl_txn txn, uint64_t lba, uint32_t count,
                       uint32_t offset)
{
    struct blk *b = ctx;
    struct nvme_io io = rw_io(b, IO_READ, lba, count, offset);
    return req_start(b, txn, &io);
}

static status_t b_write(void *ctx, struct idl_txn txn, uint64_t lba, uint32_t count,
                        uint32_t offset)
{
    struct blk *b = ctx;
    if (!b->writable || cmd_mode() != MODE_JAMOS)
        return ERR_ACCESS_DENIED;   /* before anything else */
    struct nvme_io io = rw_io(b, IO_WRITE, lba, count, offset);
    return req_start(b, txn, &io);
}

/* Every write answered before it is on the medium already (FUA); a Flush
 * anyway, for the drive's cache, where there is one to flush. */
static status_t b_sync(void *ctx, struct idl_txn txn)
{
    struct blk *b = ctx;
    if (!b->writable || !nv->vwc) {   /* answered now: nothing to flush */
        if (client_gone(b)) {
            b->dropped++;
            return ERR_PEER_CLOSED;
        }
        return OK;
    }
    struct nvme_io io = { .kind = IO_FLUSH };
    return req_start(b, txn, &io);
}

static const struct block_ops block_ops = {
    .info = b_info,
    .map_buffer = b_map_buffer,
    .read = b_read,
    .write = b_write,
    .sync = b_sync,
};

/* ---- the channels -------------------------------------------------------------------- */

status_t blk_open(struct nvme *c, uint8_t index, uint64_t start, uint64_t blocks, bool writable,
                  handle_t *out)
{
    nv = c;
    if (writable && (cmd_mode() != MODE_JAMOS || index >= 2))
        return ERR_ACCESS_DENIED;
    if (index >= PT_MAX_PARTS)
        return ERR_OUT_OF_RANGE;
    struct blk *b = NULL;
    for (unsigned i = 0; i < BLK_MAX && !b; i++)
        if (blks[i].ch == HANDLE_INVALID)
            b = &blks[i];
    if (!b)
        return ERR_NO_RESOURCES;
    handle_t ours, theirs;
    status_t st = drv_channel_create(&ours, &theirs);
    if (st != OK)
        return st;
    b->gen++;
    st = drv_port_bind(c->port, ours, blk_key(b), SIG_READABLE | SIG_PEER_CLOSED,
                       PORT_BIND_PERSISTENT);
    if (st != OK) {
        drv_handle_close(ours);
        drv_handle_close(theirs);
        return st;
    }
    b->ch = ours;
    b->part = index;
    b->start = start;
    b->blocks = blocks;
    b->writable = writable;
    b->pending = true;   /* look once: a request may be there already */
    *out = theirs;
    return OK;
}

void blk_packet(uint64_t key)
{
    unsigned i = key & 0xff, gen = (key >> 8) & 0xffff;
    if (i < BLK_MAX && blks[i].ch != HANDLE_INVALID && blks[i].gen == gen && !blks[i].closed)
        blks[i].pending = true;
}

/* May one more request of b's be read now? */
static bool room(const struct blk *b)
{
    return b->ch != HANDLE_INVALID && !b->closed && b->busy < BLK_DEPTH && !draining[b->part];
}

/* ... and may b have one queued? */
static bool readable(const struct blk *b)
{
    return b->pending && room(b);
}

bool blk_ready(void)
{
    for (unsigned i = 0; i < BLK_MAX; i++)
        if (readable(&blks[i]))
            return true;
    return false;
}

bool blk_serve(struct nvme *c)
{
    bool any = false;
    for (unsigned i = 0; i < BLK_MAX; i++) {
        struct blk *b = &blks[i];
        if (!readable(b))
            continue;
        any = true;
        b->pending = false;
        /* A few at a time, so one busy client doesn't starve the others. */
        status_t st = OK;
        for (int guard = 0; guard < 16 && st == OK && room(b); guard++)
            st = block_serve_one(b->ch, &block_ops, b);
        if (st == OK && !b->closed)
            b->pending = true;   /* maybe more: the port won't say so again */
        else if (st != OK && st != ERR_SHOULD_WAIT)
            blk_gone(c, b);      /* ERR_PEER_CLOSED: nothing more queued, the client gone */
    }
    return any;
}

void blk_close_all(struct nvme *c)
{
    for (unsigned i = 0; i < BLK_MAX; i++)
        if (blks[i].ch != HANDLE_INVALID)
            blk_free(c, &blks[i]);
    for (unsigned p = 0; p < PT_MAX_PARTS; p++)
        draining[p] = 0;
}
