/* utest: drv/nvme's `storage` and `block` protocols, on the first NVMe
 * drive devmgr has bound (tools/nvme-test.sh's `block` case gives QEMU
 * one: the Windows-like image of tools/nvme-img.py). Without one the
 * tests skip themselves (the init run has no NVMe drive). The storage
 * channel is the test's own (devmgrctl.disk_storage, on the control
 * channel); volumes keeps its own meanwhile.
 *   nvme_storage   info, the GPT partitions, open_partition read-only only,
 *                  ident, health; digest's refusals, and digest equal to
 *                  the SHA-256 of the same blocks read through a block
 *                  channel; the region lines of tools/disk-regions.py for
 *                  the GPT areas and the two small partitions, which the
 *                  test script compares with the host's
 *   nvme_block     block.info, map_buffer once, every refusal (range,
 *                  offset, buffer, write), sync
 *   nvme_inflight  32 reads sent without waiting, each answered once by its
 *                  txid, in whatever order the drive finished them, with
 *                  the bytes a read of the same blocks alone gives
 *   nvme_fence     a client leaves with its reads out: on the next client's
 *                  channel to the same partition nothing of its comes, only
 *                  the new client's own answers, and its data is right
 *                  (the driver's log says the drain rule held the partition) */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/block.h>
#include <idl/devmgr.h>
#include <idl/storage.h>
#include <sha256.h>
#include "utest.h"

#define WAIT       (10 * NS_PER_S)
#define NVME_CLASS 0x010802u
#define BUF        (1u << 20)
#define DEPTH      32

static const uint8_t esp_guid[16] = {   /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B, on disk */
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};

/* A storage channel of our own to the first NVMe drive; false (and a
 * line) when there is none. */
static bool nvme_storage(handle_t *out)
{
    handle_t dm = devmgr();
    *out = HANDLE_INVALID;
    if (!dm)
        return false;
    status_t st = devmgrctl_disk_storage_within(dm, WAIT, DEVMGR_KIND_PCI_CLASS, NVME_CLASS, 0, 0,
                                                out);
    if (st != OK)
        printf("utest: %s: no NVMe drive (%s): skipped\n", utest_cur, status_str(st));
    return st == OK;
}

/* The drive's block size and size, and its partitions' count. */
struct disk {
    uint32_t bs, parts;
    uint64_t blocks;
};

static bool disk_info(handle_t s, struct disk *d)
{
    char vendor[9], product[17];
    uint32_t vl, pl, mbr_id;
    uint8_t scheme, guid[16];
    CHECK_ST(storage_info_within(s, WAIT, vendor, &vl, product, &pl, &d->bs, &d->blocks, &scheme,
                                 &d->parts, &mbr_id, guid), OK);
    CHECK(d->bs == 512 || d->bs == 4096);
    CHECK_EQ(scheme, STORAGE_SCHEME_GPT);
    CHECK(d->parts >= 2);
    return true;
}

/* Open partition `index` read-only and give it a buffer of BUF bytes,
 * mapped here at *map. */
static bool open_ro(handle_t s, uint32_t index, handle_t *blk, const uint8_t **map)
{
    handle_t vmo;
    uint32_t size;
    uint64_t addr = 0;
    CHECK_ST(storage_open_partition_within(s, WAIT, index, 1, blk), OK);
    CHECK_ST(block_map_buffer_within(*blk, WAIT, BUF, &vmo, &size), OK);
    CHECK_EQ(size, BUF);
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, BUF, VMAR_READ, &addr), OK);
    jam_handle_close(vmo);   /* the mapping keeps it */
    *map = (const uint8_t *)(uintptr_t)addr;
    return true;
}

static void unmap(const uint8_t *map)
{
    (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)map, BUF);
}

/* The next answer on ch within WAIT: its txid and status. */
static bool answer(handle_t ch, uint32_t *txid, status_t *st)
{
    _Alignas(8) uint8_t rep[64];
    struct idl_msg m;
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(ch, SIG_READABLE, now() + WAIT, &seen), OK);
    CHECK_ST(idl_reply_read(ch, rep, sizeof(rep), &m), OK);
    *txid = m.txid;
    *st = block_read_result(rep, &m);   /* read and sync answer alike: a status */
    return true;
}

