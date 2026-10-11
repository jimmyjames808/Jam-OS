/* nvme: serving the disk (drv/nvme): its partition table, read once at
 * the start with the library (<jam/parttable.h>), and the `storage`
 * protocol (abi/idl/storage.idl) on DR_SERVE and on each svc.connect's
 * channel (devmgr asks one for volumes, and one for a test that holds its
 * control channel). The `block` channels it opens are block.c's.
 *
 * Writes (docs/M12.7-PLAN.md, "Stage 2"): what the drive is, Jam OS's,
 * blank or someone else's, is cmd.c's to decide (cmd_arm) from the table
 * read here, at the start and after create_table. open_partition with
 * read_only 0 is ERR_ACCESS_DENIED on every drive but a Jam OS one, and
 * create_table on every drive but a blank one, before anything else is
 * looked at but the arguments' form.
 *
 * The table decides what a channel may reach: partition i's bounds are
 * the library's t.parts[i], which lie inside the disk, inside GPT's usable
 * range and overlap no other. Nothing serves the whole disk: `digest`
 * reads any range of it here and answers a SHA-256, never the data.
 *
 * Answered later (the loop never waits for the drive):
 *   digest  read through one 1 MiB buffer, pinned writable and mapped
 *           here, in two halves: one is hashed while the other is read.
 *           One digest at a time, the others queued (DIGEST_QUEUE).
 *   health  the health log, one Get Log Page out at a time
 *           (ctrl_admin_send), and every request waiting for it answered
 *           from it.
 *   create_table  Jam OS's table (the library's pt_jamos_make) into a
 *           64 KiB buffer of its own, pinned writable, then written one
 *           piece at a time, each after the last one's answer, in the
 *           plan's order: the backup entry array, the backup header, the
 *           primary entry array, the primary header, a Flush, the
 *           protective MBR (a cut leaves the drive blank-in-progress: it
 *           runs again). Then the table is read back (from the loop, not
 *           a completion) and the drive's mode set from it.
 * A request kept for later remembers its channel (struct conn); when a
 * connect's channel closes, its requests are forgotten (a digest under way
 * runs to its end, unanswered), so no answer goes to a closed handle.
 *
 * The table's reads wait (read_wait): at the start no client is served
 * yet; after create_table they are a few blocks on the loop's own drive.
 * The wait is bounded by the reads' own timeouts, abort and reset
 * (ctrl_poll) and START_WAIT. */
#include <idl/storage.h>
#include <idl/svc.h>
#include <jam/parttable.h>
#include <jam/sha256.h>
#include "nvme.h"

#define MAX_CONNS   4                       /* connects' channels at once */
#define SERVE_SLOT  MAX_CONNS               /* conns[]'s slot for DR_SERVE */
#define DG_BYTES    (1u << 20)              /* the digest buffer: two halves */
#define DG_HALF     (DG_BYTES / 2)
#define DG_PAGES    (DG_BYTES / NVME_PAGE)
#define PT_BUF      (64u << 10)             /* the table's reads: the buffer's first 64 KiB */
#define HEALTH_WAIT 8                       /* health requests waiting for one log page */
#define START_WAIT  (3 * IO_TIMEOUT)        /* one read of the table, at most */
#define CT_BYTES    (64u << 10)             /* create_table's buffer: the table, its read-back */
#define CT_PAGES    (CT_BYTES / NVME_PAGE)

_Static_assert(PT_SCHEME_NONE == STORAGE_SCHEME_NONE && PT_SCHEME_MBR == STORAGE_SCHEME_MBR &&
               PT_SCHEME_GPT == STORAGE_SCHEME_GPT, "the library's schemes are storage's");
_Static_assert(PT_NAME_MAX == 108, "storage.partition's name is str[<=108]");

/* A channel `storage` is served on: DR_SERVE, or a connect's. */
struct conn {
    handle_t ch;               /* our end; HANDLE_INVALID: a free slot */
    bool     pending;          /* may have requests queued */
    uint16_t gen;              /* bumped at every close: in its port key, and kept requests' */
};

/* A request answered later, and the channel it came on. */
struct kept {
    struct idl_txn txn;
    uint8_t  conn;             /* conns[] index */
    uint16_t gen;              /* that conn's gen then: a closed one's is never answered */
};

struct dg_req {
    struct kept k;
    uint64_t lba, count;
};

static struct nvme_tbl {
    struct pt_table t;         /* static: big (the library's rule) */
    status_t st;               /* pt_read's */
} tbl;

static struct conn conns[MAX_CONNS + 1];
static bool serve_closed;

