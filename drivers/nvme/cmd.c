/* nvme: the submission queues and the commands (drv/nvme). The ONE file
 * that writes a submission queue entry, and so the one that decides what
 * the drive is asked to do.
 *
 * The commands (docs/M12.7-PLAN.md, "The read-only guarantee" and "Stage
 * 2"): the opcodes below and no others, each built by one function that
 * checks its arguments against the list: sqe_admin (enum admin_op),
 * sqe_read, sqe_write and sqe_flush; sqe_put, the one store of an entry,
 * checks the opcode against its queue (and a Write's or Flush's against
 * the drive's mode) once more. Anything else ends the driver at once
 * (exit 1, a RESULTS line): it fails closed. There is no code for any
 * other command that writes the medium or changes the drive (write
 * zeroes, formats, sanitize, firmware, security, namespace management,
 * any other feature): nothing to call by mistake.
 *
 * Writes: the gate. The drive's mode (enum nvme_mode) and the blocks a
 * write may reach are set here alone, by cmd_arm, from the partition
 * table the driver read itself (the library's pt_kind: Jam OS's mark,
 * blank, or other) when it starts and after its own create_table; the
 * `ro` word makes every drive MODE_OTHER. sqe_write looks at the mode
 * FIRST: on a drive that isn't Jam OS's (a partition write) or blank or
 * being erased (a write of its table) it refuses whatever the blocks are,
 * so a write to Windows' drive can't be built at all; only then does it
 * check that the blocks lie inside the target (one of the two Jam OS
 * partitions, or the blocks Jam OS's table takes, inside the blank rule's
 * zeros). The one other mode, MODE_ERASE, is the owner's: cmd_arm sets it
 * only when told the owner confirmed the erase (table.c's create_table
 * checked the end of the serial number and "erase") of a drive the table
 * calls someone else's, and only for that create_table: the table's
 * blocks as for a blank drive, and the blank rule's spots (where the two
 * partitions will start), nothing else; never a partition. Every Write
 * carries FUA (its completion means it is on the medium). Flush only on a
 * drive that isn't MODE_OTHER. tools/checknvme.py, run by `make check`,
 * keeps this file's opcode lists the stage's, a Write built in sqe_write
 * alone with the mode looked at before the range, the gate set in cmd_arm
 * alone, and every other file away from submission entries (struct
 * nvme_sq and the gate are defined here alone, so no other file can reach
 * them).
 *
 * Each queue's submission side is one page (64 entries of 64 bytes) of a
 * contiguous VMO, pinned READ ONLY: the device only reads it. */
#include <jam/parttable.h>
#include "nvme.h"

/* NVMe 1.4, Figure 139 (admin) and Figure 346 (NVM I/O): the stage's
 * commands, every one. */
enum {
    OPC_DELETE_SQ = 0x00,
    OPC_CREATE_SQ = 0x01,
    OPC_GET_LOG_PAGE = 0x02,
    OPC_DELETE_CQ = 0x04,
    OPC_CREATE_CQ = 0x05,
    OPC_IDENTIFY = 0x06,
    OPC_ABORT = 0x08,
    OPC_SET_FEATURES = 0x09,
};
enum {
    OPC_FLUSH = 0x00,
    OPC_WRITE = 0x01,
    OPC_READ = 0x02,
};

#define CNS_NS       0x00   /* identify a namespace */
#define CNS_CTRL     0x01   /* identify the controller */
#define CNS_NS_LIST  0x02   /* the active namespace list */
#define LOG_ERROR    0x01
#define LOG_HEALTH   0x02   /* SMART / health information */
#define FID_QUEUES   0x07   /* number of queues */
#define CQ_PC        (1u << 0)   /* physically contiguous */
#define CQ_IEN       (1u << 1)   /* interrupts enabled */
#define RW_FUA       (1u << 30)  /* Write's dword 12: Force Unit Access */
#define RW_MAX       0x10000u    /* blocks one read or write names at most (NLB, 16 bits) */

struct nvme_sq {
    uint16_t qid;              /* its queue */
    uint16_t depth;            /* entries */
    uint16_t tail;             /* the next entry to fill */
    handle_t vmo;              /* its page */
    volatile uint32_t *sq_mem; /* mapped: depth entries of 16 dwords */
    uint64_t addr;             /* the device address */
    uint64_t pin;
    bool     pinned;
    volatile void *db;         /* its tail doorbell */
};

static struct nvme_sq sqs[2];   /* the admin queue's and the I/O queue's */

