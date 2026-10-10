/* disks, disk: the disks Jam OS sees, and what can be asked of one whole
 * (M12.7 stage 1, docs/M12.7-PLAN.md "volumes and the shell").
 *
 *   disks                       every disk: its drive, size, partition table, and each
 *                               partition: type, size, name, where it is mounted
 *   disk digest <disk> [<lba> <count>]
 *                               SHA-256s of regions of the disk, computed in its driver
 *                               (the data never leaves it); without a range, the region
 *                               list tools/disk-regions.py computes on Linux from the
 *                               partition table alone, one line each, in its order
 *   disk smart <disk>           the drive's health log
 *
 * The shell asks init for a channel that speaks voldisks (initctl.disks,
 * abi/idl/volumes.idl): volumes answers the list from what its drivers said
 * and asks a driver for a digest or the health log each time. That channel
 * can't mount, remount or kill anything. A disk is named as `disks` names
 * it: nvme0, usb0, ... */
#include <idl/initctl.h>
#include <idl/storage.h>
#include <idl/volumes.h>
#include "sh.h"

#define LIST_WAIT   (5 * NS_PER_S)
#define DIGEST_WAIT (90 * NS_PER_S)   /* volumes gives the driver 60 s for one */
#define MAX_PARTS   16
#define MiB         (1ull << 20)
#define GiB         (1ull << 30)

/* A new voldisks channel from init (the caller closes it). */
static status_t open_list(handle_t *out)
{
    if (!sh_initctl()) {
        sh_tty("disks: no init to ask\n");   /* a shell init didn't start */
        return ERR_NOT_FOUND;
    }
    status_t st = initctl_disks_until(sh_initctl(), now() + LIST_WAIT, out);
    if (st != OK)
        sh_tty("disks: volumes doesn't answer (%s)\n", status_str(st));
    return st;
}

/* ---- names ------------------------------------------------------------------------- */

struct guid_name {
    uint8_t     g[16];   /* the bytes on disk */
    const char *name;
};

static const struct guid_name guid_names[] = {
    { { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e,
        0xc9, 0x3b }, "EFI system" },
    { { 0x16, 0xe3, 0xc9, 0xe3, 0x5c, 0x0b, 0xb8, 0x4d, 0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02,
        0x15, 0xae }, "Microsoft reserved" },
    { { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26,
        0x99, 0xc7 }, "Microsoft basic data" },
    { { 0xa4, 0xbb, 0x94, 0xde, 0xd1, 0x06, 0x40, 0x4d, 0xa1, 0x6a, 0xbf, 0xd5, 0x01, 0x79,
        0xd6, 0xac }, "Windows recovery" },
    { { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x93, 0x00, 0x8e, 0x7f, 0x4e, 0x1d,
        0x28, 0x48 }, "Linux filesystem" },
    { { 0x65, 0xe5, 0xba, 0x71, 0xa8, 0xd4, 0x15, 0x43, 0x8c, 0x81, 0x9b, 0x35, 0x46, 0x2c,
        0xc1, 0x58 }, "Jam OS data" },
};

/* The printed form of a GUID's 16 bytes on disk (37 bytes with the NUL). */
static void guid_str(const uint8_t *g, char *out)
{
    snprintf(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12],
             g[13], g[14], g[15]);
}

static bool guid_zero(const uint8_t *g)
{
    for (unsigned i = 0; i < 16; i++)
        if (g[i])
            return false;
    return true;
}

/* A partition's type in words: a GPT type GUID, else an MBR type byte. */
static const char *type_name(uint8_t scheme, uint8_t mbr, const uint8_t *guid, char *buf,
                             size_t cap)
{
    if (scheme == STORAGE_SCHEME_GPT || !guid_zero(guid)) {
        for (unsigned i = 0; i < sizeof(guid_names) / sizeof(guid_names[0]); i++)
            if (!memcmp(guid_names[i].g, guid, 16))
                return guid_names[i].name;
        char g[37];
        guid_str(guid, g);
        snprintf(buf, cap, "%s", g);
        return buf;
    }
    switch (mbr) {
    case 0x00: return "whole disk";
    case 0x01: case 0x04: case 0x06: return "FAT";
    case 0x0b: case 0x0c: return "FAT32";
    case 0x0e: return "FAT16 (LBA)";
    case 0x07: return "NTFS or exFAT";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xee: return "GPT protective";
    case 0xef: return "EFI system";
    default:
        snprintf(buf, cap, "type 0x%02x", mbr);
        return buf;
    }
}

