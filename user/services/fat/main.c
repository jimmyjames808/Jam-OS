/* fat: the FAT filesystem service. One process serves one partition: it
 * takes a `block` channel (usb-storage's, one partition and nothing else),
 * mounts the FAT volume on it with FatFs and serves the `fs` protocol, plus
 * a `file` channel per open file. devmgr starts one over the boot stick's
 * data partition (/data) and a read-only one over its ESP (/esp).
 * <fatsvc.h> has the startup handles, fat.h the map of the files.
 *
 * Mounting: a volume is mounted as it is, dirty or not (disk.c logs a dirty
 * one). Formatting is off unless fat was started with FAT_ARG_FORMAT
 * (<fatsvc.h>; devmgr gives it to the boot disk's data partition and to
 * nothing else). With it a writable, blank partition is formatted (FAT32
 * when it is big enough for one, label JAMOS-DATA): that is how a freshly
 * flashed stick gets its /data. Nothing else ever is: not a disk that
 * fails, not a partition holding another filesystem or a damaged FAT, not
 * a read-only one, and nothing at all on someone else's stick. format()
 * is reached from one place, mount_blank, behind vol.may_format.
 *
 * A restart: an instance started with a dead one's state (devmgr's
 * SR_STATE) carries on from it instead of mounting (adopt.c); its clients
 * see nothing but a slower answer.
 *
 * Exit: 0 once there is nothing left to serve (the fs channel's client
 * closed it, after every file is closed and the volume settled; or the
 * disk went away, mounted or not yet); 1 when the volume can't be served but might be later
 * (no handles, a disk that doesn't answer); FAT_EXIT_NO_VOLUME when the
 * partition holds no FAT volume fat can serve and nothing was formatted;
 * FAT_EXIT_TEST a test power ended it (test.c). */
#include <idl/fsctl.h>
#include "fat.h"

#define LABEL       "JAMOS-DATA"
#define FORMAT_WORK (64u << 10)   /* f_mkfs's buffer: more sectors per write */
/* A FAT of this many sectors or more is not a FAT volume (FatFs R0.16
 * trusts the field; its patch 2 adds this same bound, CVE-2026-6682). */
#define FAT_SECTORS_MAX 0x200000u

_Static_assert(FAT_SR_SERVE == SR_DRIVER(DR_SERVE), "fat serves on a driver's DR_SERVE role");

struct fat_vol vol;

static const char *type_name(void)
{
    BYTE t = kept->fs.fs_type;
    return t == FS_FAT32 ? "FAT32" : t == FS_FAT16 ? "FAT16" : "FAT12";
}

/* A new volume over the whole partition (no partition table inside it).
 * Its boot sector is written last (disk.c), so a format cut short leaves
 * the partition blank. */
static FRESULT format(void)
{
    static const MKFS_PARM fat32 = { .fmt = FM_FAT32 | FM_SFD, .n_fat = 2 };
    static const MKFS_PARM small = { .fmt = FM_FAT | FM_SFD, .n_fat = 2 };
    void *work = malloc(FORMAT_WORK);
    if (!work)
        return FR_NOT_ENOUGH_CORE;
    printf("fat %s: the partition is blank: formatting it\n", vol.name);
    disk_hold_boot();
    FRESULT fr = f_mkfs("", &fat32, work, FORMAT_WORK);
    if (fr == FR_MKFS_ABORTED)   /* too few clusters for FAT32 */
        fr = f_mkfs("", &small, work, FORMAT_WORK);
    free(work);
    /* Only a format that went through gets its boot sector: after a failed
     * one the partition stays blank, for the next start to format. */
    if (fr != FR_OK)
        disk_drop_boot();
    else if (disk_commit_boot(LABEL) != OK)
        fr = FR_DISK_ERR;
    if (fr == FR_OK)
        fr = f_mount(&kept->fs, "", 1);
    if (fr == FR_OK)
        fr = f_setlabel(LABEL);
    return fr;
}

/* FatFs found no FAT volume. Without FAT_ARG_FORMAT that is the end of
 * it. With it, only a blank partition is formatted: one whose first sector
 * has no boot signature (tools/mbr-grow.py wipes a new data partition's
 * start). A boot sector FatFs can't use is someone's filesystem, another
 * kind or a damaged FAT, and is left alone. ERR_NOT_FOUND: nothing was
 * formatted. */