static struct {
    handle_t vmo;
    uint8_t *buf;              /* mapped */
    uint64_t pages[DG_PAGES];
    uint64_t pin;
    bool     pinned;
    struct dg_req q[STORAGE_DIGEST_QUEUE];   /* q[head] runs */
    uint32_t head, n;
    bool     running;
    bool     orphan;           /* its channel closed: run to the end, unanswered */
    struct sha256 sha;
    uint64_t next, left;       /* the next block to ask for; blocks not asked for */
    uint32_t sent, hashed;     /* chunks asked for, hashed (chunk i is in half i % 2) */
    struct nvme_io io[2];
    bool     out[2], ready[2];
    status_t st;               /* the first failure */
} dg = { .vmo = HANDLE_INVALID };

static struct {
    struct kept w[HEALTH_WAIT];
    uint32_t n;
    bool     asked;            /* a Get Log Page is out */
} hl;

static bool kept_live(const struct kept *k)
{
    return conns[k->conn].ch != HANDLE_INVALID && conns[k->conn].gen == k->gen;
}

/* ---- the table ------------------------------------------------------------------------ */

static struct {
    bool     done;
    bool     stuck;            /* a read got no answer, or the controller failed */
    status_t st;               /* the read's */
    status_t fail;             /* why it is stuck */
} rw;

/* The buffer the table is read through: its pages as the device reaches them. */
struct reader {
    struct nvme    *c;
    const uint64_t *pages;
    uint32_t        npages;
};

static void read_waited(struct nvme *c, struct nvme_io *io, status_t st)
{
    (void)c;
    (void)io;
    rw.done = true;
    rw.st = st;
}

/* `count` blocks from `lba` into the buffer's start, waited for. */
static status_t read_wait(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    const struct reader *rd = ctx;
    struct nvme *c = rd->c;
    (void)buf;   /* the reader's buffer: the device writes it */
    static struct nvme_io io;   /* static: its done may come after a failed wait */
    if (rw.stuck)
        return rw.fail;   /* io may be out still: never reused */
    io = (struct nvme_io){ .lba = lba, .blocks = count, .pages = rd->pages,
                           .npages = rd->npages, .done = read_waited };
    rw.done = false;
    status_t st = nvme_submit(c, &io);
    uint64_t deadline = drv_clock_ns() + START_WAIT;
    while (st == OK && !rw.done) {
        if ((st = ctrl_poll(c)) != OK) {
            rw.stuck = true;
            rw.fail = st;
            break;
        }
        if (rw.done)
            break;
        if (drv_clock_ns() >= deadline) {
            drv_log("%s: the partition table's read at lba %lu: no answer", c->name,
                    (unsigned long)lba);
            rw.stuck = true;
            st = rw.fail = ERR_TIMED_OUT;
            break;
        }
        drv_sleep_until(drv_clock_ns() + 200 * NS_PER_US);
    }
    return st == OK ? rw.st : st;
}

/* A partition's type by name, if it is one the PC's disks have. */
static const char *type_name(const struct pt_part *p)
{
    static const struct { const uint8_t *g; const char *name; } known[] = {
        { pt_type_esp, "EFI system" },
        { pt_type_msr, "Microsoft reserved" },
        { pt_type_basic_data, "Microsoft basic data" },
        { pt_type_recovery, "Windows recovery" },
        { pt_type_jamos_data, "Jam OS data" },
    };
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (pt_guid_eq(p->type_guid, known[i].g))
            return known[i].name;
    return NULL;
}

static void log_table(const struct nvme *c, const struct pt_table *t)
{
    char g[37], why[40];
    if (t->scheme == PT_SCHEME_GPT) {
        pt_guid_str(t->disk_guid, g);
        drv_log("%s: GPT: primary header %s, backup header %s%s; disk GUID %s, usable %lu..%lu, "
                "%u entries of %u bytes, %u used", c->name,
                t->primary ? pt_bad_str(t->primary) : "good",
                t->backup ? pt_bad_str(t->backup) : "good", t->hybrid ? ", a hybrid MBR" : "", g,
                (unsigned long)t->first_usable, (unsigned long)t->last_usable, t->entry_count,
                t->entry_size, t->used);
    }
    for (uint32_t i = 0; i < t->nparts; i++) {
        const struct pt_part *p = &t->parts[i];
        const char *name = type_name(p);
        if (t->scheme == PT_SCHEME_GPT && !name) {
            pt_guid_str(p->type_guid, g);
            name = g;
        } else if (t->scheme != PT_SCHEME_GPT) {
            drv_snprintf(why, sizeof(why), "type %02x", p->mbr_type);
            name = why;
        }
        drv_log("%s: partition %u (entry %u): %s, lba %lu + %lu, \"%s\"", c->name, i + 1,
                p->entry + 1, name, (unsigned long)p->start, (unsigned long)p->blocks, p->name);
    }
    for (uint32_t i = 0; i < t->nnotes; i++)
        drv_log("%s: table entry %u left out: %s", c->name, t->notes[i].entry + 1,
                pt_note_str(t->notes[i].why));
    if (t->notes_total > t->nnotes)
        drv_log("%s: ... and %u more entries left out", c->name, t->notes_total - t->nnotes);
}

