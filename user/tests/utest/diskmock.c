/* utest: the mock usb-storage (see diskmock.h). One thread serves the
 * `storage` channel and every `block` channel opened from it, off a port;
 * dm_stop wakes it with a user packet. The thread allocates nothing and
 * prints nothing (libos's heap and printf belong to the test's thread):
 * what it has to tell goes into the counters. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/block.h>
#include <idl/storage.h>
#include <os.h>
#include "diskmock.h"
#include "utest.h"

#define KEY_STORAGE 1ull
#define KEY_STOP    2ull
#define KEY_BLOCK   16ull      /* + slot */
#define DM_BUFFER   65536u     /* a `block` channel's transfer buffer */
#define MBR_TABLE   446u       /* the partition table: 4 entries of 16 bytes */

static void bump(uint32_t *counter)
{
    __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED);
}

uint32_t dm_count(const uint32_t *counter)
{
    return __atomic_load_n(counter, __ATOMIC_RELAXED);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Partition `index` as the disk's own MBR has it (false: an empty entry). */
static bool mbr_entry(const struct diskmock *m, unsigned index, uint8_t *type, uint64_t *start,
                      uint64_t *blocks)
{
    const uint8_t *e = m->ram + MBR_TABLE + 16 * index;
    if (index >= 4 || !e[4])
        return false;
    *type = e[4];
    *start = le32(e + 8);
    *blocks = le32(e + 12);
    return true;
}

/* ---- block ----------------------------------------------------------------------- */

static status_t b_info(void *ctx, uint32_t *block_size, uint64_t *blocks, uint8_t *read_only)
{
    const struct dm_block *b = ctx;
    uint8_t type;
    uint64_t start;
    if (!mbr_entry(b->m, b->part, &type, &start, blocks))
        return ERR_BAD_STATE;
    *block_size = DM_BLOCK;
    *read_only = b->ro;
    return OK;
}

static status_t b_map_buffer(void *ctx, handle_t *buffer, uint32_t *size)
{
    struct dm_block *b = ctx;
    if (b->vmo)
        return ERR_BAD_STATE;
    status_t st = jam_vmo_create(DM_BUFFER, 0, HANDLE_INVALID, &b->vmo);
    if (st == OK && (st = jam_handle_duplicate(b->vmo, RIGHT_SAME, buffer)) != OK) {
        jam_handle_close(b->vmo);
        b->vmo = HANDLE_INVALID;
    }
    *size = DM_BUFFER;
    return st;
}

/* Where in RAM `count` blocks from `lba` of b's partition are, if both the
 * partition and the buffer (from `offset`) hold them. */
static status_t span(const struct dm_block *b, uint64_t lba, uint32_t count, uint32_t offset,
                     uint8_t **at)
{
    uint8_t type;
    uint64_t start, blocks;
    if (!b->vmo || !mbr_entry(b->m, b->part, &type, &start, &blocks))
        return ERR_BAD_STATE;
    if (lba > blocks || count > blocks - lba)
        return ERR_OUT_OF_RANGE;
    if (offset > DM_BUFFER || count > (DM_BUFFER - offset) / DM_BLOCK)
        return ERR_INVALID_ARGS;
    *at = b->m->ram + (start + lba) * DM_BLOCK;
    return OK;
}

static status_t b_read(void *ctx, uint64_t lba, uint32_t count, uint32_t offset)
{
    struct dm_block *b = ctx;
    uint8_t *at;
    status_t st = span(b, lba, count, offset, &at);
    if (st == OK)
        st = jam_vmo_write(b->vmo, offset, at, (uint64_t)count * DM_BLOCK);
    bump(st == OK ? &b->m->reads : &b->m->refused);
    return st;
}

static status_t b_write(void *ctx, uint64_t lba, uint32_t count, uint32_t offset)
{
    struct dm_block *b = ctx;
    uint8_t *at;
    status_t st = b->ro ? ERR_ACCESS_DENIED : span(b, lba, count, offset, &at);
    if (st == OK)
        st = jam_vmo_read(b->vmo, offset, at, (uint64_t)count * DM_BLOCK);
    bump(st == OK ? &b->m->writes : &b->m->refused);
    return st;
}

static status_t b_sync(void *ctx)
{
    struct dm_block *b = ctx;
    bump(&b->m->syncs);
    return OK;
}

static const struct block_ops block_ops = {
    .info = b_info, .map_buffer = b_map_buffer, .read = b_read, .write = b_write, .sync = b_sync,
};

static void block_close(struct dm_block *b)
{
    jam_port_unbind(b->m->port, b->ch, KEY_BLOCK + (uint64_t)(b - b->m->blk));
    jam_handle_close(b->ch);
    if (b->vmo)
        jam_handle_close(b->vmo);
    b->ch = b->vmo = HANDLE_INVALID;
}

/* ---- storage --------------------------------------------------------------------- */

static status_t s_info(void *ctx, uint8_t vendor[8], uint8_t product[16], uint32_t *block_size,
                       uint64_t *blocks, uint8_t *partitions)
{
    struct diskmock *m = ctx;
    uint8_t type;
    uint64_t start, n;
    memcpy(vendor, "JAMOS   ", 8);
    memcpy(product, "utest RAM disk  ", 16);
    *block_size = DM_BLOCK;
    *blocks = DM_BLOCKS;
    *partitions = 0;
    while (mbr_entry(m, *partitions, &type, &start, &n))
        (*partitions)++;
    bump(&m->infos);
    return OK;
}

static status_t s_partition(void *ctx, uint8_t index, uint8_t *type, uint64_t *start,
                            uint64_t *blocks)
{
    struct diskmock *m = ctx;
    bump(&m->partitions);
    return mbr_entry(m, index, type, start, blocks) ? OK : ERR_OUT_OF_RANGE;
}

static status_t s_open_partition(void *ctx, uint8_t index, uint8_t read_only, handle_t *block)
{
    struct diskmock *m = ctx;
    uint8_t type;
    uint64_t start, blocks;
    if (index > 1 || !mbr_entry(m, index, &type, &start, &blocks))
        return ERR_OUT_OF_RANGE;
    unsigned slot = 0;
    while (slot < DM_MAX_OPEN && m->blk[slot].ch)
        slot++;
    if (slot == DM_MAX_OPEN)
        return ERR_NO_RESOURCES;
    handle_t ours;
    status_t st = jam_channel_create(&ours, block);
    if (st != OK)
        return st;
    st = jam_port_bind(m->port, ours, KEY_BLOCK + slot, SIG_READABLE | SIG_PEER_CLOSED,
                       PORT_BIND_PERSISTENT);
    if (st != OK) {
        jam_handle_close(ours);
        jam_handle_close(*block);
        *block = HANDLE_INVALID;
        return st;
    }
    m->blk[slot] = (struct dm_block){ .m = m, .ch = ours, .part = index, .ro = read_only != 0 };
    bump(&m->opened[index]);
    if (!read_only)
        bump(&m->opened_rw);
    return OK;
}

static const struct storage_ops storage_ops = {
    .info = s_info, .partition = s_partition, .open_partition = s_open_partition,
};

/* ---- the thread ------------------------------------------------------------------ */

static void serve(void *arg)
{
    struct diskmock *m = arg;
    for (;;) {
        struct port_packet pkt;
        if (jam_port_wait(m->port, DEADLINE_NEVER, &pkt) != OK || pkt.key == KEY_STOP)
            return;
        if (pkt.key == KEY_STORAGE) {
            while (storage_serve_one(m->storage, &storage_ops, m) == OK)
                ;
            continue;   /* devmgr gone: the test stops us */
        }
        struct dm_block *b = &m->blk[(pkt.key - KEY_BLOCK) % DM_MAX_OPEN];
        if (pkt.key < KEY_BLOCK || !b->ch)
            continue;
        status_t st;
        while ((st = block_serve_one(b->ch, &block_ops, b)) == OK)
            ;
        if (st != ERR_SHOULD_WAIT) {
            bump(&m->closed[b->part]);
            block_close(b);
        }
    }
}

/* One MBR partition entry: type and extent (the CHS fields stay 0). */
static void mbr_set(uint8_t *ram, unsigned index, uint8_t type, uint32_t start, uint32_t blocks)
{
    uint8_t *e = ram + MBR_TABLE + 16 * index;
    e[4] = type;
    for (unsigned i = 0; i < 4; i++) {
        e[8 + i] = (uint8_t)(start >> (8 * i));
        e[12 + i] = (uint8_t)(blocks >> (8 * i));
    }
}

bool dm_start(struct diskmock *m, uint8_t type1, uint8_t type2, handle_t *client)
{
    uint64_t addr = 0;
    memset(m, 0, offsetof(struct diskmock, stack));
    /* a VMO of its own, not the heap: only the blocks written take memory */
    CHECK_ST(jam_vmo_create(DM_BLOCKS * DM_BLOCK, 0, HANDLE_INVALID, &m->ram_vmo), OK);
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), m->ram_vmo, 0, DM_BLOCKS * DM_BLOCK,
                          VMAR_READ | VMAR_WRITE, &addr), OK);
    m->ram = (uint8_t *)(uintptr_t)addr;
    mbr_set(m->ram, 0, type1, DM_ESP_START, DM_ESP_BLOCKS);
    mbr_set(m->ram, 1, type2, DM_DATA_START, DM_DATA_BLOCKS);
    m->ram[510] = 0x55;
    m->ram[511] = 0xaa;
    CHECK_ST(jam_port_create(&m->port), OK);
    CHECK_ST(jam_channel_create(client, &m->storage), OK);
    CHECK_ST(jam_port_bind(m->port, m->storage, KEY_STORAGE, SIG_READABLE, PORT_BIND_PERSISTENT),
             OK);
    CHECK_ST(thread_spawn("diskmock", serve, m, m->stack, sizeof(m->stack), &m->thread), OK);
    return true;
}

bool dm_stop(struct diskmock *m)
{
    struct port_packet stop = { .key = KEY_STOP, .type = PORT_PACKET_USER };
    CHECK_ST(jam_port_queue(m->port, &stop), OK);
    if (!wait_threads(&m->thread, 1))
        return false;
    for (unsigned i = 0; i < DM_MAX_OPEN; i++)
        if (m->blk[i].ch)
            block_close(&m->blk[i]);
    CHECK_ST(jam_handle_close(m->storage), OK);
    CHECK_ST(jam_handle_close(m->port), OK);
    CHECK_ST(jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)m->ram,
                            DM_BLOCKS * DM_BLOCK), OK);
    CHECK_ST(jam_handle_close(m->ram_vmo), OK);
    m->ram = NULL;
    return true;
}
