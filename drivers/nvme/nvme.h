/* nvme: what the driver's files share (drv/nvme). main.c: start, the
 * loop and ending; ctrl.c: the controller's registers, enable, disable,
 * identify, the queues' creation, reset and shutdown; queue.c: completion
 * queues, command slots and ids, timeouts and abort, PRPs and requests;
 * cmd.c: submission queues, the ONE place a submission entry is written,
 * and the write gate; table.c: the partition table, read and made
 * (create_table); serve.c: the `storage` protocol (digest, health);
 * block.c: the `block` channels; selftest.c: the test words' reads.
 *
 * One thread; nothing is locked. Every command is a slot of its queue
 * (struct nvme_slot): its id is the slot's index and the slot's
 * generation, so a completion that arrives for a slot reused since (after
 * a reset, or an abort) is recognised and ignored.
 *
 * The write rule (docs/M12.7-PLAN.md, "The read-only guarantee" and
 * "Stage 2: writes to a Jam OS disk only"): cmd.c alone knows the
 * submission queues' memory (struct nvme_sq is opaque here), the opcodes
 * and the drive's mode (enum nvme_mode), which it sets itself from the
 * partition table the driver read (cmd_arm). It builds only the commands
 * its lists name, checks each one again at run time, and builds a Write
 * in one function, sqe_write, which refuses anything but a Jam OS drive's
 * own partitions (or a blank drive's table, for create_table) before it
 * looks at the blocks. tools/checknvme.py keeps it that way at build
 * time. Windows' drive, like every drive that isn't Jam OS's, is never
 * written: its mode is MODE_OTHER and no Write or Flush can be built. The
 * one way out of MODE_OTHER is the owner's: create_table given the end of
 * the drive's serial number and "erase" (table.c checks them) arms the
 * gate MODE_ERASE for that create_table alone, its table's blocks and the
 * spots where its partitions will start. */
#pragma once

#include <jam/driver.h>

/* ---- the controller's registers (NVMe 1.4, section 3.1) ---------------------- */

#define NVME_CAP   0x00   /* capabilities (64 bits) */
#define NVME_VS    0x08   /* version */
#define NVME_CC    0x14   /* controller configuration */
#define NVME_CSTS  0x1c   /* controller status */
#define NVME_AQA   0x24   /* admin queue attributes */
#define NVME_ASQ   0x28   /* admin submission queue base (64 bits) */
#define NVME_ACQ   0x30   /* admin completion queue base (64 bits) */
#define NVME_REGS_BYTES 0x1000u
#define NVME_DOORBELLS  0x1000u   /* the first doorbell's offset in BAR 0 */

#define CAP_MQES(c)   ((uint32_t)((c) & 0xffff))          /* largest queue, 0's based */
#define CAP_CQR       (1ull << 16)                        /* queues physically contiguous */
#define CAP_TO(c)     ((uint32_t)(((c) >> 24) & 0xff))    /* ready timeout, 500 ms units */
#define CAP_DSTRD(c)  ((uint32_t)(((c) >> 32) & 0xf))     /* doorbell stride: 4 << DSTRD */
#define CAP_CSS_NVM   (1ull << 37)                        /* the NVM command set */
#define CAP_MPSMIN(c) ((uint32_t)(((c) >> 48) & 0xf))     /* smallest page: 4 KiB << MPSMIN */

#define CC_EN          (1u << 0)
#define CC_CSS_NVM     (0u << 4)
#define CC_MPS_4K      (0u << 7)
#define CC_AMS_RR      (0u << 11)
#define CC_SHN_MASK    (3u << 14)
#define CC_SHN_NORMAL  (1u << 14)
#define CC_IOSQES      (6u << 16)   /* 64-byte submission entries */
#define CC_IOCQES      (4u << 20)   /* 16-byte completion entries */

#define CSTS_RDY         (1u << 0)
#define CSTS_CFS         (1u << 1)   /* controller fatal status */
#define CSTS_SHST_MASK   (3u << 2)
#define CSTS_SHST_DONE   (2u << 2)
#define CSTS_GONE        0xffffffffu   /* the function stopped answering */

/* ---- sizes and limits ---------------------------------------------------------- */

