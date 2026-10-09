/* nvme: what the driver's files share (drv/nvme). main.c: start, the
 * loop and ending; ctrl.c: the controller's registers, enable, disable,
 * identify, the queues' creation, reset and shutdown; queue.c: completion
 * queues, command slots and ids, timeouts and abort, PRPs and reads;
 * cmd.c: submission queues, the ONE place a submission entry is written;
 * serve.c: the partition table and the `storage` protocol (digest,
 * health); block.c: the `block` channels; selftest.c: the test words'
 * reads.
 *
 * One thread; nothing is locked. Every command is a slot of its queue
 * (struct nvme_slot): its id is the slot's index and the slot's
 * generation, so a completion that arrives for a slot reused since (after
 * a reset, or an abort) is recognised and ignored.
 *
 * The read-only rule (docs/M12.7-PLAN.md, "The read-only guarantee"):
 * cmd.c alone knows the submission queues' memory (struct nvme_sq is
 * opaque here) and the opcodes; it builds only the commands its enum
 * lists and checks each one again at run time. tools/checknvme.py keeps
 * it that way at build time. Nothing in this driver can write a block. */
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
#define NVME_CMD_MAX   (1u << 20)   /* bytes one read command moves at most (and MDTS) */
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

/* One command in flight on a queue. */
struct nvme_slot {
    bool     busy;           /* a command with this slot's id is out */
    bool     aborted;        /* an Abort naming it was sent */
    bool     is_abort;       /* it IS an Abort */
    uint16_t gen;            /* bumped at every use: the id's upper bits */
    uint64_t deadline;       /* when it is aborted (or the controller reset) */
    struct nvme_io *io;      /* the read it is part of (I/O queue), else NULL */
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
    bool     stopping;         /* DR_SERVE closed: no new reads */
    struct nvme_queue admin, io;

    /* Identify (CNS 1): strings trimmed, NUL-terminated. */
    char     model[41], serial[21], firmware[9];
    uint32_t max_cmd;          /* bytes a read command may move */
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

    /* Reads waiting for a slot, first to last (an io partly sent stays first). */
    struct nvme_io *wait_head, *wait_tail;

    /* Counts, for the log. */
    uint64_t irqs, polls, completions, stale, reads, read_fail, aborts;
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

/* A read of `blocks` blocks at `lba` into a buffer whose pages the
 * device reaches at pages[0..npages), starting `offset` bytes in (a
 * multiple of 4). done is called once, from the loop, with OK or the
 * first failure: ERR_IO (the device failed a command or a reset failed
 * it twice), ERR_TIMED_OUT (aborted after its deadline), ERR_PEER_CLOSED
 * (the driver is stopping). The caller keeps io and the buffer until
 * then. */
struct nvme_io {
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
    bool     retried;          /* resubmitted after a reset once already */
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

/* Queue q's submission queue: its page, pinned read only, and its tail
 * doorbell. OK, or what failed (nothing kept). */
status_t sq_create(struct nvme *c, struct nvme_queue *q);
void     sq_destroy(struct nvme *c, struct nvme_queue *q);
/* Empty again (the controller was disabled): tail 0, entries zeroed. */
void     sq_reset(struct nvme_queue *q);
uint64_t sq_addr(const struct nvme_queue *q);   /* for ASQ */
/* Send an admin command in slot s of c->admin, or a read in slot s of
 * c->io. Anything outside the stage's list ends the driver (exit 1,
 * a RESULTS line): they never return an error for it. */
void     sqe_admin(struct nvme *c, struct nvme_slot *s, enum admin_op op,
                   const struct admin_args *a);
void     sqe_read(struct nvme *c, struct nvme_slot *s, uint64_t lba, uint32_t blocks,
                  uint64_t prp1, uint64_t prp2);
/* The command id a slot's command carries. */
uint16_t slot_cid(const struct nvme_queue *q, const struct nvme_slot *s);

/* ---- queue.c: completions, slots, timeouts, reads ------------------------------------ */

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
 * reads put back at the front of the wait list (once; then ERR_IO). */
void     queue_fail_all(struct nvme *c);
/* Reads: see struct nvme_io. ERR_INVALID_ARGS / ERR_OUT_OF_RANGE before
 * anything is sent; else OK and io->done later. */
status_t nvme_read(struct nvme *c, struct nvme_io *io);
/* Take io off the wait list if none of its commands was ever sent (its
 * done is then never called): true. false: it is out, or not waiting. */
bool     io_cancel(struct nvme *c, struct nvme_io *io);
/* Send what the wait list can (slots permitting). */
void     io_kick(struct nvme *c);
/* Every read still waiting or out ends with st (the driver stops). */
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
 * flight resubmitted. ERR_IO: RESETS_MAX in RESET_WINDOW, or it failed. */
status_t ctrl_reset(struct nvme *c, const char *why);
/* The end: reads failed, the I/O queues deleted, the normal shutdown
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

/* ---- serve.c: the disk's table and `storage` ------------------------------------------ */

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
/* The table in a few words for the RESULTS line: "GPT, 4 partitions". */
void     serve_describe(char *out, size_t n);
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

/* ---- block.c: `block` channels ----------------------------------------------------------- */

/* A read-only `block` channel limited to partition `part` (index `index`
 * of the table), bound to c->port; *out: the client's end.
 * ERR_NO_RESOURCES: BLK_MAX open already. */
status_t blk_open(struct nvme *c, uint8_t index, uint64_t start, uint64_t blocks, handle_t *out);
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
