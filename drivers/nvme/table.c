/* nvme: the drive's partition table (drv/nvme): read with the library
 * (<jam/parttable.h>) when the driver starts, made by create_table on a
 * blank drive, and read back after it. Every reading sets the drive's
 * mode (cmd.c's cmd_arm: Jam OS's, blank or other) from what it found;
 * nothing else does. serve.c serves `storage` from the table (table_get).
 *
 * The reads wait (read_wait): at the start no client is served yet;
 * after create_table they are a few blocks on the loop's own drive. The
 * wait is bounded by the reads' own timeouts, abort and reset (ctrl_poll)
 * and START_WAIT.
 *
 * create_table: Jam OS's table (the library's pt_jamos_make) into a
 * 64 KiB buffer of its own, pinned writable, then written one piece at a
 * time, each after the last one's answer, in the plan's order: the backup
 * entry array, the backup header, the primary entry array, the primary
 * header, a Flush, the protective MBR (a cut leaves the drive
 * blank-in-progress: it runs again). Every write targets PART_TABLE, so
 * cmd.c builds it only while the drive is MODE_BLANK, and only inside the
 * table's blocks. Then the table is read back, from the loop (table_work),
 * not from a completion, and serve.c answers (serve_created). */
#include <jam/parttable.h>
#include "nvme.h"

#define START_WAIT  (3 * IO_TIMEOUT)        /* one read of the table, at most */
#define CT_BYTES    (64u << 10)             /* create_table's buffer: the table, its read-back */
#define CT_PAGES    (CT_BYTES / NVME_PAGE)

static struct nvme_tbl {
    struct pt_table t;         /* static: big (the library's rule) */
    status_t st;               /* pt_read's */
} tbl;

/* ---- reading it ------------------------------------------------------------------------ */

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

void table_describe(char *out, size_t n)
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
    table_describe(table, sizeof(table));
    if (st == OK)
        drv_report("%s: create_table: Jam OS's table written and read back: %s, %s", c->name,
                   table, cmd_mode_str());
    else
        drv_report("%s: create_table: FAILED (%s): %s, %s", c->name, status_str(st), table,
                   cmd_mode_str());
    serve_created(st);
}


/* ---- the rest of the driver's side ------------------------------------------------------ */

status_t table_start(struct nvme *c, void *buf, const uint64_t *pages, uint32_t size)
{
    static struct reader rd;
    rd = (struct reader){ .c = c, .pages = pages, .npages = size / NVME_PAGE };
    return table_read(c, &rd, buf, size);
}

const struct pt_table *table_get(void)
{
    return &tbl.t;
}

status_t table_create(struct nvme *c, const uint8_t *disk_guid, const uint8_t *esp_guid,
                      const uint8_t *data_guid)
{
    if (cmd_mode() != MODE_BLANK)
        return ERR_ACCESS_DENIED;
    if (ct.running)
        return ERR_BAD_STATE;
    uint32_t bs = c->ns.block_size;
    status_t st = pt_jamos_plan(bs, c->ns.blocks, &ct.plan);
    if (st == OK && !ct.buf)
        st = ct_buffer(c);
    if (st == OK)
        st = pt_jamos_make(bs, c->ns.blocks, disk_guid, esp_guid, data_guid, ct.buf, CT_BYTES);
    if (st != OK)
        return st;
    drv_log("%s: create_table: Jam OS's table onto the blank drive: ESP lba %lu + %lu, data lba "
            "%lu + %lu", c->name, (unsigned long)ct.plan.esp_start,
            (unsigned long)ct.plan.esp_blocks, (unsigned long)ct.plan.data_start,
            (unsigned long)ct.plan.data_blocks);
    ct.running = true;
    ct.step = CT_BACKUP_ARRAY;
    ct.st = OK;
    ct_issue(c);
    return OK;
}

bool table_ready(void)
{
    return ct.reread;
}

void table_work(struct nvme *c)
{
    if (ct.reread)
        ct_finish(c);
}

void table_stop(struct nvme *c)
{
    if (ct.pinned && drv_vmo_unpin(ct.vmo, c->dma, ct.pin) == OK)
        ct.pinned = false;
    if (ct.buf)
        (void)drv_vmo_unmap(ct.buf, CT_BYTES);
    if (ct.vmo != HANDLE_INVALID)
        drv_handle_close(ct.vmo);
    ct.buf = NULL;
    ct.vmo = HANDLE_INVALID;
}