/* The SHA-256 of `count` blocks from `lba` of a partition, read through
 * its block channel one buffer at a time. */
static bool hash_through(handle_t blk, const uint8_t *map, uint32_t bs, uint64_t lba,
                         uint64_t count, uint8_t out[SHA256_BYTES])
{
    struct sha256 s;
    sha256_init(&s);
    while (count) {
        uint32_t n = count < BUF / bs ? (uint32_t)count : BUF / bs;
        CHECK_ST(block_read_within(blk, WAIT, lba, n, 0), OK);
        sha256_add(&s, map, (size_t)n * bs);
        lba += n;
        count -= n;
    }
    sha256_done(&s, out);
    return true;
}

/* One line of tools/disk-regions.py's list, from storage.digest. */
static bool region(handle_t s, const char *name, uint64_t lba, uint64_t count)
{
    uint8_t d[SHA256_BYTES];
    char hex[2 * SHA256_BYTES + 1];
    CHECK_ST(storage_digest_within(s, 4 * WAIT, lba, count, d), OK);
    sha256_hex(d, hex);
    printf("utest: nvme region %s lba %lu count %lu sha256 %s\n", name, (unsigned long)lba,
           (unsigned long)count, hex);
    return true;
}

bool t_nvme_storage(void)
{
    handle_t s, blk;
    struct disk d;
    if (!nvme_storage(&s))
        return true;
    if (!disk_info(s, &d))
        return false;
    /* The ESP first, as the PC's; past the last: refused. */
    uint8_t mbr_type, type[16], uniq[16];
    uint64_t start, blocks, attr;
    char name[109];
    uint32_t nl;
    CHECK_ST(storage_partition_within(s, WAIT, 0, &mbr_type, type, uniq, &start, &blocks, &attr,
                                      name, &nl), OK);
    CHECK(!memcmp(type, esp_guid, 16));
    CHECK_EQ(mbr_type, 0);
    CHECK(nl > 0);
    CHECK_ST(storage_partition_within(s, WAIT, d.parts, &mbr_type, type, uniq, &start, &blocks,
                                      &attr, name, &nl), ERR_OUT_OF_RANGE);
    /* Writable: never, whatever the index; bad arguments: refused. */
    CHECK_ST(storage_open_partition_within(s, WAIT, 0, 0, &blk), ERR_ACCESS_DENIED);
    CHECK_ST(storage_open_partition_within(s, WAIT, 0, 2, &blk), ERR_INVALID_ARGS);
    CHECK_ST(storage_open_partition_within(s, WAIT, d.parts, 1, &blk), ERR_OUT_OF_RANGE);
    /* Who it is, and its health log. */
    char model[41], serial[21], fw[9];
    uint32_t ml, sl, fl;
    CHECK_ST(storage_ident_within(s, WAIT, model, &ml, serial, &sl, fw, &fl), OK);
    CHECK(ml > 0 && sl > 0);
    uint64_t ur, uw, hr, hw, pc, us;
    uint8_t used, crit;
    CHECK_ST(storage_health_within(s, WAIT, &ur, &uw, &hr, &hw, &pc, &us, &used, &crit), OK);
    CHECK(hr > 0);
    printf("utest: %s: %.*s, serial %.*s, firmware %.*s; host read commands %lu, host write "
           "commands %lu\n", utest_cur, (int)ml, model, (int)sl, serial, (int)fl, fw,
           (unsigned long)hr, (unsigned long)hw);
    /* digest's refusals. */
    uint8_t sum[SHA256_BYTES], want[SHA256_BYTES];
    CHECK_ST(storage_digest_within(s, WAIT, 0, 0, sum), ERR_INVALID_ARGS);
    CHECK_ST(storage_digest_within(s, WAIT, d.blocks, 1, sum), ERR_OUT_OF_RANGE);
    CHECK_ST(storage_digest_within(s, WAIT, d.blocks - 1, 2, sum), ERR_OUT_OF_RANGE);
    CHECK_ST(storage_digest_within(s, WAIT, 0, STORAGE_DIGEST_MAX / d.bs + 1, sum),
             ERR_OUT_OF_RANGE);
    /* digest = the same blocks read through a block channel (the ESP). */
    CHECK_ST(storage_partition_within(s, WAIT, 0, &mbr_type, type, uniq, &start, &blocks, &attr,
                                      name, &nl), OK);
    const uint8_t *map;
    if (!open_ro(s, 0, &blk, &map))
        return false;
    if (!hash_through(blk, map, d.bs, 0, blocks, want))
        return false;
    CHECK_ST(storage_digest_within(s, 4 * WAIT, start, blocks, sum), OK);
    CHECK(!memcmp(sum, want, SHA256_BYTES));
    unmap(map);
    jam_handle_close(blk);
    /* The region lines the script checks against the host's. */
    if (!region(s, "gpt-head", 0, 34))
        return false;
    for (uint32_t i = 0; i < 2; i++) {
        char part[8];
        CHECK_ST(storage_partition_within(s, WAIT, i, &mbr_type, type, uniq, &start, &blocks,
                                          &attr, name, &nl), OK);
        snprintf(part, sizeof(part), "part%u", i + 1);
        if (!region(s, part, start, blocks))
            return false;
    }
    if (!region(s, "gpt-tail", d.blocks - 33, 33))
        return false;
    jam_handle_close(s);
    return true;
}