void serve_describe(char *out, size_t n)
{
    const struct pt_table *t = &tbl.t;
    if (tbl.st != OK) {
        drv_snprintf(out, n, "no partition table (%s)", status_str(tbl.st));
    } else if (t->scheme == PT_SCHEME_GPT && t->primary && t->backup) {
        drv_snprintf(out, n, "GPT with no readable header (%s, %s)", pt_bad_str(t->primary),
                     pt_bad_str(t->backup));
    } else if (t->scheme == PT_SCHEME_GPT) {
        int o = drv_snprintf(out, n, "GPT, %u partitions", t->nparts);
        if (o >= 0 && (size_t)o < n && (t->primary || t->backup))
            drv_snprintf(out + o, n - (size_t)o, ", %s GPT damaged (%s)",
                         t->primary ? "primary" : "backup",
                         pt_bad_str(t->primary ? t->primary : t->backup));
    } else if (t->scheme == PT_SCHEME_MBR) {
        drv_snprintf(out, n, "MBR, %u partitions", t->nparts);
    } else {
        drv_snprintf(out, n, t->blank ? "blank" : t->nparts ? "no table, served whole"
                                                          : "no partition table");
    }
}

/* The digest buffer: pinned writable (the device writes it), mapped. */
static status_t buffer(struct nvme *c)
{
    void *p = NULL;
    status_t st = drv_vmo_create(DG_BYTES, 0, &dg.vmo);
    if (st == OK && (st = drv_vmo_map(dg.vmo, 0, DG_BYTES, VMAR_READ | VMAR_WRITE, &p)) == OK)
        dg.buf = p;
    if (st == OK && (st = drv_vmo_pin(dg.vmo, c->dma, 0, DG_BYTES, dg.pages, &dg.pin)) == OK)
        dg.pinned = true;
    if (st != OK)
        drv_log("%s: no buffer for the partition table and digests (%s)", c->name,
                status_str(st));
    return st;
}

/* The table read through rd's buffer (buf, `size` bytes, mapped), logged,
 * and the drive's mode set from it (cmd_arm: from nothing, MODE_OTHER,
 * if it can't be read). OK, or the read's failure that ends the driver. */
static status_t table_read(struct nvme *c, struct reader *rd, void *buf, uint32_t size)
{
    struct pt_disk d = { .block_size = c->ns.block_size, .blocks = c->ns.blocks,
                         .read = read_wait, .ctx = rd, .buf = buf, .buf_size = size };
    tbl.st = pt_read(&d, &tbl.t);
    if (c->gone || rw.stuck) {
        cmd_arm(c, NULL);
        return c->gone ? ERR_PEER_CLOSED : rw.fail;   /* the buffer may still be the device's */
    }
    if (tbl.st != OK) {
        /* Served with no partitions: nothing on it can be opened. */
        drv_log("%s: the partition table can't be read (%s): no partitions served", c->name,
                status_str(tbl.st));
        tbl.t.nparts = 0;
        tbl.t.scheme = PT_SCHEME_NONE;
    } else {
        log_table(c, &tbl.t);
    }
    cmd_arm(c, tbl.st == OK ? &tbl.t : NULL);
    drv_log("%s: the drive is %s%s", c->name, cmd_mode_str(), c->ro ? " (the ro word)" : "");
    return OK;
}

status_t serve_start(struct nvme *c)
{
    for (unsigned i = 0; i <= MAX_CONNS; i++)
        conns[i].ch = HANDLE_INVALID;
    cmd_arm(c, NULL);   /* nothing writable until the table says otherwise */
    status_t st = buffer(c);
    if (st != OK)
        return st;
    static struct reader rd;
    rd = (struct reader){ .c = c, .pages = dg.pages, .npages = PT_BUF / NVME_PAGE };
    return table_read(c, &rd, dg.buf, PT_BUF);
}

/* ---- digest ------------------------------------------------------------------------------ */

static void dg_done(struct nvme *c, struct nvme_io *io, status_t st);

