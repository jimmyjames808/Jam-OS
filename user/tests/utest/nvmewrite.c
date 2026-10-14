/* utest: drv/nvme's write path (M12.7's stage 2), straight at the driver:
 * each drive's `storage` channel comes from devmgrctl.disk_storage, so
 * volumes is bypassed and every refusal below is the driver's own.
 * tools/nvme-test.sh's `write` case gives QEMU a Windows-like drive and a
 * blank one; its `diskro` case those and a Jam OS one, with `diskro`.
 *
 * THESE TESTS WRITE. They run only when named (`utest only nvmew_`,
 * `utest only nvmero_`: never in a run of every test, as the init run's
 * is), and touch only QEMU's drives, whose serial tools/qemu-test.sh sets
 * ("jamos-nvme<N>"): on the PC they find none and skip, whatever drives
 * it has.
 *   nvmew_refuse   the Windows-like drive (four partitions: not Jam OS's):
 *                  open_partition writable refused for every index and one
 *                  past the last (the drive's kind before the index),
 *                  create_table refused (good GUIDs, and zeros: the drive
 *                  before the arguments; and an erase confirmed wrongly:
 *                  the serial's end without "erase", a wrong end, "ERASE":
 *                  the driver's own check), storage.mode "other", block.write on a read-only
 *                  channel refused, sync OK. The script checks the image's
 *                  hash: nothing reached it.
 *   nvmew_prepare  the blank drive: a writable open refused (not Jam OS's
 *                  yet), GUIDs all zeros or the same refused, then
 *                  create_table, and the table it lists is Jam OS's layout;
 *                  create_table again refused. On a later boot (the drive
 *                  Jam OS's already) create_table refused.
 *   nvmew_check    the Jam OS drive: the blocks nvmew_write writes hold its
 *                  pattern if a boot before wrote them ("written by an
 *                  earlier boot"), or are all zeros ("nothing written yet").
 *   nvmew_write    the Jam OS drive's two partitions opened writable: the
 *                  refusals (past the partition, wrapping, count 0, an
 *                  offset not a multiple of 4), then the pattern written
 *                  (each partition's first and last 8 blocks and 64 KiB in
 *                  the middle), synced, and read back through a read-only
 *                  channel of its own.
 *   nvmero_refuse  (the `diskro` boot) every QEMU drive, blank, Jam OS's or
 *                  other, refuses a writable open and create_table, an
 *                  erase confirmed rightly too, and says `ro` (storage.mode).
 * The pattern (tools/nvme-written.py makes the same): block b of
 * partition p starts "JAMOSW1", a zero, p and b (32 bits each,
 * little-endian), then byte k is (b * 7 + k + p * 13) & 0xff. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/block.h>
#include <idl/devmgr.h>
#include <idl/storage.h>
#include "utest.h"

#define WAIT       (10 * NS_PER_S)
#define NVME_CLASS 0x010802u
#define BUF        (1u << 20)
#define DRIVES_MAX 4
#define RUN_BLOCKS 8                 /* the first and last blocks written */
#define MID_BYTES  (64u << 10)       /* and these in the middle */

enum drive_kind {
    DRIVE_NONE,                      /* no such QEMU drive */
    DRIVE_OTHER,                     /* the Windows-like one (or anything else) */
    DRIVE_BLANK,
    DRIVE_JAMOS,
};

/* A QEMU drive's storage channel, and what it holds. */
struct drive {
    handle_t s;
    uint32_t n;                      /* devmgr's index among NVMe functions */
    enum drive_kind kind;
    uint32_t bs, parts;
    uint64_t blocks;
};