bool t_nvme_block(void)
{
    handle_t s, blk, bare, vmo;
    struct disk d;
    if (!nvme_storage(&s))
        return true;
    if (!disk_info(s, &d))
        return false;
    /* A channel with no buffer yet. */
    CHECK_ST(storage_open_partition_within(s, WAIT, 1, 1, &bare), OK);
    CHECK_ST(block_read_within(bare, WAIT, 0, 1, 0), ERR_BAD_STATE);
    const uint8_t *map;
    if (!open_ro(s, 1, &blk, &map))
        return false;
    uint32_t bs, max, bufmax, size;
    uint64_t blocks;
    uint8_t ro;
    uint16_t depth;
    CHECK_ST(block_info_within(blk, WAIT, &bs, &blocks, &ro, &max, &depth, &bufmax), OK);
    CHECK_EQ(bs, d.bs);
    CHECK_EQ(ro, 1);
    CHECK_EQ(depth, DEPTH);
    CHECK_EQ(bufmax, BUF);
    CHECK_EQ(max, BUF / bs);
    CHECK_ST(block_map_buffer_within(blk, WAIT, 0, &vmo, &size), ERR_BAD_STATE);
    /* Refused: no blocks, an offset that isn't a multiple of 4, past the
     * partition (also wrapping), past the buffer. */
    CHECK_ST(block_read_within(blk, WAIT, 0, 0, 0), ERR_INVALID_ARGS);
    CHECK_ST(block_read_within(blk, WAIT, 0, 1, 2), ERR_INVALID_ARGS);
    CHECK_ST(block_read_within(blk, WAIT, blocks, 1, 0), ERR_OUT_OF_RANGE);
    CHECK_ST(block_read_within(blk, WAIT, blocks - 1, 2, 0), ERR_OUT_OF_RANGE);
    CHECK_ST(block_read_within(blk, WAIT, ~0ull, 2, 0), ERR_OUT_OF_RANGE);
    CHECK_ST(block_read_within(blk, WAIT, 0, BUF / bs + 1, 0), ERR_OUT_OF_RANGE);
    CHECK_ST(block_read_within(blk, WAIT, 0, BUF / bs, 4), ERR_OUT_OF_RANGE);
    CHECK_ST(block_read_within(blk, WAIT, 0, 1, BUF), ERR_OUT_OF_RANGE);
    /* The last block, a whole buffer, and a sync; never a write. */
    CHECK_ST(block_read_within(blk, WAIT, blocks - 1, 1, BUF - bs), OK);
    CHECK_ST(block_read_within(blk, WAIT, 0, BUF / bs, 0), OK);
    CHECK_ST(block_sync_within(blk, WAIT), OK);
    CHECK_ST(block_write_within(blk, WAIT, 0, 1, 0), ERR_NOT_SUPPORTED);
    unmap(map);
    jam_handle_close(blk);
    jam_handle_close(bare);
    jam_handle_close(s);
    return true;
}

/* Request i of nvme_inflight: its blocks and buffer range. The first is
 * the biggest (256 KiB), so the small ones after it may well finish
 * first; together they fill the buffer without overlapping. */