/* The gate: what may be written, set by cmd_arm alone (see the header). */
static struct gate {
    enum nvme_mode mode;
    uint64_t disk;             /* the namespace's blocks when armed */
    uint64_t part_start[2];    /* MODE_JAMOS: the ESP's and the data partition's blocks */
    uint64_t part_blocks[2];
    uint64_t head;             /* MODE_BLANK, MODE_ERASE: the table's blocks [0, head) ... */
    uint64_t tail;             /* ... and [tail, disk) */
    uint64_t spot_start[2];    /* MODE_ERASE: the blank rule's spots, in blocks */
    uint64_t spot_blocks;
} gate;

_Noreturn static void refuse(const char *what, uint32_t a, uint32_t b)
{
    drv_report("nvme: FAILED: refused to send %s (%#x, %#x): not in the command list; "
               "the driver stops", what, a, b);
    drv_exit(1);
}

/* ---- the gate ------------------------------------------------------------------------ */

void cmd_arm(struct nvme *c, const struct pt_table *t, bool erase)
{
    enum pt_kind kind = t ? pt_kind(t) : PT_KIND_OTHER;
    struct pt_jamos_plan p;
    gate = (struct gate){ .mode = MODE_OTHER, .disk = c->ns.blocks };   /* nothing, first */
    if (c->ro || !c->ns.nsid)
        return;
    bool plan = pt_jamos_plan(c->ns.block_size, c->ns.blocks, &p) == OK;
    if (kind == PT_KIND_JAMOS && t->nparts == 2) {
        for (unsigned i = 0; i < 2; i++) {
            gate.part_start[i] = t->parts[i].start;
            gate.part_blocks[i] = t->parts[i].blocks;
        }
        gate.mode = MODE_JAMOS;
    } else if (kind == PT_KIND_BLANK && plan) {
        gate.head = p.head_blocks;
        gate.tail = p.tail_start;
        gate.mode = MODE_BLANK;
    } else if (erase && kind == PT_KIND_OTHER && t && plan) {
        gate.head = p.head_blocks;
        gate.tail = p.tail_start;
        gate.spot_start[0] = p.esp_start;    /* PT_BLANK_SPOT_ESP (gptcheck checks) */
        gate.spot_start[1] = p.data_start;   /* PT_BLANK_SPOT_DATA */
        gate.spot_blocks = PT_BLANK_SPOT_BYTES / c->ns.block_size;
        gate.mode = MODE_ERASE;
    }
}

enum nvme_mode cmd_mode(void)
{
    return gate.mode;
}

const char *cmd_mode_str(void)
{
    switch (gate.mode) {
    case MODE_JAMOS: return "a Jam OS disk: its two partitions writable";
    case MODE_BLANK: return "blank: create_table may write its table";
    case MODE_ERASE: return "being erased (the owner confirmed it): create_table writes its table";
    default:         return "read-only";
    }
}

/* Do `blocks` blocks at `lba` lie inside the gate's target `part`? Only
 * asked once the mode is right for it. Written so nothing can wrap. */
static bool in_gate(uint8_t part, uint64_t lba, uint32_t blocks)
{
    if (!blocks || blocks > RW_MAX || lba >= gate.disk || blocks > gate.disk - lba)
        return false;
    if (part == PART_TABLE) {
        if ((lba < gate.head && blocks <= gate.head - lba) || lba >= gate.tail)
            return true;
        for (unsigned i = 0; i < 2 && gate.mode == MODE_ERASE; i++) {
            uint64_t start = gate.spot_start[i];
            if (lba >= start && lba - start < gate.spot_blocks &&
                blocks <= gate.spot_blocks - (lba - start))
                return true;
        }
        return false;
    }
    if (part >= 2)
        return false;
    uint64_t start = gate.part_start[part], n = gate.part_blocks[part];
    return lba >= start && lba - start < n && blocks <= n - (lba - start);
}

/* ---- the queues' pages ---------------------------------------------------------- */

status_t sq_create(struct nvme *c, struct nvme_queue *q)
{
    if (q->qid > NVME_IO_QID || q->depth > NVME_PAGE / NVME_SQE_BYTES)
        return ERR_INVALID_ARGS;
    struct nvme_sq *sq = &sqs[q->qid];
    *sq = (struct nvme_sq){ .qid = q->qid, .depth = q->depth, .vmo = HANDLE_INVALID };
    void *p = NULL;
    status_t st = drv_vmo_create(NVME_PAGE, DRV_VMO_CONTIGUOUS, &sq->vmo);
    if (st == OK)
        st = drv_vmo_map(sq->vmo, 0, NVME_PAGE, VMAR_READ | VMAR_WRITE, &p);
    if (st == OK)
        sq->sq_mem = p;
    if (st == OK && (st = drv_vmo_pin_read_only(sq->vmo, c->dma, 0, NVME_PAGE, &sq->addr,
                                                &sq->pin)) == OK)
        sq->pinned = true;
    if (st != OK) {
        q->sq = sq;
        sq_destroy(c, q);
        return st;
    }
    sq->db = (volatile uint8_t *)c->dbs + 2u * q->qid * c->dstrd;
    q->sq = sq;
    return OK;
}

