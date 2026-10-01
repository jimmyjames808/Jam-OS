/* usb-storage: the partition table, and the `block` protocol
 * (abi/idl/block.idl), one channel per opened partition.
 *
 * Authority: a block channel reaches its partition's blocks and nothing
 * else. Every request's range is checked against the partition here
 * (check_range), before the partition's start is added; a read-only
 * channel refuses writes before anything else is looked at. Nothing
 * serves the whole disk, so the partition table and the space outside the
 * partitions can't be reached by any client.
 *
 * Data: the client maps a 64 KiB buffer VMO of its channel (map_buffer)
 * and names offsets in it. A request moves at most the buffer's 64 KiB;
 * it becomes as many READ(10) / WRITE(10) commands as the bulk buffer
 * needs. How the data gets between that VMO and usb-bus's bulk buffer:
 *   - a read-only channel: the client's handle has no RIGHT_WRITE, so it
 *     can neither write the buffer nor shrink or decommit it; usb-storage
 *     maps it and copies each block straight in (one copy);
 *   - a writable channel: the client must write the buffer, and the same
 *     right would let it shrink it under a mapping here (a fault, which
 *     would take every partition of the disk down with usb-storage). So
 *     usb-storage never maps it and moves the data with drv_vmo_read /
 *     drv_vmo_write, which a shrunk buffer only fails (the kernel copies
 *     through a buffer of its own: two copies). One copy there needs a
 *     right that allows writing a VMO but not resizing it.
 *
 * The table is the MBR's four primary entries (block 0, signature 55 AA
 * at 510): the used ones, in table order. An entry that doesn't fit on
 * the disk, or overlaps an earlier one (two channels would share blocks),
 * is left out. Extended and GPT-protective entries are listed as
 * they are (types 05 / 0F / EE): nobody mounts them.
 *
 * A stick with no table at all, whose block 0 is a FAT boot sector (a
 * "superfloppy": many sticks are sold that way), is listed as one
 * partition of type PART_WHOLE covering the whole disk. That is the one
 * case in which a channel reaches block 0: there is no table to protect.
 * The table is looked for first (mbr_table): block 0 of a partitioned
 * stick may look like a FAT boot sector too. */
#include <idl/block.h>
#include "storage.h"

/* An open block channel. */
struct blk {
    handle_t ch;         /* our end; HANDLE_INVALID: a free slot */
    struct disk *k;      /* the disk */
    uint8_t part;        /* index into k->parts */
    bool ro;             /* refuses writes */
    bool pending;        /* may have requests queued */
    uint16_t gen;        /* bumped at every open and close: in its port key */
    handle_t vmo;        /* the client's buffer (map_buffer), HANDLE_INVALID: not made yet */
    uint8_t *map;        /* a read-only channel's buffer, mapped here (NULL: not mapped) */
};

static struct blk blks[MAX_BLKS];

/* ---- the partition table ----------------------------------------------------------- */

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[1] << 8 | p[0];
}

/* Is s (block 0) a FAT boot sector for a volume that fits on the disk? An
 * MBR's first bytes may be a jump too, and its boot code may hold any
 * bytes, so every field a FAT volume needs is checked: the jump, 512 to
 * 4096 bytes per sector, a power-of-two cluster, reserved sectors, one or
 * two FATs, a size inside the disk, and the "FAT" of the type text (at 54
 * for FAT12/16, at 82 for FAT32). */
static bool fat_boot_sector(const struct disk *k, const uint8_t *s)
{
    if (s[0] != 0xeb && s[0] != 0xe9)
        return false;
    uint32_t bps = le16(s + 11), spc = s[13], total = le16(s + 19);
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || !spc || (spc & (spc - 1)))
        return false;
    if (!le16(s + 14) || s[16] < 1 || s[16] > 2)
        return false;
    if (!total)
        total = le32(s + 32);
    if (!total || (uint64_t)total * bps > k->blocks * k->block_size)
        return false;
    bool fat16 = s[54] == 'F' && s[55] == 'A' && s[56] == 'T';
    bool fat32 = s[82] == 'F' && s[83] == 'A' && s[84] == 'T' && s[85] == '3' && s[86] == '2';
    return fat16 || fat32;
}

/* Does s (block 0) hold a partition table? Every entry's status byte is
 * 00 or 80, at least one entry is in use, and none starts at block 0 (the
 * table's own block: mtools' mformat writes such an entry, covering the
 * whole disk, into the boot sector of a volume without a table). Asked before fat_boot_sector:
 * some formatters leave a jump and a BPB in the MBR of a partitioned
 * stick, and a disk with partitions must never be served whole. A FAT boot
 * sector has boot code or zeros where the table would be: text and code
 * fail the status bytes, zeros have no entry in use. */