/* The next chunk into half k, if any is left and nothing failed. */
static void dg_issue(struct nvme *c, unsigned k)
{
    if (!dg.left || dg.st != OK || c->stopping)
        return;
    uint64_t most = DG_HALF / c->ns.block_size;
    uint32_t n = (uint32_t)(dg.left < most ? dg.left : most);
    dg.io[k] = (struct nvme_io){ .lba = dg.next, .blocks = n,
                                 .pages = dg.pages + k * (DG_PAGES / 2), .npages = DG_PAGES / 2,
                                 .done = dg_done, .ctx = (void *)(uintptr_t)k };
    dg.out[k] = true;
    dg.ready[k] = false;
    status_t st = nvme_submit(c, &dg.io[k]);
    if (st != OK) {
        dg.out[k] = false;
        dg.st = st;
        return;
    }
    dg.next += n;
    dg.left -= n;
    dg.sent++;
}

static void dg_start(struct nvme *c);

/* The digest running is over: answered (unless its channel closed), and
 * the next one started. */
static void dg_finish(struct nvme *c)
{
    uint8_t sum[SHA256_BYTES];
    sha256_done(&dg.sha, sum);
    struct dg_req *r = &dg.q[dg.head];
    if (!dg.orphan && kept_live(&r->k))
        (void)storage_reply_digest(r->k.txn, dg.st, sum);
    dg.head = (dg.head + 1) % STORAGE_DIGEST_QUEUE;
    dg.n--;
    dg.running = dg.orphan = false;
    dg_start(c);
}

/* Hash the chunks that have come, in order; ask for more; end it when
 * nothing is out and nothing is left (or a read failed). */
static void dg_advance(struct nvme *c)
{
    while (dg.st == OK && dg.ready[dg.hashed % 2]) {
        unsigned k = dg.hashed % 2;
        sha256_add(&dg.sha, dg.buf + k * DG_HALF, (size_t)dg.io[k].blocks * c->ns.block_size);
        dg.ready[k] = false;
        dg.hashed++;
        dg_issue(c, k);
    }
    bool idle = !dg.out[0] && !dg.out[1];
    if (idle && (dg.st != OK || (!dg.left && dg.hashed == dg.sent)))
        dg_finish(c);
}

static void dg_done(struct nvme *c, struct nvme_io *io, status_t st)
{
    unsigned k = (unsigned)(uintptr_t)io->ctx;
    dg.out[k] = false;
    if (st != OK && dg.st == OK)
        dg.st = st;
    dg.ready[k] = st == OK;
    dg_advance(c);
}

static void dg_start(struct nvme *c)
{
    if (dg.running || !dg.n || c->stopping)
        return;
    const struct dg_req *r = &dg.q[dg.head];
    dg.running = true;
    dg.orphan = false;
    sha256_init(&dg.sha);
    dg.next = r->lba;
    dg.left = r->count;
    dg.sent = dg.hashed = 0;
    dg.st = OK;
    dg.out[0] = dg.out[1] = dg.ready[0] = dg.ready[1] = false;
    dg_issue(c, 0);
    dg_issue(c, 1);
    dg_advance(c);   /* a read refused at once ends it here */
}

/* ---- health ------------------------------------------------------------------------------- */

static void health_done(struct nvme *c, struct nvme_slot *s, uint16_t status, uint32_t dw0)
{
    (void)s;
    (void)dw0;
    struct nvme_health h;
    hl.asked = false;
    if (status == NVME_SC_SUCCESS)
        health_parse(c->data, &h);
    else
        drv_log("%s: health log: status %#x", c->name, status);
    for (uint32_t i = 0; i < hl.n; i++) {
        if (!kept_live(&hl.w[i]))
            continue;
        if (status != NVME_SC_SUCCESS)
            (void)storage_reply_health(hl.w[i].txn, ERR_IO, 0, 0, 0, 0, 0, 0, 0, 0);
        else
            (void)storage_reply_health(hl.w[i].txn, OK, h.units_read, h.units_written,
                                       h.host_reads, h.host_writes, h.power_cycles,
                                       h.unsafe_shutdowns, h.percent_used, h.critical);
    }
    hl.n = 0;
}

/* ---- create_table ------------------------------------------------------------------------ */

/* Its writes, in the plan's order (see the header). */
enum ct_step {
    CT_BACKUP_ARRAY,
    CT_BACKUP_HEADER,
    CT_PRIMARY_ARRAY,
    CT_PRIMARY_HEADER,
    CT_FLUSH,
    CT_MBR,
    CT_STEPS
};

static struct {
    handle_t vmo;
    uint8_t *buf;              /* mapped: pt_jamos_make's blocks, then the read-back */
    uint64_t pages[CT_PAGES];
    uint64_t pin;
    bool     pinned;
    bool     running;          /* one at a time */
    bool     reread;           /* its writes are over: the loop reads the table back */
    struct kept k;
    struct pt_jamos_plan plan;
    uint32_t step;             /* enum ct_step: the write out */
    status_t st;               /* the first failure */
    struct nvme_io io;
} ct = { .vmo = HANDLE_INVALID };