#define NVME_PAGE      4096u
#define NVME_QDEPTH    64u    /* entries in each queue (fewer if CAP.MQES says so) */
#define NVME_SQE_BYTES 64u
#define NVME_CQE_BYTES 16u
#define NVME_IO_QID    1u     /* the one I/O queue pair */
#define NVME_CMD_MAX   (1u << 20)   /* bytes one read or write command moves at most (and MDTS) */
#define NVME_PRP_PER_PAGE (NVME_PAGE / 8)

#define ADMIN_TIMEOUT  (10 * NS_PER_S)
#define IO_TIMEOUT     (30 * NS_PER_S)
#define ABORT_TIMEOUT  (5 * NS_PER_S)
#define READY_MAX      (60 * NS_PER_S)   /* CAP.TO's largest: 255 x 500 ms is more */
#define SHUTDOWN_WAIT  (10 * NS_PER_S)
#define DRAIN_WAIT     (1 * NS_PER_S)    /* commands in flight at the end */
#define RESET_WINDOW   (60 * NS_PER_S)
#define RESETS_MAX     3                 /* resets inside RESET_WINDOW: give up */

/* Completion status (NVMe 1.4, 4.6.1.2), as kept: SC | SCT << 8. */
#define NVME_SC_SUCCESS   0x000
#define NVME_SC_ABORTED   0x007   /* Command Abort Requested */
#define NVME_SC_RESET     0x7fff  /* not the device's: the slot was failed by a reset */

/* ---- the controller's parts ----------------------------------------------------- */

struct nvme;
struct nvme_io;
struct nvme_sq;   /* cmd.c's alone */
struct pt_table;

/* What the driver may write on its drive (cmd.c's gate decides; cmd_mode
 * tells). Only ever from the partition table the driver read itself,
 * when it starts and after its own create_table. */
enum nvme_mode {
    MODE_OTHER = 0,   /* someone else's (Windows'), unreadable, or `ro`: never written */
    MODE_BLANK,       /* blank: create_table may write its table's blocks, nothing else */
    MODE_JAMOS,       /* Jam OS's: its two partitions may be written, nothing else */
    MODE_ERASE,       /* someone else's, its erase confirmed by the owner: create_table may
                       * write its table's blocks and zero the blank rule's spots, nothing
                       * else, until it ends (then the table read back decides again) */
};

/* A write's target that is no partition: the table's blocks, for
 * create_table on a blank drive (an nvme_io's `part`), and on an erased
 * one also the blank rule's spots. */
#define PART_TABLE 0xffu

/* One command in flight on a queue. */
struct nvme_slot {
    bool     busy;           /* a command with this slot's id is out */
    bool     aborted;        /* an Abort naming it was sent */
    bool     is_abort;       /* it IS an Abort */
    uint16_t gen;            /* bumped at every use: the id's upper bits */
    uint64_t deadline;       /* when it is aborted (or the controller reset) */
    struct nvme_io *io;      /* the request it is part of (I/O queue), else NULL */
    /* An admin command's answer: status (NVME_SC_*) and completion dword 0. */
    void   (*done)(struct nvme *c, struct nvme_slot *s, uint16_t status, uint32_t dw0);
    void    *ctx;            /* done's */
};

/* A queue pair: the submission side is cmd.c's, the rest queue.c's. */
struct nvme_queue {
    uint16_t qid;              /* 0: admin, NVME_IO_QID */
    uint16_t depth;            /* entries in each of its queues */
    bool     live;             /* the controller has it (admin: enabled) */
    struct nvme_sq *sq;        /* cmd.c's */
    handle_t cq_vmo;           /* the completion queue's page */
    volatile uint8_t *cq;      /* mapped */
    uint64_t cq_addr;          /* its device address */
    uint64_t cq_pin;
    bool     cq_pinned;
    uint16_t cq_head;          /* the next entry to look at */
    uint8_t  phase;            /* the phase bit a new entry has */
    volatile void *cq_db;      /* its head doorbell */
    uint32_t busy;             /* slots in use */
    struct nvme_slot slots[NVME_QDEPTH];
};

/* The namespace served (one per controller). */
struct nvme_ns {
    uint32_t nsid;             /* 0: none */
    uint32_t block_size;       /* 512 or 4096 */
    uint64_t blocks;           /* NSZE */
};

