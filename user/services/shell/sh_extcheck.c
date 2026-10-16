/* disk check <mount>: a read-only check of an ext volume (docs/M13.6-PLAN.md,
 * Q10), by the library's checker (user/services/extfs/ext/check.c: the same
 * code as `jamext check` on the Mac). It reports; it never repairs (repairs
 * are Linux's e2fsck) and never writes.
 *
 * The volume is read through its own `block` channel, asked of volumes with
 * voldisks.open_partition and opened read-only (the disk refuses every
 * write on it), whether the volume is mounted or not. A mounted volume is
 * synced first (fs.sync), and is read as it lies on the disk: extfs commits a
 * request to its journal and writes the blocks home before it answers, so
 * on a quiet volume that is the whole truth; on one in use the check can
 * see a transaction half home. A problem found is therefore looked for
 * once more a second later, and said only if it is still there.
 *
 * Memory is the volume's: about 13 bytes an inode, a bit a block, and the
 * largest directory's names (ext_check.h); a drive too big for it says so. */
#include <idl/block.h>
#include <idl/volumes.h>
#include <ext.h>
#include <ext_check.h>
#include "sh.h"

#define LIST_WAIT   (5 * NS_PER_S)
#define BLOCK_WAIT  (30 * NS_PER_S)
#define BUF_WANT    (256u << 10)
#define CACHE_BYTES (4u << 20)
#define MAX_MOUNTS  16
#define LINES_MAX   (EXT_CHECK_MAX_PROBLEMS + 2)
#define LINE_BYTES  200

/* ---- the volume as a device -------------------------------------------------------- */

struct dev {
    handle_t block;
    uint8_t *buf;
    uint32_t buf_size, sector, max_sectors;
    uint64_t sectors;
    bool     failed;
};

static status_t dev_read(void *ctx, uint64_t off, uint32_t len, void *out)
{
    struct dev *d = ctx;
    uint8_t *p = out;
    uint32_t per = d->buf_size / d->sector;
    if (per > d->max_sectors)
        per = d->max_sectors;
    while (len) {
        uint64_t lba = off / d->sector;
        uint32_t skip = (uint32_t)(off % d->sector);
        uint64_t want = ((uint64_t)skip + len + d->sector - 1) / d->sector;
        uint32_t count = want > per ? per : (uint32_t)want;
        if (lba >= d->sectors || count > d->sectors - lba)
            return ERR_OUT_OF_RANGE;
        status_t st = block_read_within(d->block, BLOCK_WAIT, lba, count, 0);
        if (st != OK) {
            d->failed = true;
            return ERR_IO;
        }
        uint32_t n = count * d->sector - skip;
        if (n > len)
            n = len;
        memcpy(p, d->buf + skip, n);
        p += n;
        off += n;
        len -= n;
    }
    return OK;
}

/* Open the channel as a device: its buffer mapped. */
static status_t dev_open(struct dev *d, handle_t block, struct ext_dev *out)
{
    uint32_t bs = 0, size = 0, maxb = 0;
    uint8_t ro = 0;
    handle_t vmo = HANDLE_INVALID;
    memset(d, 0, sizeof(*d));
    status_t st = block_info_within(block, BLOCK_WAIT, &bs, &d->sectors, &ro, &maxb, NULL, NULL);
    if (st == OK && (bs < 512 || bs > 4096 || (bs & (bs - 1))))
        st = ERR_NOT_SUPPORTED;
    if (st == OK)
        st = block_map_buffer_within(block, BLOCK_WAIT, BUF_WANT, &vmo, &size);
    if (st != OK)
        return st;
    size -= size % PAGE_SIZE;
    uint64_t addr = 0;
    st = size >= bs ? jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, size, VMAR_READ, &addr)
                    : ERR_INVALID_ARGS;
    jam_handle_close(vmo);
    if (st != OK)
        return st;
    d->block = block;
    d->buf = (uint8_t *)(uintptr_t)addr;
    d->buf_size = size;
    d->sector = bs;
    d->max_sectors = maxb ? maxb : size / bs;
    *out = (struct ext_dev){ .ctx = d, .bytes = d->sectors * bs, .read = dev_read };
    return OK;
}

static void dev_close(struct dev *d)
{
    if (d->buf)
        (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)d->buf,
                             d->buf_size);
    jam_handle_close(d->block);
    memset(d, 0, sizeof(*d));
}

/* ---- the checker's calls ----------------------------------------------------------- */

struct pass {
    char     lines[LINES_MAX][LINE_BYTES];
    unsigned n;
};

static void *pass_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return calloc(1, n);
}