/* Its buffer, made once: pinned writable (the device reads the table
 * from it, and writes the read-back into it), mapped. */
static status_t ct_buffer(struct nvme *c)
{
    void *p = NULL;
    status_t st = drv_vmo_create(CT_BYTES, 0, &ct.vmo);
    if (st == OK && (st = drv_vmo_map(ct.vmo, 0, CT_BYTES, VMAR_READ | VMAR_WRITE, &p)) == OK)
        ct.buf = p;
    if (st == OK && (st = drv_vmo_pin(ct.vmo, c->dma, 0, CT_BYTES, ct.pages, &ct.pin)) == OK)
        ct.pinned = true;
    if (st != OK)
        drv_log("%s: create_table: no buffer (%s)", c->name, status_str(st));
    return st;
}

static void ct_done(struct nvme *c, struct nvme_io *io, status_t st);

/* The write (or flush) of step ct.step: its blocks on the disk, and
 * where they are in the buffer (pt_jamos_make's layout, in blocks). */
static void ct_issue(struct nvme *c)
{
    const struct pt_jamos_plan *p = &ct.plan;
    if (ct.step == CT_FLUSH && !c->vwc)
        ct.step = CT_MBR;   /* no volatile cache: nothing to flush */
    uint64_t lba = 0;
    uint32_t from = 0, n = 1;
    switch (ct.step) {
    case CT_BACKUP_ARRAY:
        lba = p->tail_start;
        from = PT_JAMOS_AT_ARRAY;
        n = p->array_blocks;
        break;
    case CT_BACKUP_HEADER:
        lba = c->ns.blocks - 1;
        from = pt_jamos_at_backup(p);
        break;
    case CT_PRIMARY_ARRAY:
        lba = from = PT_JAMOS_AT_ARRAY;
        n = p->array_blocks;
        break;
    case CT_PRIMARY_HEADER:
        lba = from = PT_JAMOS_AT_PRIMARY;
        break;
    case CT_MBR:
        lba = from = PT_JAMOS_AT_MBR;
        break;
    default:   /* CT_FLUSH */
        n = 0;
        break;
    }
    ct.io = (struct nvme_io){ .kind = n ? IO_WRITE : IO_FLUSH, .part = PART_TABLE, .lba = lba,
                              .blocks = n, .done = ct_done };
    if (n) {
        ct.io.pages = ct.pages;
        ct.io.npages = CT_PAGES;
        ct.io.offset = from * c->ns.block_size;
    }
    status_t st = c->stopping ? ERR_PEER_CLOSED : nvme_submit(c, &ct.io);
    if (st != OK) {
        ct.st = st;
        ct.reread = true;
    }
}

/* A write's answer (from a completion: nothing waits here). */
static void ct_done(struct nvme *c, struct nvme_io *io, status_t st)
{
    (void)io;
    if (st != OK) {
        drv_log("%s: create_table: step %u failed (%s): nothing more written", c->name, ct.step,
                status_str(st));
        ct.st = st;
        ct.reread = true;
        return;
    }
    if (++ct.step == CT_STEPS) {
        ct.reread = true;
        return;
    }
    ct_issue(c);
}

/* From the loop: the writes are over (or one failed). The table read back,
 * the mode set from it, and the answer: OK once the drive reads as Jam
 * OS's. */
static void ct_finish(struct nvme *c)
{
    ct.reread = false;
    ct.running = false;
    status_t st = ct.st;
    if (!c->stopping) {
        static struct reader rd;
        rd = (struct reader){ .c = c, .pages = ct.pages, .npages = CT_PAGES };
        status_t rst = table_read(c, &rd, ct.buf, CT_BYTES);
        if (rst != OK)
            c->failed = rst;   /* the loop ends: the next start reads it again */
        if (st == OK && cmd_mode() != MODE_JAMOS)
            st = ERR_IO;
    }
    char table[80];
    serve_describe(table, sizeof(table));
    if (st == OK)
        drv_report("%s: create_table: Jam OS's table written and read back: %s, %s", c->name,
                   table, cmd_mode_str());
    else
        drv_report("%s: create_table: FAILED (%s): %s, %s", c->name, status_str(st), table,
                   cmd_mode_str());
    if (kept_live(&ct.k))
        (void)storage_reply_create_table(ct.k.txn, st);
}

/* ---- the storage protocol ------------------------------------------------------------------ */

static struct nvme *nv;   /* the controller (serve_watch's) */

/* s (n bytes, NUL-terminated) into out as storage wants it: ASCII,
 * anything else '?'. Returns its length. */