struct nvme {
    const char *name;          /* "nvme 02:00.0", for the log */
    handle_t dev, dma, irq, port, serve;
    volatile void *regs;       /* BAR 0's first page */
    volatile void *dbs;        /* its doorbells for queues 0 and 1 */
    uint32_t dstrd;            /* the doorbell stride in bytes */
    uint64_t cap;
    uint32_t vs;
    uint64_t ready_wait;       /* CAP.TO in ns */
    bool     bus_master;
    bool     gone;             /* registers read all ones */
    bool     stopping;         /* DR_SERVE closed: no new requests */
    bool     ro;               /* the `ro` word (devmgr's, for `diskro`): every drive MODE_OTHER */
    bool     vwc;              /* a volatile write cache: a Flush means something */
    status_t failed;           /* not OK: the loop ends with it (the table's read-back stuck) */
    struct nvme_queue admin, io;

    /* Identify (CNS 1): strings trimmed, NUL-terminated. */
    char     model[41], serial[21], firmware[9];
    uint32_t max_cmd;          /* bytes a read or write command may move */
    uint32_t nn;               /* namespaces the controller may have */
    struct nvme_ns ns;

    /* The data page: identify and log pages land here. */
    handle_t data_vmo;
    uint8_t *data;
    uint64_t data_addr, data_pin;
    bool     data_pinned;

    /* PRP lists: page i is the I/O queue's slot i's, read only to the device. */
    handle_t prp_vmo;
    uint64_t *prp;
    uint64_t prp_addr[NVME_QDEPTH];
    uint64_t prp_pin;
    bool     prp_pinned;

    /* Requests waiting for a slot, first to last (an io partly sent stays first). */
    struct nvme_io *wait_head, *wait_tail;

    /* Counts, for the log. */
    uint64_t irqs, polls, completions, stale, reads, read_fail, aborts;
    uint64_t writes, flushes, write_fail;
    uint32_t resets;
    uint64_t reset_at[RESETS_MAX];   /* the last resets' times */

    /* Test words (nvmetest=...): selftest.c's. */
    uint32_t test;             /* NVME_TEST_* */
    uint32_t expire_next;      /* I/O commands whose deadline is made already past */
};

#define NVME_TEST_SELFTEST (1u << 0)   /* read and hash the namespace's ends */
#define NVME_TEST_LOOP     (1u << 1)   /* ... again and again (a kill lands mid-read) */
#define NVME_TEST_RESET    (1u << 2)   /* reset the controller with the reads in flight */
#define NVME_TEST_ABORT    (1u << 3)   /* the first read times out at once: Abort */
/* A block channel's answers come at least BLK_SLOW_STEP apart, as from a
 * slow drive (block.c): a client that leaves then has reads out, which
 * the drain rule waits for (utest's nvme_fence). */
#define NVME_TEST_SLOW     (1u << 4)

/* What an nvme_io asks: IO_READ (0, so a request made with no kind is a
 * read), IO_WRITE (with FUA: answered means on the medium), IO_FLUSH. */
enum io_kind {
    IO_READ = 0,
    IO_WRITE,
    IO_FLUSH,
};

/* A read or write of `blocks` blocks at `lba`, from or into a buffer
 * whose pages the device reaches at pages[0..npages), starting `offset`
 * bytes in (a multiple of 4); or a Flush (no blocks, no buffer). A write
 * names its target, `part`: the table's index of its partition, or
 * PART_TABLE (cmd.c checks the lba against that, and the drive's mode,
 * whatever the caller checked). done is called once, from the loop, with
 * OK or the first failure: ERR_IO (the device failed a command, or a
 * reset failed it: a read twice, a write or flush at once), ERR_TIMED_OUT
 * (aborted after its deadline), ERR_PEER_CLOSED (the driver is
 * stopping). The caller keeps io and the buffer until then. */
struct nvme_io {
    uint8_t  kind;             /* enum io_kind */
    uint8_t  part;             /* a write's target (see above) */
    uint64_t lba;
    uint32_t blocks;
    const uint64_t *pages;
    uint32_t npages;
    uint32_t offset;
    void   (*done)(struct nvme *c, struct nvme_io *io, status_t st);
    void    *ctx;              /* done's */
    /* queue.c's */
    uint32_t sent;             /* blocks given to commands so far */
    uint32_t inflight;         /* its commands out */
    status_t st;               /* the first failure */
    bool     retried;          /* a read resubmitted after a reset once already */
    bool     waiting;          /* on the wait list */
    struct nvme_io *next;      /* on the wait list */
};

static inline uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}
static inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

/* ---- cmd.c: submission queues and the commands -------------------------------------- */