static const char *fs_note(uint8_t fs, uint32_t block_size, const char *mount, char *buf,
                           size_t cap)
{
    switch (fs) {
    case VOLDISKS_FS_MOUNTED: return mount;
    case VOLDISKS_FS_WAITING: return "(starting)";
    case VOLDISKS_FS_NO_FAT: return "(no FAT volume)";
    case VOLDISKS_FS_BLOCK_SIZE:
        snprintf(buf, cap, "(no FAT: %u-byte blocks)", block_size);
        return buf;
    default: return "";
    }
}

/* ---- disks ------------------------------------------------------------------------- */

struct disk_row {
    uint32_t id, block_size, partitions;
    uint64_t blocks;
    uint8_t  bus, ready, kind, scheme, read_only;
    char     name[9], model[41], serial[21], firmware[9];
};

static status_t get_disk(handle_t list, uint32_t index, struct disk_row *r)
{
    uint32_t nl, ml, sl, fl;
    status_t st = voldisks_disk_until(list, now() + LIST_WAIT, index, &r->id, &r->bus, &r->ready,
                                      &r->kind, &r->scheme, &r->read_only, &r->block_size,
                                      &r->blocks, &r->partitions, r->name, &nl, r->model, &ml,
                                      r->serial, &sl, r->firmware, &fl);
    return st;
}

static void say_partition(handle_t list, const struct disk_row *r, unsigned n)
{
    uint8_t mbr, guid[16], fs;
    uint64_t start, blocks;
    char label[109], mount[16], tbuf[40], fbuf[40], size[24];
    uint32_t ll, ml;
    status_t st = voldisks_partition_until(list, now() + LIST_WAIT, r->id, n, &mbr, guid, &start,
                                           &blocks, &fs, label, &ll, mount, &ml);
    if (st != OK) {
        sh_say("  %2u  (%s)\n", n + 1, status_str(st));
        return;
    }
    sh_say("  %2u  %-21s %9s  %-30s %s\n", n + 1,
           type_name(r->scheme, mbr, guid, tbuf, sizeof(tbuf)),
           sh_human(blocks * r->block_size, size, sizeof(size)),
           label[0] ? label : "-", fs_note(fs, r->block_size, mount, fbuf, sizeof(fbuf)));
}

static void say_disk(handle_t list, const struct disk_row *r)
{
    static const char *const schemes[] = { "no partition table", "MBR", "GPT" };
    char size[24];
    if (!r->ready) {
        sh_say("%-6s (its driver has not answered)\n", r->name);
        return;
    }
    sh_say("%-6s %s%s%s%s%s  %s  %s%s%s%s\n", r->name, r->model[0] ? r->model : "(unnamed)",
           r->serial[0] ? "  serial " : "", r->serial, r->firmware[0] ? "  fw " : "",
           r->firmware, sh_human(r->blocks * r->block_size, size, sizeof(size)),
           r->scheme < 3 ? schemes[r->scheme] : "?",
           r->kind == VOLDISKS_KIND_JAMOS ? ", Jam OS disk" : "",
           r->kind == VOLDISKS_KIND_NO_TABLE ? ", nothing found on it" : "",
           r->read_only ? ", read-only" : "");
    for (unsigned n = 0; n < r->partitions && n < MAX_PARTS; n++)
        say_partition(list, r, n);
    if (r->partitions > MAX_PARTS)
        sh_say("      (%u more not listed)\n", r->partitions - MAX_PARTS);
}