static uint32_t ascii(char *out, const char *s, uint32_t n)
{
    uint32_t i = 0;
    for (; i < n && s[i]; i++)
        out[i] = s[i] >= 0x20 && s[i] < 0x7f ? s[i] : '?';
    while (i > 0 && out[i - 1] == ' ')
        i--;
    return i;
}

static status_t s_info(void *ctx, char *vendor, uint32_t *vendor_len, char *product,
                       uint32_t *product_len, uint32_t *block_size, uint64_t *blocks,
                       uint8_t *scheme, uint32_t *partitions, uint32_t *mbr_id,
                       uint8_t disk_guid[16])
{
    (void)ctx;
    *vendor_len = ascii(vendor, "NVMe", 8);
    *product_len = ascii(product, nv->model, 16);   /* the whole model: ident */
    *block_size = nv->ns.block_size;
    *blocks = nv->ns.blocks;
    *scheme = tbl.t.scheme;
    *partitions = tbl.t.nparts;
    *mbr_id = tbl.t.mbr_id;
    __builtin_memcpy(disk_guid, tbl.t.disk_guid, 16);
    return OK;
}

static status_t s_partition(void *ctx, uint32_t index, uint8_t *mbr_type, uint8_t type_guid[16],
                            uint8_t unique_guid[16], uint64_t *start, uint64_t *blocks,
                            uint64_t *attributes, char *name, uint32_t *name_len)
{
    (void)ctx;
    if (index >= tbl.t.nparts)
        return ERR_OUT_OF_RANGE;
    const struct pt_part *p = &tbl.t.parts[index];
    *mbr_type = p->mbr_type;
    __builtin_memcpy(type_guid, p->type_guid, 16);
    __builtin_memcpy(unique_guid, p->unique_guid, 16);
    *start = p->start;
    *blocks = p->blocks;
    *attributes = p->attributes;
    uint32_t n = 0;
    while (n < PT_NAME_MAX && p->name[n])
        n++;
    __builtin_memcpy(name, p->name, n);   /* UTF-8 already, as the library made it */
    *name_len = n;
    return OK;
}

static status_t s_open_partition(void *ctx, uint32_t index, uint8_t read_only, handle_t *block)
{
    (void)ctx;
    if (read_only > 1)
        return ERR_INVALID_ARGS;
    if (!read_only && cmd_mode() != MODE_JAMOS)
        return ERR_ACCESS_DENIED;   /* not a Jam OS drive: whatever the index */
    if (index >= tbl.t.nparts)
        return ERR_OUT_OF_RANGE;
    const struct pt_part *p = &tbl.t.parts[index];
    return blk_open(nv, (uint8_t)index, p->start, p->blocks, !read_only, block);
}

static status_t s_ident(void *ctx, char *model, uint32_t *model_len, char *serial,
                        uint32_t *serial_len, char *revision, uint32_t *revision_len)
{
    (void)ctx;
    *model_len = ascii(model, nv->model, 40);
    *serial_len = ascii(serial, nv->serial, 20);
    *revision_len = ascii(revision, nv->firmware, 8);   /* the firmware revision */
    return OK;
}

static struct kept keep(const struct conn *k, struct idl_txn txn)
{
    return (struct kept){ .txn = txn, .conn = (uint8_t)(k - conns), .gen = k->gen };
}

static status_t s_digest(void *ctx, struct idl_txn txn, uint64_t lba, uint64_t count,
                         uint8_t sha256[32])
{
    (void)sha256;
    uint64_t blocks = nv->ns.blocks;
    if (!count)
        return ERR_INVALID_ARGS;
    if (lba >= blocks || count > blocks - lba ||
        count > STORAGE_DIGEST_MAX / nv->ns.block_size)
        return ERR_OUT_OF_RANGE;
    if (dg.n == STORAGE_DIGEST_QUEUE)
        return ERR_NO_RESOURCES;
    if (!dg.buf)
        return ERR_BAD_STATE;
    dg.q[(dg.head + dg.n++) % STORAGE_DIGEST_QUEUE] =
        (struct dg_req){ .k = keep(ctx, txn), .lba = lba, .count = count };
    dg_start(nv);
    return IDL_LATER;
}

static status_t s_health(void *ctx, struct idl_txn txn, uint64_t *units_read,
                         uint64_t *units_written, uint64_t *host_reads, uint64_t *host_writes,
                         uint64_t *power_cycles, uint64_t *unsafe_shutdowns,
                         uint8_t *percent_used, uint8_t *critical)
{
    (void)units_read, (void)units_written, (void)host_reads, (void)host_writes;
    (void)power_cycles, (void)unsafe_shutdowns, (void)percent_used, (void)critical;
    if (hl.n == HEALTH_WAIT)
        return ERR_NO_RESOURCES;
    if (!hl.asked) {
        struct admin_args a;
        health_args(nv, &a);
        status_t st = ctrl_admin_send(nv, ADMIN_GET_LOG_PAGE, &a, health_done, NULL);
        if (st != OK)
            return st;
        hl.asked = true;
    }
    hl.w[hl.n++] = keep(ctx, txn);
    return IDL_LATER;
}