static bool mbr_table(const uint8_t *s)
{
    bool used = false;
    for (int i = 0; i < MAX_PARTS; i++) {
        const uint8_t *e = s + 446 + 16 * i;
        bool in_use = e[4] && le32(e + 12);
        if ((e[0] != 0x00 && e[0] != 0x80) || (in_use && !le32(e + 8)))
            return false;
        used |= in_use;
    }
    return used;
}

status_t parts_read(struct disk *k)
{
    k->nparts = 0;
    if (!k->block_size)
        return OK;   /* no medium */
    status_t st = scsi_rw(k, false, 0, 1);
    if (st != OK)
        return st;
    const uint8_t *s = k->xbuf;
    if (s[510] != 0x55 || s[511] != 0xaa)
        return OK;   /* no partition table */
    if (!mbr_table(s) && fat_boot_sector(k, s)) {
        k->parts[k->nparts++] = (struct part){ .type = PART_WHOLE, .start = 0, .blocks = k->blocks };
        return OK;
    }
    for (int i = 0; i < MAX_PARTS; i++) {
        const uint8_t *e = s + 446 + 16 * i;
        uint64_t start = le32(e + 8), count = le32(e + 12);
        if (!e[4] || !count)
            continue;   /* an unused entry */
        if (!start || start >= k->blocks || count > k->blocks - start) {
            drv_log("usb-storage %04x:%04x: partition entry %d (type %02x, %lu + %lu) is not "
                    "inside the disk: left out", k->vid, k->pid, i, e[4], (unsigned long)start,
                    (unsigned long)count);
            continue;
        }
        bool overlaps = false;
        for (int j = 0; j < k->nparts; j++)
            overlaps |= start < k->parts[j].start + k->parts[j].blocks &&
                        k->parts[j].start < start + count;
        if (overlaps) {
            drv_log("usb-storage %04x:%04x: partition entry %d overlaps an earlier one: left out",
                    k->vid, k->pid, i);
            continue;
        }
        k->parts[k->nparts++] = (struct part){ .type = e[4], .start = start, .blocks = count };
    }
    return OK;
}

/* ---- the block protocol ------------------------------------------------------------- */

static status_t b_info(void *ctx, uint32_t *block_size, uint64_t *blocks, uint8_t *read_only)
{
    const struct blk *b = ctx;
    *block_size = b->k->block_size;
    *blocks = b->k->parts[b->part].blocks;
    *read_only = b->ro;
    return OK;
}

static status_t b_map_buffer(void *ctx, handle_t *buffer, uint32_t *size)
{
    struct blk *b = ctx;
    if (b->vmo != HANDLE_INVALID)
        return ERR_BAD_STATE;
    handle_t vmo, theirs;
    status_t st = drv_vmo_create(BLOCK_BUF, 0, &vmo);
    if (st != OK)
        return st;
    /* The client's handle: to map, read and (writable channel) write, and
     * no more. */
    rights_t r = RIGHT_READ | RIGHT_MAP | RIGHT_TRANSFER | (b->ro ? 0 : RIGHT_WRITE);
    st = drv_handle_duplicate(vmo, r, &theirs);
    if (st != OK) {
        drv_handle_close(vmo);
        return st;
    }
    void *map = NULL;
    if (b->ro && (st = drv_vmo_map(vmo, 0, BLOCK_BUF, VMAR_READ | VMAR_WRITE, &map)) != OK) {
        drv_handle_close(theirs);
        drv_handle_close(vmo);
        return st;
    }
    b->vmo = vmo;
    b->map = map;
    *buffer = theirs;
    *size = BLOCK_BUF;
    return OK;
}

/* Is [lba, lba + count) inside the partition, and count blocks at
 * `offset` inside the buffer? Written so nothing can wrap. */
static status_t check_range(const struct blk *b, uint64_t lba, uint32_t count, uint32_t offset)
{
    const struct part *p = &b->k->parts[b->part];
    uint32_t bs = b->k->block_size;
    if (b->vmo == HANDLE_INVALID)
        return ERR_BAD_STATE;   /* no buffer yet */
    if (!count)
        return ERR_INVALID_ARGS;
    if (count > BLOCK_BUF / bs || offset > BLOCK_BUF || count * bs > BLOCK_BUF - offset)
        return ERR_OUT_OF_RANGE;
    if (lba >= p->blocks || count > p->blocks - lba)
        return ERR_OUT_OF_RANGE;
    return OK;
}