/* The admin commands this driver can send, and no others (cmd.c maps each
 * to its opcode and checks its arguments). */
enum admin_op {
    ADMIN_IDENTIFY,       /* cdw10: CNS 0, 1 or 2 */
    ADMIN_GET_LOG_PAGE,   /* cdw10: log 01h or 02h, the length in dwords */
    ADMIN_SET_FEATURES,   /* cdw10: feature 07h (number of queues) only */
    ADMIN_CREATE_CQ,      /* cdw10: queue NVME_IO_QID */
    ADMIN_CREATE_SQ,      /* cdw10: queue NVME_IO_QID; its address is cmd.c's own */
    ADMIN_DELETE_SQ,
    ADMIN_DELETE_CQ,
    ADMIN_ABORT,          /* cdw10: the queue and the id of the command */
};

struct admin_args {
    uint32_t nsid;
    uint64_t prp1;        /* a data page, or the completion queue (CREATE_CQ) */
    uint32_t cdw10, cdw11;
};

/* The drive's mode from the table the driver read itself (t; NULL: none
 * could be read), and where it may then be written: MODE_JAMOS, its two
 * partitions; MODE_BLANK, the blocks pt_jamos_plan gives Jam OS's table;
 * MODE_OTHER, nothing (also with c->ro). `erase`: the owner confirmed
 * this drive's erase (table.c, create_table, and nowhere else): a drive
 * the table calls someone else's is MODE_ERASE, the table's blocks and
 * the blank rule's spots. The one way the mode changes. */
void     cmd_arm(struct nvme *c, const struct pt_table *t, bool erase);
enum nvme_mode cmd_mode(void);
const char *cmd_mode_str(void);   /* for the log and the RESULTS line */

/* Queue q's submission queue: its page, pinned read only, and its tail
 * doorbell. OK, or what failed (nothing kept). */
status_t sq_create(struct nvme *c, struct nvme_queue *q);
void     sq_destroy(struct nvme *c, struct nvme_queue *q);
/* Empty again (the controller was disabled): tail 0, entries zeroed. */
void     sq_reset(struct nvme_queue *q);
uint64_t sq_addr(const struct nvme_queue *q);   /* for ASQ */
/* Send an admin command in slot s of c->admin, or a read, a write (to
 * target `part`, see struct nvme_io) or a flush in slot s of c->io.
 * Anything outside the lists ends the driver (exit 1, a RESULTS line):
 * they never return an error for it. sqe_write refuses so a write while
 * the mode isn't MODE_JAMOS (a partition) or MODE_BLANK or MODE_ERASE
 * (PART_TABLE), and then one outside its target's blocks; sqe_flush, a flush on a
 * MODE_OTHER drive. The callers check first (block.c, serve.c,
 * table.c): these are the last line. */
void     sqe_admin(struct nvme *c, struct nvme_slot *s, enum admin_op op,
                   const struct admin_args *a);
void     sqe_read(struct nvme *c, struct nvme_slot *s, uint64_t lba, uint32_t blocks,
                  uint64_t prp1, uint64_t prp2);
void     sqe_write(struct nvme *c, struct nvme_slot *s, uint8_t part, uint64_t lba,
                   uint32_t blocks, uint64_t prp1, uint64_t prp2);
void     sqe_flush(struct nvme *c, struct nvme_slot *s);
/* The command id a slot's command carries. */
uint16_t slot_cid(const struct nvme_queue *q, const struct nvme_slot *s);

/* ---- queue.c: completions, slots, timeouts, requests --------------------------------- */

status_t queue_create(struct nvme *c, struct nvme_queue *q, uint16_t qid, uint16_t depth);
void     queue_destroy(struct nvme *c, struct nvme_queue *q);
/* Empty both sides again (the controller was disabled). */
void     queue_reset(struct nvme_queue *q);
/* A free slot of q (deadline set, gen bumped), or NULL. */
struct nvme_slot *slot_get(struct nvme *c, struct nvme_queue *q);
/* Take every completion the controller wrote; false if a queue looks
 * corrupt (a completion for a slot with no command): reset it. */
bool     queue_reap(struct nvme *c);
bool     queue_reap_one(struct nvme *c, struct nvme_queue *q);   /* the same, one queue */
/* Commands past their deadline: Abort, and past the abort's: false
 * (reset). */