static status_t s_create_table(void *ctx, struct idl_txn txn, const uint8_t disk_guid[16],
                               const uint8_t esp_guid[16], const uint8_t data_guid[16])
{
    if (cmd_mode() != MODE_BLANK)
        return ERR_ACCESS_DENIED;   /* not a blank drive: before anything else */
    if (ct.running)
        return ERR_BAD_STATE;
    uint32_t bs = nv->ns.block_size;
    status_t st = pt_jamos_plan(bs, nv->ns.blocks, &ct.plan);
    if (st == OK && !ct.buf)
        st = ct_buffer(nv);
    if (st == OK)
        st = pt_jamos_make(bs, nv->ns.blocks, disk_guid, esp_guid, data_guid, ct.buf, CT_BYTES);
    if (st != OK)
        return st;
    drv_log("%s: create_table: Jam OS's table onto the blank drive: ESP lba %lu + %lu, data lba "
            "%lu + %lu", nv->name, (unsigned long)ct.plan.esp_start,
            (unsigned long)ct.plan.esp_blocks, (unsigned long)ct.plan.data_start,
            (unsigned long)ct.plan.data_blocks);
    ct.running = true;
    ct.k = keep(ctx, txn);
    ct.step = CT_BACKUP_ARRAY;
    ct.st = OK;
    ct_issue(nv);
    return IDL_LATER;
}

static const struct storage_ops storage_ops = {
    .info = s_info,
    .partition = s_partition,
    .open_partition = s_open_partition,
    .ident = s_ident,
    .digest = s_digest,
    .health = s_health,
    .create_table = s_create_table,
};

/* ---- svc.connect: a `storage` channel of the caller's own ------------------------------ */

static uint64_t conn_key(const struct conn *k)
{
    return KEY_CONN | (uint64_t)k->gen << 8 | (uint64_t)(k - conns);
}

/* k closes: what it kept for later is forgotten (see the header). */
static void conn_close(struct conn *k)
{
    if (k->ch == HANDLE_INVALID)
        return;
    (void)drv_port_unbind(nv->port, k->ch, conn_key(k));
    drv_handle_close(k->ch);
    k->ch = HANDLE_INVALID;
    k->pending = false;
    k->gen++;
    if (dg.running && !kept_live(&dg.q[dg.head].k))
        dg.orphan = true;
    /* The queued ones (not the running one) of closed channels go. */
    uint32_t n = dg.running ? 1 : 0;
    for (uint32_t i = n; i < dg.n; i++) {
        struct dg_req *r = &dg.q[(dg.head + i) % STORAGE_DIGEST_QUEUE];
        if (kept_live(&r->k))
            dg.q[(dg.head + n++) % STORAGE_DIGEST_QUEUE] = *r;
    }
    dg.n = n;
}