void sq_destroy(struct nvme *c, struct nvme_queue *q)
{
    struct nvme_sq *sq = q->sq;
    if (!sq)
        return;
    if (sq->pinned && drv_vmo_unpin(sq->vmo, c->dma, sq->pin) == OK)
        sq->pinned = false;
    if (sq->sq_mem)
        (void)drv_vmo_unmap((void *)sq->sq_mem, NVME_PAGE);
    if (sq->vmo != HANDLE_INVALID)
        drv_handle_close(sq->vmo);
    sq->sq_mem = NULL;
    sq->vmo = HANDLE_INVALID;
    q->sq = NULL;
}

void sq_reset(struct nvme_queue *q)
{
    struct nvme_sq *sq = q->sq;
    sq->tail = 0;
    for (uint32_t i = 0; i < NVME_PAGE / 4; i++)
        sq->sq_mem[i] = 0;
}

uint64_t sq_addr(const struct nvme_queue *q)
{
    return q->sq->addr;
}

uint16_t slot_cid(const struct nvme_queue *q, const struct nvme_slot *s)
{
    return (uint16_t)((uint32_t)(s - q->slots) | (uint32_t)s->gen << 6);
}

/* ---- the one store of an entry ------------------------------------------------------ */

/* May opcode opc go on queue qid? The admin queue: the admin list; the I/O
 * queue: Read, and Write and Flush only on a drive that isn't MODE_OTHER. */
static bool allowed(uint16_t qid, uint8_t opc)
{
    if (qid == NVME_IO_QID)
        return opc == OPC_READ ||
               (gate.mode != MODE_OTHER && (opc == OPC_WRITE || opc == OPC_FLUSH));
    if (qid != 0)
        return false;
    switch (opc) {
    case OPC_DELETE_SQ:
    case OPC_CREATE_SQ:
    case OPC_GET_LOG_PAGE:
    case OPC_DELETE_CQ:
    case OPC_CREATE_CQ:
    case OPC_IDENTIFY:
    case OPC_ABORT:
    case OPC_SET_FEATURES:
        return true;
    default:
        return false;
    }
}

/* One entry (NVMe 1.4, 4.2): dw[0] is the opcode and the id (filled
 * here), dw[1] the namespace, dw[6..9] PRP 1 and 2, dw[10..15] the
 * command's own. Then the tail doorbell. */
static void sqe_put(struct nvme_queue *q, struct nvme_slot *s, uint8_t opc, uint32_t dw[16])
{
    struct nvme_sq *sq = q->sq;
    if (!sq || sq != &sqs[q->qid] || !allowed(sq->qid, opc))
        refuse("an opcode on this queue", opc, q->qid);
    if (s < q->slots || s >= q->slots + q->depth || !s->busy)
        refuse("a command without its slot", opc, (uint32_t)(s - q->slots));
    dw[0] = (uint32_t)opc | (uint32_t)slot_cid(q, s) << 16;   /* FUSE 0, PSDT 0: PRPs */
    volatile uint32_t *e = sq->sq_mem + (uint32_t)sq->tail * (NVME_SQE_BYTES / 4);
    for (unsigned i = 0; i < 16; i++)
        e[i] = dw[i];
    sq->tail = (uint16_t)((sq->tail + 1) % sq->depth);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* the entry before the doorbell */
    drv_write32(sq->db, 0, sq->tail);
}

/* ---- the commands ---------------------------------------------------------------------- */

/* The admin command's arguments against the list; its opcode. Refused
 * (the driver stops) otherwise. */
