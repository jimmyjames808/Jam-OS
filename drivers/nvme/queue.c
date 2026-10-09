/* nvme: completion queues, command slots, timeouts and reads (drv/nvme).
 *
 * Each queue pair has `depth` slots, at most depth - 1 in use, so its
 * submission queue can never overflow (an entry is free again before its
 * command completes). A command's id is its slot's index (6 bits) and the
 * slot's generation (10 bits, never 0x3ff, so no id is FFFFh), bumped at
 * every use: a completion carrying an older generation (a command
 * forgotten by a reset or after its abort) is counted and ignored, and one
 * for a free slot of the current generation means the queue is corrupt
 * (the controller is reset).
 *
 * A completion queue is one page of 16-byte entries, pinned writable,
 * taken by its phase bit; its head doorbell is written once per scan.
 *
 * Reads (struct nvme_io) wait on a list, first come first served, and are
 * cut into commands of at most c->max_cmd bytes (MDTS, and NVME_CMD_MAX so
 * one PRP list page always reaches: 257 pages at most). PRP 1 is the first
 * page's address plus the offset into it, PRP 2 the second page or the
 * slot's own PRP list page (pinned read only: the device only reads it).
 * A read is answered once, when its last command completes, with the
 * first failure. Nothing here writes a submission entry: cmd.c does. */
#include "nvme.h"

#define GEN_MOD 0x3ffu   /* generations 0..0x3fe */

/* ---- queues ------------------------------------------------------------------------ */

status_t queue_create(struct nvme *c, struct nvme_queue *q, uint16_t qid, uint16_t depth)
{
    *q = (struct nvme_queue){ .qid = qid, .depth = depth, .cq_vmo = HANDLE_INVALID, .phase = 1 };
    void *p = NULL;
    status_t st = drv_vmo_create(NVME_PAGE, DRV_VMO_CONTIGUOUS, &q->cq_vmo);
    if (st == OK)
        st = drv_vmo_map(q->cq_vmo, 0, NVME_PAGE, VMAR_READ | VMAR_WRITE, &p);
    if (st == OK)
        q->cq = p;
    if (st == OK && (st = drv_vmo_pin(q->cq_vmo, c->dma, 0, NVME_PAGE, &q->cq_addr,
                                      &q->cq_pin)) == OK)
        q->cq_pinned = true;
    if (st == OK)
        st = sq_create(c, q);
    if (st != OK) {
        queue_destroy(c, q);
        return st;
    }
    q->cq_db = (volatile uint8_t *)c->dbs + (2u * qid + 1) * c->dstrd;
    return OK;
}

void queue_destroy(struct nvme *c, struct nvme_queue *q)
{
    sq_destroy(c, q);
    if (q->cq_pinned && drv_vmo_unpin(q->cq_vmo, c->dma, q->cq_pin) == OK)
        q->cq_pinned = false;
    if (q->cq)
        (void)drv_vmo_unmap((void *)q->cq, NVME_PAGE);
    if (q->cq_vmo != HANDLE_INVALID)
        drv_handle_close(q->cq_vmo);
    q->cq = NULL;
    q->cq_vmo = HANDLE_INVALID;
    q->live = false;
}

void queue_reset(struct nvme_queue *q)
{
    for (uint32_t i = 0; i < NVME_PAGE / 4; i++)
        ((volatile uint32_t *)q->cq)[i] = 0;
    q->cq_head = 0;
    q->phase = 1;
    sq_reset(q);
}

/* ---- slots -------------------------------------------------------------------------- */

struct nvme_slot *slot_get(struct nvme *c, struct nvme_queue *q)
{
    if (!q->live || q->busy + 1 >= q->depth)
        return NULL;
    for (uint32_t i = 0; i < q->depth; i++) {
        struct nvme_slot *s = &q->slots[i];
        if (s->busy)
            continue;
        uint64_t now = drv_clock_ns();
        *s = (struct nvme_slot){ .busy = true, .gen = (uint16_t)((s->gen + 1) % GEN_MOD),
                                 .deadline = now + (q->qid ? IO_TIMEOUT : ADMIN_TIMEOUT) };
        if (q->qid && c->expire_next) {   /* the test word: this one has timed out */
            c->expire_next--;
            s->deadline = now;
        }
        q->busy++;
        return s;
    }
    return NULL;
}

static void slot_put(struct nvme_queue *q, struct nvme_slot *s)
{
    s->busy = false;
    q->busy--;
}

/* ---- reads ------------------------------------------------------------------------------ */

static void wait_remove(struct nvme *c, struct nvme_io *io)
{
    struct nvme_io **pp = &c->wait_head, *prev = NULL;
    while (*pp && *pp != io) {
        prev = *pp;
        pp = &(*pp)->next;
    }
    if (!*pp)
        return;
    *pp = io->next;
    if (c->wait_tail == io)
        c->wait_tail = prev;
    io->next = NULL;
    io->waiting = false;
}