static status_t mount_blank(void)
{
    bool blank = false;
    if (!vol.may_format) {
        printf("fat %s: no FAT volume (formatting is off: nothing written)\n", vol.name);
        return ERR_NOT_FOUND;
    }
    status_t st = vol.read_only ? OK : disk_is_blank(&blank);
    if (st != OK)
        return st;
    if (!blank) {
        printf("fat %s: no FAT volume, and the partition is %s: not formatting it\n", vol.name,
               vol.read_only ? "read-only" : "not blank (another filesystem, or a damaged one)");
        return ERR_NOT_FOUND;
    }
    FRESULT fr = format();
    if (fr != FR_OK)
        printf("fat %s: formatting failed: FatFs error %d\n", vol.name, (int)fr);
    return fr_status(fr);
}

status_t mount(bool *no_volume)
{
    FRESULT fr = f_mount(&kept->fs, "", 1);
    status_t st = fr == FR_NO_FILESYSTEM ? mount_blank() : fr_status(fr);
    if (st != OK) {
        if (fr != FR_NO_FILESYSTEM)
            printf("fat %s: can't mount: FatFs error %d\n", vol.name, (int)fr);
        *no_volume = fr == FR_NO_FILESYSTEM && st == ERR_NOT_FOUND;
        return st;
    }
    if (kept->fs.fsize >= FAT_SECTORS_MAX) {
        printf("fat %s: a FAT of %u sectors: not a volume to trust, not mounted\n", vol.name,
               (unsigned)kept->fs.fsize);
        *no_volume = true;
        return ERR_IO;
    }
    disk_watch();
    kept->mounted = true;   /* what a successor adopts */
    printf("fat %s: mounted %s, %lu MiB%s\n", vol.name, type_name(),
           (unsigned long)((uint64_t)(kept->fs.n_fatent - 2) * kept->fs.csize / 2048),
           vol.read_only ? ", read-only" : "");
    return OK;
}

/* Wait (ONCE) for the fs channel's next request or its client's close. */
static status_t arm_fs(handle_t serve)
{
    return jam_port_bind(vol.port, serve, FAT_KEY_FS, SIG_READABLE | SIG_PEER_CLOSED,
                         PORT_BIND_ONCE);
}

static bool stopping;   /* fsctl.stop was answered: nothing more is served */

/* fsctl.stop: everything closed and on the medium; run() then ends. */
static status_t op_stop(void *ctx)
{
    (void)ctx;
    files_close_all();
    stopping = true;
    return vol.disk_gone ? ERR_PEER_CLOSED : disk_settle(true);
}

/* fsctl.stats: the counters, as they are. */
static status_t op_stats(void *ctx, uint64_t *out_entries_read, uint64_t *out_cache_hits,
                         uint64_t *out_cache_fills, uint64_t *out_cache_bypassed,
                         uint64_t *out_cache_updated)
{
    (void)ctx;
    struct fat_cache_stats cs;
    cache_stats(&cs);
    *out_entries_read = dirs_entries_read();
    *out_cache_hits = cs.hits;
    *out_cache_fills = cs.fills;
    *out_cache_bypassed = cs.bypassed;
    *out_cache_updated = cs.updated;
    return OK;
}

const struct fsctl_ops fat_ctl_ops = { .stop = op_stop, .stats = op_stats };

/* Serve until the fs channel's client is gone, the disk is, or fsctl.stop
 * was asked (OK), or something fails (its status). Each packet serves one
 * request, and its channel's binding fires again at once if more is
 * queued, so one busy client can't starve the others. Every request is
 * read into a slot of the state and run as an operation (request.c); its
 * reply goes out with the next port wait (fat_wait), in that same system
 * call, or with the next take. */
