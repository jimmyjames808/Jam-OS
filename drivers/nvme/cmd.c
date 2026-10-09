/* nvme: the submission queues and the commands (drv/nvme). The ONE file
 * that writes a submission queue entry, and so the one that decides what
 * the drive is asked to do.
 *
 * The read-only guarantee (docs/M12.7-PLAN.md): the commands are the
 * opcodes below and no others, each built by one of two functions,
 * sqe_admin (enum admin_op) and sqe_read, which check their arguments
 * against the list; sqe_put, the one store of an entry, checks the opcode
 * against its queue once more. Anything else ends the driver at once
 * (exit 1, a RESULTS line): it fails closed. There is no code for any
 * command that writes the medium or changes the drive (writes, flushes,
 * formats, sanitize, firmware, security, namespace management, any other
 * feature): nothing to call by mistake. tools/checknvme.py, run by `make
 * check`, keeps this file's opcode list the stage's and keeps every other
 * file away from submission entries (struct nvme_sq is defined here
 * alone, so no other file can reach one).
 *
 * Each queue's submission side is one page (64 entries of 64 bytes) of a
 * contiguous VMO, pinned READ ONLY: the device only reads it. */
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

_Noreturn static void refuse(const char *what, uint32_t a, uint32_t b)
{
    drv_report("nvme: FAILED: refused to send %s (%#x, %#x): not in the read-only command list; "
               "the driver stops", what, a, b);
    drv_exit(1);
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
 * queue: Read alone. */
static bool allowed(uint16_t qid, uint8_t opc)
{
    if (qid == NVME_IO_QID)
        return opc == OPC_READ;
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
    if (!ns->nsid || !blocks || blocks > 0x10000 || lba >= ns->blocks ||
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