/* The checked request, one command per bulk buffer's worth. */
static status_t transfer(struct blk *b, bool write, uint64_t lba, uint32_t count, uint32_t offset)
{
    struct disk *k = b->k;
    uint32_t bs = k->block_size, most = k->data_max / bs;
    lba += k->parts[b->part].start;
    while (count) {
        uint32_t n = count < most ? count : most;
        status_t st = OK;
        if (write)
            st = drv_vmo_read(b->vmo, offset, k->xbuf, (uint64_t)n * bs);
        if (st == OK)
            st = scsi_rw(k, write, lba, n);
        if (st == OK && !write && b->map)
            __builtin_memcpy(b->map + offset, k->xbuf, (size_t)n * bs);
        else if (st == OK && !write)
            st = drv_vmo_write(b->vmo, offset, k->xbuf, (uint64_t)n * bs);
        if (st != OK)
            return st;
        lba += n;
        count -= n;
        offset += n * bs;
    }
    return OK;
}

static status_t b_read(void *ctx, uint64_t lba, uint32_t count, uint32_t offset)
{
    struct blk *b = ctx;
    status_t st = check_range(b, lba, count, offset);
    return st == OK ? transfer(b, false, lba, count, offset) : st;
}

static status_t b_write(void *ctx, uint64_t lba, uint32_t count, uint32_t offset)
{
    struct blk *b = ctx;
    if (b->ro)
        return ERR_ACCESS_DENIED;
    status_t st = check_range(b, lba, count, offset);
    return st == OK ? transfer(b, true, lba, count, offset) : st;
}

static status_t b_sync(void *ctx)
{
    struct blk *b = ctx;
    return scsi_sync(b->k);
}

static const struct block_ops block_ops = {
    .info = b_info,
    .map_buffer = b_map_buffer,
    .read = b_read,
    .write = b_write,
    .sync = b_sync,
};

/* ---- the channels -------------------------------------------------------------------- */

static void blk_close(struct blk *b)
{
    if (b->ch == HANDLE_INVALID)
        return;
    drv_handle_close(b->ch);
    if (b->map)
        (void)drv_vmo_unmap(b->map, BLOCK_BUF);   /* our own mapping: nothing else to do */
    b->map = NULL;
    if (b->vmo != HANDLE_INVALID)
        drv_handle_close(b->vmo);
    b->ch = b->vmo = HANDLE_INVALID;
    b->pending = false;
    b->gen++;
}

status_t blk_open(struct disk *k, handle_t port, uint8_t index, bool read_only, handle_t *out)
{
    if (index >= k->nparts)
        return ERR_OUT_OF_RANGE;
    struct blk *b = NULL;
    for (int i = 0; i < MAX_BLKS && !b; i++)
        if (blks[i].ch == HANDLE_INVALID)
            b = &blks[i];
    if (!b)
        return ERR_NO_RESOURCES;
    handle_t ours, theirs;
    status_t st = drv_channel_create(&ours, &theirs);
    if (st != OK)
        return st;
    b->gen++;
    uint64_t key = KEY_BLK | ((uint64_t)b->gen << 8) | (uint64_t)(b - blks);
    st = drv_port_bind(port, ours, key, SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st != OK) {
        drv_handle_close(ours);
        drv_handle_close(theirs);
        return st;
    }
    b->ch = ours;
    b->k = k;
    b->part = index;
    b->ro = read_only;
    b->vmo = HANDLE_INVALID;
    b->map = NULL;
    b->pending = true;   /* look once: a request may be there already */
    *out = theirs;
    return OK;
}

void blk_packet(const struct port_packet *p)
{
    unsigned i = p->key & 0xff, gen = (p->key >> 8) & 0xffff;
    if (i < MAX_BLKS && blks[i].ch != HANDLE_INVALID && blks[i].gen == gen)
        blks[i].pending = true;
}

bool blk_serve(void)
{
    bool any = false;
    for (int i = 0; i < MAX_BLKS; i++) {
        struct blk *b = &blks[i];
        if (b->ch == HANDLE_INVALID || !b->pending)
            continue;
        any = true;
        b->pending = false;
        /* A few at a time, so one busy client doesn't starve the others. */
        status_t st = OK;
        for (int guard = 0; guard < 16 && st == OK && !b->k->gone; guard++)
            st = block_serve_one(b->ch, &block_ops, b);
        if (st == OK)
            b->pending = true;   /* maybe more: the port won't say so again */
        else if (st != ERR_SHOULD_WAIT)
            blk_close(b);        /* the client is gone */
    }
    return any;
}

void blk_close_all(void)
{
    for (int i = 0; i < MAX_BLKS; i++)
        blk_close(&blks[i]);
}