static status_t s_connect(void *ctx, handle_t *out)
{
    (void)ctx;
    for (unsigned i = 0; i < MAX_CONNS; i++) {
        struct conn *k = &conns[i];
        if (k->ch != HANDLE_INVALID)
            continue;
        handle_t theirs;
        status_t st = drv_channel_create(&k->ch, &theirs);
        if (st != OK) {
            k->ch = HANDLE_INVALID;
            return st;
        }
        st = drv_port_bind(nv->port, k->ch, conn_key(k), SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
        if (st != OK) {
            drv_handle_close(k->ch);
            drv_handle_close(theirs);
            k->ch = HANDLE_INVALID;
            return st;
        }
        k->pending = true;   /* look once: the port says only what comes later */
        *out = theirs;
        return OK;
    }
    return ERR_NO_RESOURCES;
}

static const struct svc_ops svc_ops = { .connect = s_connect };
static const struct svc_ops no_svc_ops = { 0 };   /* a connect's channel makes no more */

/* Take one message off k's channel and answer it (or keep it for later),
 * with svc.connect next to `storage` on DR_SERVE alone. OK once a message
 * was handled; else drv_channel_read's status (ERR_SHOULD_WAIT: nothing
 * queued; ERR_PEER_CLOSED: the client is gone for good). */
static status_t serve_one(struct conn *k)
{
    _Static_assert(SVC_REQ_MAX <= STORAGE_REQ_MAX && SVC_REP_MAX <= STORAGE_REP_MAX,
                   "a connect fits where a storage request does");
    _Alignas(8) uint8_t q[STORAGE_REQ_MAX], r[STORAGE_REP_MAX];
    handle_t hs[IDL_READ_HANDLES], rhs[IDL_REP_HANDLES];
    uint32_t n = 0, nh = 0, rhn = 0, rn;
    status_t st = drv_channel_read(k->ch, q, sizeof(q), &n, hs, IDL_READ_HANDLES, &nh);
    if (st == ERR_BUFFER_TOO_SMALL)
        return idl_drain(k->ch, n, nh);
    if (st != OK)
        return st;
    if (nh) {
        idl_close_all(hs, nh);
        idl_reply_status(k->ch, q, n, ERR_INVALID_ARGS);
        return OK;
    }
    const struct idl_req_hdr *h = (const struct idl_req_hdr *)q;
    bool dr_serve = k == &conns[SERVE_SLOT];
    if (n >= sizeof(*h) && h->ordinal >> 16 == SVC_PROTOCOL_ID)
        rn = svc_dispatch(dr_serve ? &svc_ops : &no_svc_ops, k, q, n, r, rhs, &rhn);
    else
        rn = storage_dispatch_on(k->ch, &storage_ops, k, q, n, r, rhs, &rhn);
    if (rn)
        (void)drv_channel_write(k->ch, r, rn, rhs, rhn);   /* its handles go either way */
    else
        idl_close_all(rhs, rhn);
    return OK;
}

/* ---- the loop's side ------------------------------------------------------------------------ */

status_t serve_watch(struct nvme *c)
{
    nv = c;
    struct conn *k = &conns[SERVE_SLOT];
    k->ch = c->serve;
    k->pending = true;   /* look once: requests may be there already */
    return drv_port_bind(c->port, c->serve, KEY_SERVE, SIG_READABLE | SIG_PEER_CLOSED,
                         PORT_BIND_PERSISTENT);
}

void serve_packet(uint64_t key)
{
    if (key == KEY_SERVE) {
        conns[SERVE_SLOT].pending = true;
    } else if ((key & ~0xffffffull) == KEY_BLK) {
        blk_packet(key);
    } else if ((key & ~0xffffffull) == KEY_CONN && (key & 0xff) < MAX_CONNS &&
               conn_key(&conns[key & 0xff]) == key) {
        conns[key & 0xff].pending = true;
    }
}

bool serve_ready(void)
{
    if (ct.reread)
        return true;
    for (unsigned i = 0; i <= MAX_CONNS; i++)
        if (conns[i].ch != HANDLE_INVALID && conns[i].pending)
            return true;
    return blk_ready();
}

bool serve_work(struct nvme *c)
{
    if (ct.reread)
        ct_finish(c);
    for (unsigned i = 0; i <= MAX_CONNS && !serve_closed; i++) {
        struct conn *k = &conns[i];
        if (k->ch == HANDLE_INVALID || !k->pending)
            continue;
        k->pending = false;
        status_t st = OK;
        for (int guard = 0; guard < 16 && st == OK; guard++)
            st = serve_one(k);
        if (st == OK)
            k->pending = true;   /* maybe more: the port won't say so again */
        else if (st != ERR_SHOULD_WAIT && i == SERVE_SLOT)
            serve_closed = true;   /* devmgr is stopping us */
        else if (st != ERR_SHOULD_WAIT)
            conn_close(k);         /* that client's volumes ended */
    }
    blk_serve(c);
    return !serve_closed;
}

void serve_stop(struct nvme *c)
{
    nv = c;
    for (unsigned i = 0; i < MAX_CONNS; i++)
        conn_close(&conns[i]);
    conns[SERVE_SLOT].ch = HANDLE_INVALID;   /* DR_SERVE: the process's end closes it */
    blk_close_all(c);
    if (dg.pinned && drv_vmo_unpin(dg.vmo, c->dma, dg.pin) == OK)
        dg.pinned = false;
    if (dg.buf)
        (void)drv_vmo_unmap(dg.buf, DG_BYTES);
    if (dg.vmo != HANDLE_INVALID)
        drv_handle_close(dg.vmo);
    dg.buf = NULL;
    dg.vmo = HANDLE_INVALID;
    if (ct.pinned && drv_vmo_unpin(ct.vmo, c->dma, ct.pin) == OK)
        ct.pinned = false;
    if (ct.buf)
        (void)drv_vmo_unmap(ct.buf, CT_BYTES);
    if (ct.vmo != HANDLE_INVALID)
        drv_handle_close(ct.vmo);
    ct.buf = NULL;
    ct.vmo = HANDLE_INVALID;
}