bool     queue_timeouts(struct nvme *c);
/* The nearest deadline of any command out (DEADLINE_NEVER: none). */
uint64_t queue_next_deadline(const struct nvme *c);
/* Every command out failed (NVME_SC_RESET): admin commands' done with it,
 * reads put back at the front of the wait list (once; then ERR_IO),
 * writes and flushes failed ERR_IO (a write may or may not have landed:
 * its client hears so, and asks again). */
void     queue_fail_all(struct nvme *c);
/* A request: see struct nvme_io. ERR_INVALID_ARGS / ERR_OUT_OF_RANGE
 * before anything is sent; else OK and io->done later. */
status_t nvme_submit(struct nvme *c, struct nvme_io *io);
/* Take io off the wait list if none of its commands was ever sent (its
 * done is then never called): true. false: it is out, or not waiting. */
bool     io_cancel(struct nvme *c, struct nvme_io *io);
/* Send what the wait list can (slots permitting). */
void     io_kick(struct nvme *c);
/* Every request still waiting or out ends with st (the driver stops). */
void     io_end_all(struct nvme *c, status_t st);

/* ---- ctrl.c: the controller ------------------------------------------------------------ */

/* Registers mapped; the controller disabled (the firmware's queues gone)
 * BEFORE bus mastering goes on; the admin queue; enabled; identify; the
 * namespace; the I/O queue pair. OK, or the exit code's reason. */
status_t ctrl_start(struct nvme *c, handle_t bar);
/* An admin command, waited for (bounded by its deadline, and the
 * controller's state): its status (NVME_SC_*) and dword 0. ERR_TIMED_OUT,
 * ERR_IO (failed), ERR_NO_RESOURCES (no slot), ERR_PEER_CLOSED (gone). */
status_t ctrl_admin_wait(struct nvme *c, enum admin_op op, const struct admin_args *a,
                         uint32_t *dw0);
/* An admin command sent, answered later: done(c, slot, status, dw0) from
 * the loop (status NVME_SC_RESET if a reset forgot it). ERR_NO_RESOURCES:
 * no slot free; ERR_BAD_STATE: the admin queue is down (a reset). */
status_t ctrl_admin_send(struct nvme *c, enum admin_op op, const struct admin_args *a,
                         void (*done)(struct nvme *, struct nvme_slot *, uint16_t, uint32_t),
                         void *ctx);
/* Look at the controller (fatal status, gone) and its queues; act on
 * timeouts. ERR_PEER_CLOSED: gone; ERR_IO: reset failed or too many. */
status_t ctrl_poll(struct nvme *c);
/* Disable, set the queues up again, enable, the I/O pair again; reads in
 * flight resubmitted (writes failed). ERR_IO: RESETS_MAX in RESET_WINDOW,
 * or it failed. */
status_t ctrl_reset(struct nvme *c, const char *why);
/* The end: requests failed, the I/O queues deleted, the normal shutdown
 * notification waited for, bus mastering off, everything unpinned. */
void     ctrl_stop(struct nvme *c);
/* The SMART / health log (02h; NVMe 1.4, Figure 194): what `disk smart`
 * shows, and the witness that nothing was written ("host write commands"
 * doesn't move). 128-bit counters are kept to their low 64 bits. */
struct nvme_health {
    uint64_t units_read, units_written;   /* thousands of 512-byte units */
    uint64_t host_reads, host_writes;     /* commands completed */
    uint64_t power_cycles, unsafe_shutdowns, media_errors;
    uint8_t  critical;                    /* critical warning bits */
    uint8_t  percent_used;
};
/* The health log, waited for (at the start only: the loop must not wait,
 * so a request for it sends its own with ctrl_admin_send and reads it
 * with health_parse): the log page at c->data. */
status_t ctrl_health(struct nvme *c, struct nvme_health *h);
/* The health log's admin arguments (into c->data), and its fields. */
void     health_args(const struct nvme *c, struct admin_args *a);
void     health_parse(const uint8_t *page, struct nvme_health *h);

/* ---- serve.c: `storage` ------------------------------------------------------------------ */

/* Port keys (main.c's loop hands every packet but KEY_IRQ to serve_packet). */
#define KEY_IRQ   1ull
#define KEY_SERVE 2ull                  /* DR_SERVE */
#define KEY_CONN  (1ull << 40)          /* | gen << 8 (16 bits) | slot: an svc.connect channel */
#define KEY_BLK   (1ull << 32)          /* | gen << 8 (16 bits) | slot: a `block` channel */