static const uint8_t guid_disk[16] = { 0x4a, 0x41, 0x4d, 0x01, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
static const uint8_t guid_esp[16] = { 0x4a, 0x41, 0x4d, 0x02, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
static const uint8_t guid_data[16] = { 0x4a, 0x41, 0x4d, 0x03, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
static const uint8_t guid_zero[16];

/* Named on the command line, so allowed to write? A line if not. */
static bool asked(void)
{
    if (utest_only)
        return true;
    printf("utest: %s: writes drives: only when named (utest only %s): skipped\n", utest_cur,
           utest_cur);
    return false;
}

/* storage.create_table with no erase confirmation (a blank drive needs none). */
static status_t create_table(handle_t s, uint64_t wait, const uint8_t *disk, const uint8_t *esp,
                             const uint8_t *data)
{
    return storage_create_table_within(s, wait, disk, esp, data, "", 0, "", 0);
}

/* The same with an erase's confirmation: `tail` and `word` as given. */
static status_t create_table_confirmed(handle_t s, const char *tail, const char *word)
{
    return storage_create_table_within(s, WAIT, guid_disk, guid_esp, guid_data, tail,
                                       (uint32_t)strlen(tail), word, (uint32_t)strlen(word));
}

/* The end of the drive's serial number (its last 4 characters), as the
 * owner would type it to confirm an erase; and the same with its last
 * character changed. */
static bool serial_tail(handle_t s, char tail[5], char wrong[5])
{
    char model[41], serial[21], fw[9];
    uint32_t ml, sl, fl;
    CHECK_ST(storage_ident_within(s, WAIT, model, &ml, serial, &sl, fw, &fl), OK);
    CHECK(sl >= 4);
    memcpy(tail, serial + sl - 4, 4);
    tail[4] = '\0';
    memcpy(wrong, tail, 5);
    wrong[3] = wrong[3] == 'x' ? 'y' : 'x';
    return true;
}

/* Is it QEMU's drive, by its serial (tools/qemu-test.sh's)? */
static bool qemu_drive(handle_t s)
{
    char model[41], serial[21], fw[9];
    uint32_t ml, sl, fl;
    if (storage_ident_within(s, WAIT, model, &ml, serial, &sl, fw, &fl) != OK)
        return false;
    static const char want[] = "jamos-nvme";
    if (sl < sizeof(want) - 1)
        return false;
    return !memcmp(serial, want, sizeof(want) - 1);
}

/* What the drive's table lists: its kind, by the names Jam OS's has. */
static enum drive_kind kind_of(struct drive *d)
{
    char vendor[9], product[17], name[109];
    uint32_t vl, pl, mbr_id, nl;
    uint8_t scheme, guid[16], mt, tg[16], ug[16];
    uint64_t start, blocks, attr;
    if (storage_info_within(d->s, WAIT, vendor, &vl, product, &pl, &d->bs, &d->blocks, &scheme,
                            &d->parts, &mbr_id, guid) != OK)
        return DRIVE_OTHER;
    if (scheme == STORAGE_SCHEME_NONE && !d->parts)
        return DRIVE_BLANK;
    if (scheme != STORAGE_SCHEME_GPT || d->parts != 2 ||
        storage_partition_within(d->s, WAIT, 1, &mt, tg, ug, &start, &blocks, &attr, name,
                                 &nl) != OK)
        return DRIVE_OTHER;
    return nl == 11 && !memcmp(name, "Jam OS data", 11) ? DRIVE_JAMOS : DRIVE_OTHER;
}

/* The first QEMU drive of kind `want` (DRIVE_NONE: the n-th QEMU drive,
 * whatever it holds): its storage channel in *d. false: none. */
static bool find(enum drive_kind want, uint32_t nth, struct drive *d)
{
    handle_t dm = devmgr();
    uint32_t seen = 0;
    for (uint32_t n = 0; dm && n < DRIVES_MAX; n++) {
        handle_t s;
        if (devmgrctl_disk_storage_within(dm, WAIT, DEVMGR_KIND_PCI_CLASS, NVME_CLASS, 0, n,
                                          &s) != OK)
            break;
        *d = (struct drive){ .s = s, .n = n };
        if (qemu_drive(s)) {
            d->kind = kind_of(d);
            if (want == DRIVE_NONE ? seen++ == nth : d->kind == want)
                return true;
        }
        jam_handle_close(s);
    }
    return false;
}

static void pattern(uint8_t *p, uint32_t bs, uint32_t part, uint64_t block)
{
    static const char magic[8] = "JAMOSW1";
    memcpy(p, magic, 8);
    for (int i = 0; i < 4; i++) {
        p[8 + i] = (uint8_t)(part >> 8 * i);
        p[12 + i] = (uint8_t)(block >> 8 * i);
    }
    for (uint32_t k = 16; k < bs; k++)
        p[k] = (uint8_t)(block * 7 + k + part * 13);
}

/* The runs nvmew_write writes in a partition of `blocks` blocks. */
static void runs(uint32_t bs, uint64_t blocks, uint64_t lba[3], uint32_t count[3])
{
    lba[0] = 0;
    count[0] = RUN_BLOCKS;
    lba[1] = blocks / 2 / RUN_BLOCKS * RUN_BLOCKS;
    count[1] = MID_BYTES / bs;
    lba[2] = blocks - RUN_BLOCKS;
    count[2] = RUN_BLOCKS;
}


/* Partition `index` opened (read_only as given), a buffer of BUF bytes
 * mapped here: writable on a writable channel, and on a read-only one the
 * driver's handle refuses a writable mapping. */
static bool open_part(handle_t s, uint32_t index, uint8_t read_only, handle_t *blk, uint8_t **map)
{
    handle_t vmo;
    uint32_t size;
    uint64_t addr = 0, bad = 0;
    CHECK_ST(storage_open_partition_within(s, WAIT, index, read_only, blk), OK);
    CHECK_ST(block_map_buffer_within(*blk, WAIT, BUF, &vmo, &size), OK);
    CHECK_EQ(size, BUF);
    handle_t self = startup_handle(SR_SELF_VMAR);
    if (read_only)
        CHECK_ST(jam_vmar_map(self, vmo, 0, BUF, VMAR_READ | VMAR_WRITE, &bad), ERR_ACCESS_DENIED);
    uint32_t flags = read_only ? VMAR_READ : VMAR_READ | VMAR_WRITE;
    CHECK_ST(jam_vmar_map(self, vmo, 0, BUF, flags, &addr), OK);
    jam_handle_close(vmo);   /* the mapping keeps it */
    *map = (uint8_t *)(uintptr_t)addr;
    return true;
}

static void unmap(const uint8_t *map)
{
    (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)map, BUF);
}

/* ---- the drive that isn't Jam OS's --------------------------------------------------------- */

bool t_nvmew_refuse(void)
{
    struct drive d;
    if (!asked())
        return true;
    if (!find(DRIVE_OTHER, 0, &d)) {
        printf("utest: %s: no QEMU drive of someone else's: skipped\n", utest_cur);
        return true;
    }
    handle_t blk;
    /* Writable: never, whatever the index (one past the last too: the
     * drive's kind is looked at first). */
    for (uint32_t i = 0; i <= d.parts; i++)
        CHECK_ST(storage_open_partition_within(d.s, WAIT, i, 0, &blk), ERR_ACCESS_DENIED);
    /* No table made on it, whatever the GUIDs (zeros too: the kind first). */
    CHECK_ST(create_table(d.s, WAIT, guid_disk, guid_esp, guid_data),
             ERR_ACCESS_DENIED);
    CHECK_ST(create_table(d.s, WAIT, guid_zero, guid_zero, guid_zero),
             ERR_ACCESS_DENIED);
    /* Its erase only with the whole confirmation, checked by the driver
     * itself: the serial's end without "erase", "erase" with a wrong end,
     * and the word in another case are each refused (volumes is out of the
     * way here). The right pair is the `erase` case's, through volumes. */
    char tail[5], wrong[5];
    if (!serial_tail(d.s, tail, wrong))
        return false;
    CHECK_ST(create_table_confirmed(d.s, tail, ""), ERR_ACCESS_DENIED);
    CHECK_ST(create_table_confirmed(d.s, wrong, "erase"), ERR_ACCESS_DENIED);
    CHECK_ST(create_table_confirmed(d.s, tail, "ERASE"), ERR_ACCESS_DENIED);
    CHECK_ST(create_table_confirmed(d.s, tail, "erase "), ERR_ACCESS_DENIED);
    CHECK_ST(create_table_confirmed(d.s, "", "erase"), ERR_ACCESS_DENIED);
    uint8_t mode, word_ro, readable;
    CHECK_ST(storage_mode_within(d.s, WAIT, &mode, &word_ro, &readable), OK);
    CHECK(mode == STORAGE_MODE_OTHER && !word_ro && readable);
    /* A read-only channel: a write refused before anything, a sync OK. */
    uint8_t *map;
    if (!open_part(d.s, 0, 1, &blk, &map))
        return false;
    uint32_t bs, max, bufmax;
    uint64_t blocks;
    uint8_t ro;
    uint16_t depth;
    CHECK_ST(block_info_within(blk, WAIT, &bs, &blocks, &ro, &max, &depth, &bufmax), OK);
    CHECK_EQ(ro, 1);
    CHECK_ST(block_write_within(blk, WAIT, 0, 1, 0), ERR_ACCESS_DENIED);
    CHECK_ST(block_write_within(blk, WAIT, blocks, 1, 0), ERR_ACCESS_DENIED);
    CHECK_ST(block_write_within(blk, WAIT, 0, 0, 3), ERR_ACCESS_DENIED);
    CHECK_ST(block_sync_within(blk, WAIT), OK);
    CHECK_ST(block_read_within(blk, WAIT, 0, 1, 0), OK);   /* reads still work */
    unmap(map);
    jam_handle_close(blk);
    printf("utest: %s: drive %u (someone else's, %u partitions): every write refused\n",
           utest_cur, d.n, d.parts);
    jam_handle_close(d.s);
    return true;
}

/* ---- the blank drive made Jam OS's ---------------------------------------------------------- */

/* The table create_table made: Jam OS's two partitions, the ESP's 512 MiB
 * from 1 MiB, the data partition after it, both named. */
static bool jamos_listed(const struct drive *d)
{
    static const char *const names[2] = { "Jam OS ESP", "Jam OS data" };
    char vendor[9], product[17], name[109];
    uint32_t vl, pl, mbr_id, nl, bs, parts;
    uint8_t scheme, guid[16], mt, tg[16], ug[16];
    uint64_t blocks, start[2], len[2], attr;
    CHECK_ST(storage_info_within(d->s, WAIT, vendor, &vl, product, &pl, &bs, &blocks, &scheme,
                                 &parts, &mbr_id, guid), OK);
    CHECK_EQ(scheme, STORAGE_SCHEME_GPT);
    CHECK_EQ(parts, 2);
    CHECK(!memcmp(guid, guid_disk, 16));
    for (uint32_t i = 0; i < 2; i++) {
        CHECK_ST(storage_partition_within(d->s, WAIT, i, &mt, tg, ug, &start[i], &len[i], &attr,
                                          name, &nl), OK);
        CHECK(nl == strlen(names[i]) && !memcmp(name, names[i], nl));
        CHECK(!memcmp(ug, i ? guid_data : guid_esp, 16));
    }
    CHECK_EQ(start[0] * bs, 1ull << 20);
    CHECK_EQ(len[0] * bs, 512ull << 20);
    CHECK_EQ(start[1], start[0] + len[0]);
    CHECK((start[1] + len[1]) * bs % (1ull << 20) == 0);
    printf("utest: %s: drive %u is Jam OS's now: ESP lba %lu + %lu, data lba %lu + %lu\n",
           utest_cur, d->n, (unsigned long)start[0], (unsigned long)len[0],
           (unsigned long)start[1], (unsigned long)len[1]);
    return true;
}

bool t_nvmew_prepare(void)
{
    struct drive d;
    if (!asked())
        return true;
    if (find(DRIVE_JAMOS, 0, &d)) {   /* a later boot: made already, never again */
        CHECK_ST(create_table(d.s, WAIT, guid_disk, guid_esp, guid_data),
                 ERR_ACCESS_DENIED);
        printf("utest: %s: drive %u is Jam OS's already: create_table refused\n", utest_cur,
               d.n);
        jam_handle_close(d.s);
        return true;
    }
    if (!find(DRIVE_BLANK, 0, &d)) {
        printf("utest: %s: no blank QEMU drive: skipped\n", utest_cur);
        return true;
    }
    handle_t blk;
    CHECK_ST(storage_open_partition_within(d.s, WAIT, 0, 0, &blk), ERR_ACCESS_DENIED);
    CHECK_ST(create_table(d.s, WAIT, guid_zero, guid_esp, guid_data),
             ERR_INVALID_ARGS);
    CHECK_ST(create_table(d.s, WAIT, guid_disk, guid_esp, guid_esp),
             ERR_INVALID_ARGS);
    uint64_t t0 = now();
    CHECK_ST(create_table(d.s, 6 * WAIT, guid_disk, guid_esp, guid_data), OK);
    printf("utest: %s: create_table answered in %lu ms\n", utest_cur,
           (unsigned long)((now() - t0) / NS_PER_MS));
    if (!jamos_listed(&d))
        return false;
    CHECK_ST(create_table(d.s, WAIT, guid_disk, guid_esp, guid_data),
             ERR_ACCESS_DENIED);
    jam_handle_close(d.s);
    return true;
}

/* ---- the Jam OS drive written and read back --------------------------------------------------- */

/* Compare partition `part`'s runs, read through a read-only channel, with
 * the pattern: how many blocks hold it (*good) and are all zeros (*zero). */
static bool read_runs(handle_t s, uint32_t part, uint32_t *good, uint32_t *zero, uint32_t *total)
{
    handle_t blk;
    uint8_t *map;
    uint32_t bs, max, bufmax;
    uint64_t blocks, lba[3];
    uint32_t count[3];
    uint8_t ro;
    uint16_t depth;
    static uint8_t want[4096];
    if (!open_part(s, part, 1, &blk, &map))
        return false;
    CHECK_ST(block_info_within(blk, WAIT, &bs, &blocks, &ro, &max, &depth, &bufmax), OK);
    runs(bs, blocks, lba, count);
    for (unsigned r = 0; r < 3; r++) {
        CHECK_ST(block_read_within(blk, WAIT, lba[r], count[r], 0), OK);
        for (uint32_t i = 0; i < count[r]; i++) {
            const uint8_t *got = map + (size_t)i * bs;
            pattern(want, bs, part, lba[r] + i);
            *good += !memcmp(got, want, bs);
            uint8_t any = 0;
            for (uint32_t k = 0; k < bs; k++)
                any |= got[k];
            *zero += !any;
            (*total)++;
        }
    }
    unmap(map);
    jam_handle_close(blk);
    return true;
}

bool t_nvmew_check(void)
{
    struct drive d;
    if (!asked())
        return true;
    if (!find(DRIVE_JAMOS, 0, &d)) {
        printf("utest: %s: no Jam OS QEMU drive: skipped\n", utest_cur);
        return true;
    }
    uint32_t good = 0, zero = 0, total = 0;
    for (uint32_t p = 0; p < 2; p++)
        if (!read_runs(d.s, p, &good, &zero, &total))
            return false;
    CHECK(good == total || zero == total);
    printf("utest: %s: drive %u: %s (%u blocks)\n", utest_cur, d.n,
           good == total ? "the blocks written by an earlier boot are there" :
                           "nothing written yet", total);
    jam_handle_close(d.s);
    return true;
}

/* Partition `part` opened writable: the refusals, then the pattern. */
static bool write_part(handle_t s, uint32_t part)
{
    handle_t blk;
    uint8_t *map;
    uint32_t bs, max, bufmax;
    uint64_t blocks, lba[3];
    uint32_t count[3];
    uint8_t ro;
    uint16_t depth;
    if (!open_part(s, part, 0, &blk, &map))
        return false;
    CHECK_ST(block_info_within(blk, WAIT, &bs, &blocks, &ro, &max, &depth, &bufmax), OK);
    CHECK_EQ(ro, 0);
    /* Refused: past the partition (also wrapping), no blocks, past the
     * buffer, an offset not a multiple of 4. */
    CHECK_ST(block_write_within(blk, WAIT, blocks, 1, 0), ERR_OUT_OF_RANGE);
    CHECK_ST(block_write_within(blk, WAIT, blocks - 1, 2, 0), ERR_OUT_OF_RANGE);
    CHECK_ST(block_write_within(blk, WAIT, ~0ull, 2, 0), ERR_OUT_OF_RANGE);
    CHECK_ST(block_write_within(blk, WAIT, 0, 0, 0), ERR_INVALID_ARGS);
    CHECK_ST(block_write_within(blk, WAIT, 0, 1, 2), ERR_INVALID_ARGS);
    CHECK_ST(block_write_within(blk, WAIT, 0, BUF / bs + 1, 0), ERR_OUT_OF_RANGE);
    runs(bs, blocks, lba, count);
    for (unsigned r = 0; r < 3; r++) {
        for (uint32_t i = 0; i < count[r]; i++)
            pattern(map + (size_t)i * bs, bs, part, lba[r] + i);
        CHECK_ST(block_write_within(blk, WAIT, lba[r], count[r], 0), OK);
    }
    CHECK_ST(block_sync_within(blk, WAIT), OK);
    unmap(map);
    jam_handle_close(blk);
    return true;
}

bool t_nvmew_write(void)
{
    struct drive d;
    if (!asked())
        return true;
    if (!find(DRIVE_JAMOS, 0, &d)) {
        printf("utest: %s: no Jam OS QEMU drive: skipped\n", utest_cur);
        return true;
    }
    uint32_t good = 0, zero = 0, total = 0;
    for (uint32_t p = 0; p < 2; p++)
        if (!write_part(d.s, p) || !read_runs(d.s, p, &good, &zero, &total))
            return false;
    CHECK_EQ(good, total);
    printf("utest: %s: drive %u: %u blocks written in both partitions and read back\n",
           utest_cur, d.n, total);
    jam_handle_close(d.s);
    return true;
}

/* ---- diskro ------------------------------------------------------------------------------- */

bool t_nvmero_refuse(void)
{
    struct drive d;
    if (!asked())
        return true;
    uint32_t n = 0;
    for (; find(DRIVE_NONE, n, &d); n++) {
        handle_t blk;
        for (uint32_t i = 0; i <= d.parts; i++)
            CHECK_ST(storage_open_partition_within(d.s, WAIT, i, 0, &blk), ERR_ACCESS_DENIED);
        CHECK_ST(create_table(d.s, WAIT, guid_disk, guid_esp, guid_data),
                 ERR_ACCESS_DENIED);
        /* `ro` wins over the owner's confirmation of an erase too. */
        char tail[5], wrong[5];
        if (!serial_tail(d.s, tail, wrong))
            return false;
        CHECK_ST(create_table_confirmed(d.s, tail, "erase"), ERR_ACCESS_DENIED);
        uint8_t mode, ro, readable;
        CHECK_ST(storage_mode_within(d.s, WAIT, &mode, &ro, &readable), OK);
        CHECK(mode == STORAGE_MODE_OTHER && ro);
        static const char *const kinds[] = { "?", "someone else's", "blank", "Jam OS's" };
        printf("utest: %s: drive %u (%s): read-only\n", utest_cur, d.n, kinds[d.kind]);
        jam_handle_close(d.s);
    }
    CHECK(n > 0);
    return true;
}