static status_t run(handle_t serve, handle_t ctl)
{
    const struct fat_chan fs = { .ch = serve, .id = FAT_CHAN_FS, .proto = FAT_PROTO_FS };
    const struct fat_chan fc = { .ch = ctl, .id = FAT_CHAN_CTL, .proto = FAT_PROTO_CTL };
    status_t st = arm_fs(serve);
    if (st == OK)
        st = jam_port_bind(vol.port, vol.block, FAT_KEY_BLOCK, SIG_PEER_CLOSED, PORT_BIND_ONCE);
    /* PERSISTENT: one stop is all it ever serves; its holder going away
     * means nothing (the fs channel's clients decide when fat ends). */
    if (st == OK && ctl)
        st = jam_port_bind(vol.port, ctl, FAT_KEY_CTL, SIG_READABLE, PORT_BIND_PERSISTENT);
    while (st == OK && !vol.disk_gone && !stopping) {
        struct port_packet pkt;
        st = fat_wait(&pkt);
        if (st != OK)
            break;
        if (pkt.key == FAT_KEY_CTL) {
            /* Its binding fires only when a request comes to an empty
             * queue: whatever is queued is served now (devmgr alone holds
             * the other end, a call at a time, so this is short). */
            while (!stopping && !vol.disk_gone && serve_one(&fc) == OK)
                ;
        } else if (pkt.key == FAT_KEY_BLOCK) {
            vol.disk_gone = true;
        } else if (pkt.key & FAT_KEY_FILE_BIT) {
            files_event(pkt.key);
        } else if (pkt.key & FAT_KEY_VIEW_BIT) {
            views_event(pkt.key);
        } else if (pkt.key == FAT_KEY_FS) {
            files_reap();
            st = serve_one(&fs);
            if (st == ERR_PEER_CLOSED)
                return OK;
            if (st == OK || st == ERR_SHOULD_WAIT)
                st = arm_fs(serve);
        }
    }
    return st;
}

/* The words after the name (<fatsvc.h>). */
static void words(int argc, char **argv)
{
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], FAT_ARG_FORMAT))
            vol.may_format = true;
        else if (!strcmp(argv[i], FAT_ARG_KILLED) || !strcmp(argv[i], FAT_ARG_CRASHED))
            vol.ended = argv[i];
        else if (!test_word(argv[i]))
            printf("fat %s: an argument it doesn't know: %s\n", vol.name, argv[i]);
    }
}

int main(int argc, char **argv)
{
    vol.name = argc >= 2 ? argv[1] : "fat";
    words(argc, argv);
    bool no_volume = false, adopted = false;
    handle_t serve = startup_handle(FAT_SR_SERVE), block = startup_handle(FAT_SR_BLOCK);
    if (serve == HANDLE_INVALID || block == HANDLE_INVALID) {
        printf("fat %s: no %s channel: nothing to do\n", vol.name,
               serve == HANDLE_INVALID ? "fs" : "block");
        return 1;
    }
    vol.serve = serve;
    vol.ctl = startup_handle(FAT_SR_CTL);
    vol.keep = startup_handle(SR_KEEP);
    vol.rtc_root = startup_handle(SR_RESOURCE);
    vol.at[FAT_AT_MAIN] = now();
    status_t st = jam_port_create(&vol.port);
    if (st == OK)
        st = disk_open(block);
    vol.at[FAT_AT_DISK] = now();
    if (st == OK)   /* everything fat knows lives there; it is bound to the partition */
        st = state_open(&adopted);
    vol.at[FAT_AT_STATE] = now();
    if (st == OK)
        st = adopt(adopted, &no_volume);
    if (st != OK)
        reply_flush();   /* what adopt answered goes out before fat ends */
    if (st != OK && vol.disk_gone) {
        /* As a disk gone while serving: nothing is left to serve, and
         * nothing failed (devmgr stops usb-bus first at shutdown, which
         * can be while a new disk's fat is still mounting). */
        printf("fat %s: the disk went while mounting: stopping\n", vol.name);
        return 0;
    }
    if (st != OK) {
        printf("fat %s: not serving: %s\n", vol.name, status_str(st));
        return no_volume ? FAT_EXIT_NO_VOLUME : 1;
    }
    st = run(serve, vol.ctl);
    reply_flush();   /* the last request's answer (fsctl.stop's) before anything else */
    files_close_all();
    struct fat_cache_stats cs;
    cache_stats(&cs);
    printf("fat %s: cache: %lu reads from it, %lu lines read, %lu big reads past it, %lu "
           "sectors written through it\n", vol.name, (unsigned long)cs.hits,
           (unsigned long)cs.fills, (unsigned long)cs.bypassed, (unsigned long)cs.updated);
    uint64_t held, held_writes;
    disk_hold_stats(&held, &held_writes);
    if (held > held_writes)   /* some writes went out together (hold.c) */
        printf("fat %s: %lu sectors held back went out in %lu block writes\n", vol.name,
               (unsigned long)held, (unsigned long)held_writes);
    if (vol.disk_gone) {
        printf("fat %s: the disk is gone: stopping\n", vol.name);
        return 0;
    }
    status_t st2 = disk_settle(true);
    if (st == OK)
        st = st2;
    if (st != OK)
        printf("fat %s: stopping: %s\n", vol.name, status_str(st));
    return st == OK ? 0 : 1;
}