static void pass_release(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

static void pass_report(void *ctx, const char *fmt, va_list ap)
{
    struct pass *p = ctx;
    if (p->n < LINES_MAX)
        vsnprintf(p->lines[p->n++], LINE_BYTES, fmt, ap);
}

static bool pass_cancel(void *ctx)
{
    (void)ctx;
    return sh_interrupted();
}

/* One mount and check of the volume on `block` (a new channel for each). */
static status_t run_pass(handle_t block, struct pass *p, struct ext_check_result *res,
                         uint32_t *block_size, uint64_t *blocks, bool *is_ext)
{
    struct dev d;
    struct ext_dev dev;
    status_t st = dev_open(&d, block, &dev);
    if (st != OK) {
        jam_handle_close(block);
        return st;
    }
    void *cache = malloc(CACHE_BYTES);
    struct ext_fs *fs = malloc(sizeof(*fs));
    if (!cache || !fs) {
        free(cache);
        free(fs);
        dev_close(&d);
        return ERR_NO_MEMORY;
    }
    memset(fs, 0, sizeof(*fs));
    st = ext_mount(fs, &dev, cache, CACHE_BYTES, EXT_MOUNT_READ_ONLY);
    *is_ext = st != ERR_NOT_SUPPORTED || fs->refused[0] || fs->refused[1] || fs->refused[2];
    if (st == OK) {
        *block_size = fs->block_size;
        *blocks = fs->blocks;
        struct ext_check_io io = { .ctx = p, .alloc = pass_alloc, .release = pass_release,
                                   .report = pass_report, .cancel = pass_cancel };
        p->n = 0;
        st = ext_check(fs, &io, res);
    } else if (st == ERR_IO && !d.failed) {
        p->n = 0;
        snprintf(p->lines[p->n++], LINE_BYTES, "can't be mounted: %s", fs->why ? fs->why : "?");
        res->problems = 1;
        st = OK;
    } else if (st == ERR_NOT_SUPPORTED && *is_ext) {
        p->n = 0;
        snprintf(p->lines[p->n++], LINE_BYTES, "has features Jam OS does not read (%s)",
                 fs->why ? fs->why : "?");
        res->problems = 1;
        st = OK;
    }
    free(fs);
    free(cache);
    dev_close(&d);
    return st;
}

/* ---- finding the volume ------------------------------------------------------------ */

static status_t find_mount(handle_t list, const char *mount, uint32_t *id, uint32_t *part)
{
    for (uint32_t i = 0;; i++) {
        uint32_t did, bs, parts, nl, ml, sl, fl;
        uint64_t blocks;
        uint8_t bus, ready, kind, scheme, ro;
        char name[9], model[41], serial[21], firmware[9];
        status_t st = voldisks_disk_until(list, now() + LIST_WAIT, i, &did, &bus, &ready, &kind,
                                          &scheme, &ro, &bs, &blocks, &parts, name, &nl, model,
                                          &ml, serial, &sl, firmware, &fl);
        if (st == ERR_OUT_OF_RANGE)
            return ERR_NOT_FOUND;
        if (st != OK)
            return st;
        for (uint32_t n = 0; n < parts && n < MAX_MOUNTS; n++) {
            uint8_t mbr, guid[16], fs;
            uint64_t start, pblocks;
            char label[109], where[16];
            uint32_t ll, wl;
            if (voldisks_partition_until(list, now() + LIST_WAIT, did, n, &mbr, guid, &start,
                                         &pblocks, &fs, label, &ll, where, &wl) == OK &&
                fs == VOLDISKS_FS_MOUNTED && wl == strlen(mount) && !memcmp(where, mount, wl)) {
                *id = did;
                *part = n;
                return OK;
            }
        }
    }
}

int sh_disk_check(handle_t list, const char *arg)
{
    char mount[NS_NAME_MAX];
    if (!sh_resolve(arg, mount, sizeof(mount)) || !sh_is_mount(mount) || !mount[1]) {
        sh_tty("disk check: %s: give a mount (/jam, /usb0, /nvme1; `disks` lists them)\n", arg);
        return 2;
    }
    uint32_t id = 0, part = 0;
    status_t st = find_mount(list, mount, &id, &part);
    if (st != OK) {
        sh_tty("disk check: %s: %s\n", mount, st == ERR_NOT_FOUND ? "no disk has a partition "
               "mounted there (`disks` shows them)" : sh_why(st));
        return 1;
    }
    (void)fs_sync(mount);   /* what extfs holds is on the disk */
    static struct pass first, second;
    struct ext_check_result res = { 0 }, res2 = { 0 };
    uint32_t bs = 0;
    uint64_t blocks = 0;
    bool is_ext = true;
    for (int round = 0; round < 2; round++) {
        struct pass *p = round ? &second : &first;
        struct ext_check_result *r = round ? &res2 : &res;
        handle_t block;
        st = voldisks_open_partition_until(list, now() + LIST_WAIT + BLOCK_WAIT, id, part, &block);
        if (st == OK)
            st = run_pass(block, p, r, &bs, &blocks, &is_ext);
        if (st == ERR_NOT_SUPPORTED && !is_ext) {
            sh_tty("disk check: %s: not an ext volume (FAT is checked on the Mac)\n", mount);
            return 1;
        }
        if (st == ERR_CANCELED) {
            sh_tty("disk check: %s: interrupted\n", mount);
            return 1;
        }
        if (st != OK) {
            sh_tty("disk check: %s: %s\n", mount, st == ERR_NO_MEMORY ?
                   "not enough memory for this volume's check (a smaller one, or less running)" :
                   sh_why(st));
            return 1;
        }
        if (!r->problems)
            break;
        if (round == 0 && !sh_sleep(NS_PER_S))   /* a volume in use: look again before saying */
            return 130;
    }
    if (second.n || res2.problems) {
        for (unsigned i = 0; i < second.n; i++)
            sh_say("disk check: %s: %s\n", mount, second.lines[i]);
        sh_say("disk check: %s: %u problem%s%s (e2fsck on Linux repairs; nothing was changed)\n",
               mount, res2.problems, res2.problems == 1 ? "" : "s",
               res2.stopped ? ", the check stopped there" : "");
        return 1;
    }
    char size[24];
    sh_say("disk check: %s: no problems%s (%u inodes in use, %u directories, %u names; %s)\n",
           mount, res.problems ? " now (the first look saw changes in flight)" : "", res.inodes,
           res.dirs, res.names, sh_human((uint64_t)blocks * bs, size, sizeof(size)));
    return 0;
}