static void wait_push(struct nvme *c, struct nvme_io *io, bool front)
{
    io->waiting = true;
    io->next = NULL;
    if (!c->wait_head) {
        c->wait_head = c->wait_tail = io;
    } else if (front) {
        io->next = c->wait_head;
        c->wait_head = io;
    } else {
        c->wait_tail->next = io;
        c->wait_tail = io;
    }
}

static void io_finish(struct nvme *c, struct nvme_io *io)
{
    c->reads++;
    io->done(c, io, io->st);
}

status_t nvme_read(struct nvme *c, struct nvme_io *io)
{
    uint32_t bs = c->ns.block_size;
    if (!c->ns.nsid)
        return ERR_BAD_STATE;
    if (!io->blocks || !io->done || (io->offset & 3) || !io->pages)
        return ERR_INVALID_ARGS;
    if (io->lba >= c->ns.blocks || io->blocks > c->ns.blocks - io->lba)
        return ERR_OUT_OF_RANGE;
    if ((uint64_t)io->offset + (uint64_t)io->blocks * bs > (uint64_t)io->npages * NVME_PAGE)
        return ERR_OUT_OF_RANGE;   /* past the buffer */
    io->sent = io->inflight = 0;
    io->st = OK;
    io->retried = false;
    wait_push(c, io, false);
    io_kick(c);
    return OK;
}

bool io_cancel(struct nvme *c, struct nvme_io *io)
{
    if (!io->waiting || io->sent || io->inflight)
        return false;
    wait_remove(c, io);
    return true;
}

/* The PRPs of `len` bytes at byte `boff` of io's buffer, for slot index i. */
static void prps(struct nvme *c, const struct nvme_io *io, uint64_t boff, uint32_t len, uint32_t i,
                 uint64_t *prp1, uint64_t *prp2)
{
    uint32_t p0 = (uint32_t)(boff / NVME_PAGE), off = (uint32_t)(boff % NVME_PAGE);
    uint32_t n = (off + len + NVME_PAGE - 1) / NVME_PAGE;   /* pages spanned, <= 257 */
    *prp1 = io->pages[p0] + off;
    *prp2 = 0;
    if (n == 2) {
        *prp2 = io->pages[p0 + 1];
    } else if (n > 2) {
        uint64_t *list = c->prp + (size_t)i * NVME_PRP_PER_PAGE;
        for (uint32_t k = 1; k < n; k++)
            list[k - 1] = io->pages[p0 + k];
        *prp2 = c->prp_addr[i];
    }
}

void io_kick(struct nvme *c)
{
    uint32_t bs = c->ns.block_size, most = c->max_cmd / bs;
    struct nvme_io *io;
    while ((io = c->wait_head)) {
        struct nvme_slot *s = slot_get(c, &c->io);
        if (!s)
            return;
        uint32_t n = io->blocks - io->sent;
        n = n < most ? n : most;
        uint64_t prp1, prp2;
        prps(c, io, io->offset + (uint64_t)io->sent * bs, n * bs, (uint32_t)(s - c->io.slots),
             &prp1, &prp2);
        s->io = io;
        sqe_read(c, s, io->lba + io->sent, n, prp1, prp2);
        io->sent += n;
        io->inflight++;
        if (io->sent == io->blocks)
            wait_remove(c, io);
    }
}

/* One of io's commands came back with `status`. */
static void io_cmd_done(struct nvme *c, struct nvme_io *io, uint16_t status, bool aborted)
{
    io->inflight--;
    if (status != NVME_SC_SUCCESS && io->st == OK) {
        io->st = aborted || status == NVME_SC_ABORTED ? ERR_TIMED_OUT : ERR_IO;
        if (c->read_fail++ < 8)
            drv_log("%s: read at lba %lu failed: status %#x%s", c->name, (unsigned long)io->lba,
                    status, aborted ? " (aborted after its deadline)" : "");
    }
    if (io->st != OK && io->waiting) {   /* the rest isn't sent */
        wait_remove(c, io);
        io->sent = io->blocks;
    }
    if (!io->inflight && !io->waiting)
        io_finish(c, io);
}

void io_end_all(struct nvme *c, status_t st)
{
    for (uint32_t i = 0; i < c->io.depth; i++) {
        struct nvme_slot *s = &c->io.slots[i];
        if (!s->busy)
            continue;
        slot_put(&c->io, s);
        if (s->io && s->io->inflight) {
            struct nvme_io *io = s->io;
            io->inflight = 0;
            io->st = io->st == OK ? st : io->st;
            if (io->waiting)
                wait_remove(c, io);
            io_finish(c, io);
        }
    }
    struct nvme_io *io;
    while ((io = c->wait_head)) {
        wait_remove(c, io);
        io->st = io->st == OK ? st : io->st;
        io_finish(c, io);
    }
}

/* ---- completions ------------------------------------------------------------------------ */

/* Slot cid of q finished with status and dw0. false: no command had that id. */
static bool complete(struct nvme *c, struct nvme_queue *q, uint16_t cid, uint16_t status,
                     uint32_t dw0)
{
    uint32_t i = cid & 0x3f, gen = cid >> 6;
    if (i >= q->depth)
        return false;
    struct nvme_slot *s = &q->slots[i];
    if (s->gen != gen) {   /* a command forgotten since (reset, abort): ignored */
        c->stale++;
        return true;
    }
    if (!s->busy)
        return false;
    slot_put(q, s);
    if (s->io)
        io_cmd_done(c, s->io, status, s->aborted);
    else if (s->done)
        s->done(c, s, status, dw0);
    return true;
}