SH_CMD(disks)
{
    (void)argv;
    if (argc != 1) {
        sh_tty("usage: disks\n");
        return 2;
    }
    handle_t list;
    if (open_list(&list) != OK)
        return 1;
    int status = 0;
    unsigned shown = 0;
    for (uint32_t i = 0;; i++) {
        struct disk_row r;
        status_t st = get_disk(list, i, &r);
        if (st == ERR_OUT_OF_RANGE)
            break;
        if (st != OK) {
            sh_tty("disks: %s\n", status_str(st));
            status = 1;
            break;
        }
        say_disk(list, &r);
        shown++;
    }
    if (!shown && !status)
        sh_say("no disks\n");
    jam_handle_close(list);
    return status;
}

/* ---- disk digest and smart -------------------------------------------------------- */

static void hex(const uint8_t *b, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) {
        out[2 * i] = digits[b[i] >> 4];
        out[2 * i + 1] = digits[b[i] & 15];
    }
    out[64] = '\0';
}

struct digest_run {
    handle_t           list;
    const struct disk_row *disk;
    status_t           st;    /* the first failure (OK: none so far) */
};

/* One region: hashed in the driver, its line shown (and written to the boot
 * log, so the line can be read from /data/logs; in a form of its own, so
 * it is not mistaken for the line on the screen). False: stop. */
static bool region(struct digest_run *run, const char *name, uint64_t lba, uint64_t count)
{
    uint8_t sha[32];
    char h[65];
    if (sh_interrupted()) {
        run->st = ERR_CANCELED;
        return false;
    }
    run->st = voldisks_digest_until(run->list, now() + DIGEST_WAIT, run->disk->id, lba, count,
                                    sha);
    if (run->st != OK) {
        if (run->st == ERR_NOT_SUPPORTED)
            sh_tty("disk digest: %s's driver can't hash a disk (only an NVMe drive's can)\n",
                   run->disk->name);
        else
            sh_tty("disk digest: %s: lba %lu count %lu: %s\n", name, (unsigned long)lba,
                   (unsigned long)count, status_str(run->st));
        return false;
    }
    hex(sha, h);
    sh_say("%s lba %lu count %lu sha256 %s\n", name, (unsigned long)lba, (unsigned long)count, h);
    printf("disk digest: %s %s %lu+%lu %s\n", run->disk->name, name, (unsigned long)lba,
           (unsigned long)count, h);
    sh_flush();
    return true;
}

/* A big partition's head, tail and 32 samples; a small one whole. The
 * list tools/disk-regions.py makes, in its order and with its names. */
static bool partition_regions(struct digest_run *run, unsigned n, uint64_t start, uint64_t count)
{
    const uint32_t bs = run->disk->block_size;
    char name[24];
    snprintf(name, sizeof(name), "part%u", n);
    if (count < GiB / bs)
        return region(run, name, start, count);
    uint64_t s64 = 64 * MiB / bs, s1 = MiB / bs;
    char sub[24];
    snprintf(sub, sizeof(sub), "%s-head", name);
    if (!region(run, sub, start, s64))
        return false;
    snprintf(sub, sizeof(sub), "%s-tail", name);
    if (!region(run, sub, start + count - s64, s64))
        return false;
    for (unsigned k = 0; k < 32; k++) {
        snprintf(sub, sizeof(sub), "%s-s%02u", name, k);
        if (!region(run, sub, start + k * (count / 32), s1))
            return false;
    }
    return true;
}

static int digest_regions(struct digest_run *run)
{
    const struct disk_row *d = run->disk;
    if (d->blocks >= 34 && !region(run, "gpt-head", 0, 34))
        return 1;
    for (unsigned n = 0; n < d->partitions && n < MAX_PARTS && d->scheme != 0; n++) {
        uint8_t mbr, guid[16], fs;
        uint64_t start, blocks;
        char label[109], mount[16];
        uint32_t ll, ml;
        run->st = voldisks_partition_until(run->list, now() + LIST_WAIT, d->id, n, &mbr, guid,
                                           &start, &blocks, &fs, label, &ll, mount, &ml);
        if (run->st != OK) {
            sh_tty("disk digest: partition %u: %s\n", n + 1, status_str(run->st));
            return 1;
        }
        if (!partition_regions(run, n + 1, start, blocks))
            return 1;
    }
    if (d->blocks >= 34 && !region(run, "gpt-tail", d->blocks - 33, 33))
        return 1;
    return 0;
}