static void slice(uint32_t i, uint32_t bs, uint64_t *lba, uint32_t *count, uint32_t *offset)
{
    uint32_t big = (256u << 10) / bs, small = (24u << 10) / bs;
    *count = i ? small : big;
    *offset = i ? (256u << 10) + (i - 1) * (24u << 10) : 0;
    *lba = (uint64_t)i * 997 % 4096;   /* scattered over the partition's start */
}

bool t_nvme_inflight(void)
{
    handle_t s, blk;
    struct disk d;
    if (!nvme_storage(&s))
        return true;
    if (!disk_info(s, &d))
        return false;
    const uint8_t *map;
    if (!open_ro(s, 0, &blk, &map))
        return false;
    for (uint32_t i = 0; i < DEPTH; i++) {
        uint64_t lba;
        uint32_t count, offset;
        slice(i, d.bs, &lba, &count, &offset);
        CHECK_ST(block_read_send(blk, 1000 + i, lba, count, offset), OK);
    }
    /* Each answered once, by its txid, in whatever order. */
    uint32_t seen = 0, order = 0, later = 0;
    for (uint32_t k = 0; k < DEPTH; k++) {
        uint32_t txid;
        status_t st;
        if (!answer(blk, &txid, &st))
            return false;
        CHECK(txid >= 1000 && txid < 1000 + DEPTH);
        CHECK(!(seen & 1u << (txid - 1000)));
        CHECK_ST(st, OK);
        seen |= 1u << (txid - 1000);
        later += txid - 1000 < order;   /* answered after one sent after it */
        order = txid - 1000 > order ? txid - 1000 : order;
    }
    printf("utest: %s: %u of %u answers came after a later request's\n", utest_cur, later,
           DEPTH);
    /* The bytes each got are those a read of the same blocks alone gives. */
    static uint8_t got[BUF];
    memcpy(got, map, BUF);
    for (uint32_t i = 0; i < DEPTH; i++) {
        uint64_t lba;
        uint32_t count, offset;
        slice(i, d.bs, &lba, &count, &offset);
        CHECK_ST(block_read_within(blk, WAIT, lba, count, offset), OK);
        CHECK(!memcmp(got + offset, map + offset, (size_t)count * d.bs));
    }
    unmap(map);
    jam_handle_close(blk);
    jam_handle_close(s);
    return true;
}

bool t_nvme_fence(void)
{
    handle_t s, a, b;
    struct disk d;
    if (!nvme_storage(&s))
        return true;
    if (!disk_info(s, &d))
        return false;
    /* The leaving client: 32 reads of a whole buffer each, sent; once the
     * first is answered (the drive is busy with the rest) it goes. */
    const uint8_t *ma, *mb;
    if (!open_ro(s, 0, &a, &ma))
        return false;
    for (uint32_t i = 0; i < DEPTH; i++)
        CHECK_ST(block_read_send(a, 1 + i, (uint64_t)i * (BUF / d.bs), BUF / d.bs, 0), OK);
    uint32_t txid;
    status_t st;
    if (!answer(a, &txid, &st))
        return false;
    unmap(ma);
    jam_handle_close(a);
    /* Its successor, on the same partition: only its own answers come. */
    if (!open_ro(s, 0, &b, &mb))
        return false;
    CHECK_ST(block_read_send(b, 77, 0, 8, 0), OK);
    CHECK_ST(block_sync_send(b, 78), OK);
    uint32_t got = 0;
    for (int k = 0; k < 2; k++) {
        if (!answer(b, &txid, &st))
            return false;
        CHECK(txid == 77 || txid == 78);
        CHECK_ST(st, OK);
        got |= 1u << (txid - 77);
    }
    CHECK_EQ(got, 3);
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(b, SIG_READABLE, now() + NS_PER_S, &seen), ERR_TIMED_OUT);
    /* And what it read is right. */
    static uint8_t first[8 * 4096];
    memcpy(first, mb, 8 * (size_t)d.bs);
    CHECK_ST(block_read_within(b, WAIT, 0, 8, 8 * d.bs), OK);
    CHECK(!memcmp(first, mb + 8 * d.bs, 8 * (size_t)d.bs));
    unmap(mb);
    jam_handle_close(b);
    jam_handle_close(s);
    return true;
}