static uint8_t admin_check(const struct nvme *c, enum admin_op op, const struct admin_args *a)
{
    uint32_t lo = a->cdw10 & 0xffff, hi = a->cdw10 >> 16;
    switch (op) {
    case ADMIN_IDENTIFY:
        if ((lo != CNS_NS && lo != CNS_CTRL && lo != CNS_NS_LIST) || hi || a->cdw11 ||
            a->prp1 != c->data_addr)
            break;
        return OPC_IDENTIFY;
    case ADMIN_GET_LOG_PAGE:   /* LID, no LSP, no RAE; NUMDL; one page at most */
        if (((lo & 0xff) != LOG_ERROR && (lo & 0xff) != LOG_HEALTH) || (lo >> 8) ||
            (hi + 1) * 4 > NVME_PAGE || a->cdw11 || a->prp1 != c->data_addr)
            break;
        return OPC_GET_LOG_PAGE;
    case ADMIN_SET_FEATURES:   /* the number of queues, not saved */
        if (a->cdw10 != FID_QUEUES || a->cdw11 || a->prp1 || a->nsid)
            break;   /* cdw11 0: one submission and one completion queue */
        return OPC_SET_FEATURES;
    case ADMIN_CREATE_CQ:
        if (lo != NVME_IO_QID || a->prp1 != c->io.cq_addr || a->cdw11 != (CQ_PC | CQ_IEN))
            break;   /* interrupts on vector 0 */
        return OPC_CREATE_CQ;
    case ADMIN_CREATE_SQ:   /* contiguous, on completion queue 1 */
        if (lo != NVME_IO_QID || a->prp1 || a->cdw11 != (NVME_IO_QID << 16 | CQ_PC))
            break;
        return OPC_CREATE_SQ;
    case ADMIN_DELETE_SQ:
    case ADMIN_DELETE_CQ:
        if (a->cdw10 != NVME_IO_QID || a->prp1 || a->cdw11)
            break;
        return op == ADMIN_DELETE_SQ ? OPC_DELETE_SQ : OPC_DELETE_CQ;
    case ADMIN_ABORT:   /* SQID 0 or 1, any command id */
        if (lo > NVME_IO_QID || a->prp1 || a->cdw11)
            break;
        return OPC_ABORT;
    }
    refuse("an admin command", (uint32_t)op, a->cdw10);
}

void sqe_admin(struct nvme *c, struct nvme_slot *s, enum admin_op op, const struct admin_args *a)
{
    uint8_t opc = admin_check(c, op, a);
    uint64_t prp1 = a->prp1;
    if (op == ADMIN_CREATE_SQ)
        prp1 = sqs[NVME_IO_QID].addr;   /* only ever the I/O queue's own page */
    uint32_t dw[16] = { 0 };
    dw[1] = a->nsid;
    dw[6] = (uint32_t)prp1;
    dw[7] = (uint32_t)(prp1 >> 32);
    dw[10] = a->cdw10;
    dw[11] = a->cdw11;
    sqe_put(&c->admin, s, opc, dw);
}

void sqe_read(struct nvme *c, struct nvme_slot *s, uint64_t lba, uint32_t blocks, uint64_t prp1,
              uint64_t prp2)
{
    const struct nvme_ns *ns = &c->ns;
    if (!ns->nsid || !blocks || blocks > RW_MAX || lba >= ns->blocks ||
        blocks > ns->blocks - lba || (prp1 & 3))
        refuse("a read", (uint32_t)lba, blocks);
    uint32_t dw[16] = { 0 };
    dw[1] = ns->nsid;
    dw[6] = (uint32_t)prp1;
    dw[7] = (uint32_t)(prp1 >> 32);
    dw[8] = (uint32_t)prp2;
    dw[9] = (uint32_t)(prp2 >> 32);
    dw[10] = (uint32_t)lba;
    dw[11] = (uint32_t)(lba >> 32);
    dw[12] = blocks - 1;   /* NLB, 0's based; no FUA, no limited retry, no protection */
    sqe_put(&c->io, s, OPC_READ, dw);
}

/* The one function that builds a Write: the mode first, then the blocks
 * (see the header). FUA: answered means on the medium. */
void sqe_write(struct nvme *c, struct nvme_slot *s, uint8_t part, uint64_t lba, uint32_t blocks,
               uint64_t prp1, uint64_t prp2)
{
    bool mode_ok = part == PART_TABLE ? (gate.mode == MODE_BLANK || gate.mode == MODE_ERASE)
                                      : gate.mode == MODE_JAMOS;
    if (!mode_ok)
        refuse("a write to a drive that isn't Jam OS's", gate.mode, part);
    if (!in_gate(part, lba, blocks) || !c->ns.nsid || gate.disk != c->ns.blocks || (prp1 & 3))
        refuse("a write outside its partition", (uint32_t)lba, part);
    uint32_t dw[16] = { 0 };
    dw[1] = c->ns.nsid;
    dw[6] = (uint32_t)prp1;
    dw[7] = (uint32_t)(prp1 >> 32);
    dw[8] = (uint32_t)prp2;
    dw[9] = (uint32_t)(prp2 >> 32);
    dw[10] = (uint32_t)lba;
    dw[11] = (uint32_t)(lba >> 32);
    dw[12] = (blocks - 1) | RW_FUA;   /* NLB, 0's based; no limited retry, no protection */
    sqe_put(&c->io, s, OPC_WRITE, dw);
}

/* A Flush of the namespace (the drive's volatile cache to the medium). */
void sqe_flush(struct nvme *c, struct nvme_slot *s)
{
    if (gate.mode == MODE_OTHER || !c->ns.nsid)
        refuse("a flush to a drive that isn't Jam OS's", gate.mode, 0);
    uint32_t dw[16] = { 0 };
    dw[1] = c->ns.nsid;
    sqe_put(&c->io, s, OPC_FLUSH, dw);
}