/* The disk called name (a `disks` name) into *out. */
static bool find_disk(handle_t list, const char *name, struct disk_row *out)
{
    for (uint32_t i = 0;; i++) {
        status_t st = get_disk(list, i, out);
        if (st != OK) {
            sh_tty("disk: no disk called %s (`disks` lists them)\n", name);
            return false;
        }
        if (!strcmp(out->name, name))
            break;
    }
    if (!out->ready || !out->block_size) {
        sh_tty("disk: %s has not answered yet\n", name);
        return false;
    }
    return true;
}

static int digest_cmd(handle_t list, const struct disk_row *d, int argc, char **argv)
{
    struct digest_run run = { .list = list, .disk = d };
    uint64_t lba, count;
    if (argc == 5) {
        if (!sh_parse_u64(argv[3], &lba) || !sh_parse_u64(argv[4], &count)) {
            sh_tty("disk digest: <lba> and <count> are numbers of blocks\n");
            return 2;
        }
        return region(&run, "range", lba, count) ? 0 : 1;
    }
    int rc = digest_regions(&run);
    if (rc == 0)
        sh_say("disk digest: done\n");
    return rc;
}

static void count_line(const char *what, uint64_t n, const char *unit)
{
    sh_say("  %-20s %lu%s\n", what, (unsigned long)n, unit);
}

static int smart_cmd(handle_t list, const struct disk_row *d)
{
    uint64_t ur, uw, hr, hw, pc, us;
    uint8_t used, crit;
    status_t st = voldisks_health_until(list, now() + LIST_WAIT, d->id, &ur, &uw, &hr, &hw, &pc,
                                        &us, &used, &crit);
    if (st != OK) {
        sh_tty("disk smart: %s: %s\n", d->name, st == ERR_NOT_SUPPORTED ? "not an NVMe drive"
                                                                           : status_str(st));
        return 1;
    }
    char a[24], b[24];
    sh_say("%s health: critical warning 0x%02x, %u%% of its life used\n", d->name, crit, used);
    sh_say("  %-20s %lu (%s)\n", "data units read", (unsigned long)ur,
           sh_human(ur * 512000, a, sizeof(a)));
    sh_say("  %-20s %lu (%s)\n", "data units written", (unsigned long)uw,
           sh_human(uw * 512000, b, sizeof(b)));
    count_line("host read commands", hr, "");
    count_line("host write commands", hw, "");
    count_line("power cycles", pc, "");
    count_line("unsafe shutdowns", us, "");
    printf("disk smart: %s host reads %lu host writes %lu power cycles %lu\n", d->name,
           (unsigned long)hr, (unsigned long)hw, (unsigned long)pc);
    return 0;
}

SH_CMD(disk)
{
    bool digest = argc >= 3 && !strcmp(argv[1], "digest") && (argc == 3 || argc == 5);
    bool smart = argc == 3 && !strcmp(argv[1], "smart");
    if (!digest && !smart) {
        sh_tty("usage: disk digest <disk> [<lba> <count>]   SHA-256s of the disk's regions\n"
               "       disk smart <disk>                    the drive's health log\n"
               "  <disk> is a name `disks` shows (nvme0, usb0)\n");
        return 2;
    }
    handle_t list;
    if (open_list(&list) != OK)
        return 1;
    struct disk_row d;
    int rc = 1;
    if (find_disk(list, argv[2], &d))
        rc = smart ? smart_cmd(list, &d) : digest_cmd(list, &d, argc, argv);
    jam_handle_close(list);
    return rc;
}