bool queue_reap_one(struct nvme *c, struct nvme_queue *q)
{
    if (!q->live || !q->cq)
        return true;
    bool ok = true;
    uint16_t start = q->cq_head;
    for (uint32_t k = 0; k < q->depth; k++) {
        volatile uint8_t *e = q->cq + (uint32_t)q->cq_head * NVME_CQE_BYTES;
        uint32_t dw3 = *(volatile uint32_t *)(e + 12);
        if (((dw3 >> 16) & 1) != q->phase)
            break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);   /* the entry after its phase bit */
        uint32_t dw0 = *(volatile uint32_t *)e;
        if (++q->cq_head == q->depth) {
            q->cq_head = 0;
            q->phase ^= 1;
        }
        c->completions++;
        if (!complete(c, q, (uint16_t)dw3, (uint16_t)((dw3 >> 17) & 0x7ff), dw0)) {
            drv_log("%s: queue %u: a completion for command %#x, which isn't out", c->name, q->qid,
                    dw3 & 0xffff);
            ok = false;
        }
    }
    if (q->cq_head != start && q->live)
        drv_write32(q->cq_db, 0, q->cq_head);
    return ok;
}

bool queue_reap(struct nvme *c)
{
    bool ok = queue_reap_one(c, &c->admin);
    return queue_reap_one(c, &c->io) && ok;
}

/* ---- timeouts and abort ------------------------------------------------------------------- */

static void abort_done(struct nvme *c, struct nvme_slot *s, uint16_t status, uint32_t dw0)
{
    (void)s;
    if (status == NVME_SC_RESET)
        return;
    drv_log("%s: abort: answered %#x, the command %s", c->name, status,
            status ? "unknown" : (dw0 & 1) ? "was not aborted" : "was aborted");
}

/* Slot s of q is past its deadline: Abort it. false: no admin slot (reset). */
static bool abort_cmd(struct nvme *c, struct nvme_queue *q, struct nvme_slot *s, uint64_t now)
{
    struct nvme_slot *a = slot_get(c, &c->admin);
    if (!a)
        return false;
    uint16_t cid = slot_cid(q, s);
    a->is_abort = true;
    a->deadline = now + ABORT_TIMEOUT;
    a->done = abort_done;
    s->aborted = true;
    s->deadline = now + ABORT_TIMEOUT;
    c->aborts++;
    drv_log("%s: command %#x on queue %u timed out: abort", c->name, cid, q->qid);
    struct admin_args args = { .cdw10 = q->qid | (uint32_t)cid << 16 };
    sqe_admin(c, a, ADMIN_ABORT, &args);
    return true;
}

static bool timeouts_of(struct nvme *c, struct nvme_queue *q, uint64_t now)
{
    for (uint32_t i = 0; i < q->depth; i++) {
        struct nvme_slot *s = &q->slots[i];
        if (!s->busy || s->deadline > now)
            continue;
        if (s->is_abort || s->aborted)
            return false;   /* the abort got no answer, or didn't end it */
        if (!abort_cmd(c, q, s, now))
            return false;
    }
    return true;
}

bool queue_timeouts(struct nvme *c)
{
    uint64_t now = drv_clock_ns();
    return timeouts_of(c, &c->admin, now) && timeouts_of(c, &c->io, now);
}

uint64_t queue_next_deadline(const struct nvme *c)
{
    uint64_t d = DEADLINE_NEVER;
    const struct nvme_queue *qs[2] = { &c->admin, &c->io };
    for (unsigned k = 0; k < 2; k++)
        for (uint32_t i = 0; i < qs[k]->depth; i++)
            if (qs[k]->slots[i].busy && qs[k]->slots[i].deadline < d)
                d = qs[k]->slots[i].deadline;
    return d;
}

void queue_fail_all(struct nvme *c)
{
    for (uint32_t i = 0; i < c->admin.depth; i++) {
        struct nvme_slot *s = &c->admin.slots[i];
        if (!s->busy)
            continue;
        slot_put(&c->admin, s);
        if (s->done)
            s->done(c, s, NVME_SC_RESET, 0);
    }
    for (uint32_t i = 0; i < c->io.depth; i++) {
        struct nvme_slot *s = &c->io.slots[i];
        if (!s->busy)
            continue;
        slot_put(&c->io, s);
        struct nvme_io *io = s->io;
        if (!io || !io->inflight)
            continue;   /* this read was seen to already */
        io->inflight = 0;
        if (!io->retried && io->st == OK) {   /* reads are idempotent: once more */
            io->retried = true;
            io->sent = 0;
            if (io->waiting)
                wait_remove(c, io);
            wait_push(c, io, true);
            continue;
        }
        io->st = io->st == OK ? ERR_IO : io->st;
        if (io->waiting)
            wait_remove(c, io);
        io_finish(c, io);
    }
}