/* The digest buffer (pinned writable, mapped), then the partition table,
 * read through it with the library (waits: no client is served yet), and
 * logged. OK, or what failed: ERR_NO_MEMORY... (no buffer), or the
 * controller's failure (ctrl_poll's). A table that can't be read is no
 * failure: the disk is served with no partitions. */
status_t serve_start(struct nvme *c);
/* Watch DR_SERVE on c->port; then serve_work answers what is queued on it,
 * on the connects' channels and on the block channels. false: DR_SERVE
 * closed (devmgr is stopping us). */
status_t serve_watch(struct nvme *c);
bool     serve_work(struct nvme *c);
/* Something to do without waiting (a pass stopped at its guard). */
bool     serve_ready(void);
void     serve_packet(uint64_t key);
/* The end, after ctrl_stop (every read ended, bus mastering off): every
 * channel closed, every buffer unpinned. */
void     serve_stop(struct nvme *c);
/* create_table is over (table.c): its answer, st, to its request. */
void     serve_created(status_t st);

/* ---- table.c: the partition table ---------------------------------------------------------- */

/* The table read through buf (`size` bytes, mapped; the device reaches it
 * at pages), logged, and the drive's mode set from it. A table that can't
 * be read is no failure: the disk is served with no partitions, and
 * MODE_OTHER. OK, or the controller's failure (the read stuck). */
status_t table_start(struct nvme *c, void *buf, const uint64_t *pages, uint32_t size);
/* The table as read (no partitions if it couldn't be). */
const struct pt_table *table_get(void);
/* The table in a few words for the RESULTS line: "GPT, 4 partitions". */
void     table_describe(char *out, size_t n);
/* Start create_table on a MODE_BLANK drive, or on a MODE_OTHER one whose
 * table was read and whose erase the owner confirmed: `tail` (tail_len
 * bytes) the end of its serial number, `word` "erase" (table_erase_ok;
 * ERR_ACCESS_DENIED otherwise, and on a MODE_JAMOS drive; ERR_BAD_STATE:
 * one is under way; pt_jamos_make's errors): OK, and serve_created later,
 * from the loop. */
status_t table_create(struct nvme *c, const uint8_t *disk_guid, const uint8_t *esp_guid,
                      const uint8_t *data_guid, const char *tail, uint32_t tail_len,
                      const char *word, uint32_t word_len);
/* Was the drive's table read without an error (what storage.mode calls
 * `readable`)? Only such a drive is ever erased. */
bool     table_readable(void);
/* create_table's writes are over: table_work reads the table back (the
 * loop calls it; table_ready: it has something to do). */
bool     table_ready(void);
void     table_work(struct nvme *c);
/* Its buffer unpinned (after ctrl_stop: bus mastering is off). */
void     table_stop(struct nvme *c);

/* ---- block.c: `block` channels ----------------------------------------------------------- */

/* A `block` channel limited to partition `index` of the table (its
 * blocks [start, start + blocks)), bound to c->port; *out: the client's
 * end. Writable only on a MODE_JAMOS drive (ERR_ACCESS_DENIED otherwise:
 * serve.c asks first). ERR_NO_RESOURCES: BLK_MAX open already. */
status_t blk_open(struct nvme *c, uint8_t index, uint64_t start, uint64_t blocks, bool writable,
                  handle_t *out);
void     blk_packet(uint64_t key);
/* Serve what the channels have queued; true if any was looked at. */
bool     blk_serve(struct nvme *c);
bool     blk_ready(void);
/* NVME_TEST_SLOW: answer the reads whose time has come; the next such
 * time (DEADLINE_NEVER: none). */
void     blk_due(struct nvme *c);
uint64_t blk_next_due(void);
/* Every channel closed and its buffer unpinned (bus mastering off). */
void     blk_close_all(struct nvme *c);

/* ---- selftest.c ---------------------------------------------------------------------- */

/* Parse the test words (nvmetest=selftest,loop,reset,abort,slow). */
uint32_t selftest_words(const struct driver_start *s);
/* Start the test reads (c->test says which); then each call after the
 * loop's work moves them on. false: the test can't run. */
bool     selftest_start(struct nvme *c);
/* Its buffer unpinned (after ctrl_stop: bus mastering is off). */
void     selftest_stop(struct nvme *c);
