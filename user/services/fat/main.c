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
 * Exit: 0 once there is nothing left to serve (the fs channel's client
 * closed it, after every file is closed and the volume settled; or the
 * disk went away); 1 when the volume can't be served but might be later
 * (no handles, a disk that doesn't answer); FAT_EXIT_NO_VOLUME when the
 * partition holds no FAT volume fat can serve and nothing was formatted. */
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
    return vol.fs.fs_type == FS_FAT32 ? "FAT32" : vol.fs.fs_type == FS_FAT16 ? "FAT16" : "FAT12";
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
    status_t st = disk_commit_boot();
    if (fr == FR_OK && st != OK)
        fr = FR_DISK_ERR;
    if (fr == FR_OK)
        fr = f_mount(&vol.fs, "", 1);
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

/* Mount the volume. *no_volume: the partition holds none fat can serve
 * (the exit code FAT_EXIT_NO_VOLUME). */
static status_t mount(bool *no_volume)
{
    FRESULT fr = f_mount(&vol.fs, "", 1);
    status_t st = fr == FR_NO_FILESYSTEM ? mount_blank() : fr_status(fr);
    if (st != OK) {
        if (fr != FR_NO_FILESYSTEM)
            printf("fat %s: can't mount: FatFs error %d\n", vol.name, (int)fr);
        *no_volume = fr == FR_NO_FILESYSTEM && st == ERR_NOT_FOUND;
        return st;
    }
    if (vol.fs.fsize >= FAT_SECTORS_MAX) {
        printf("fat %s: a FAT of %u sectors: not a volume to trust, not mounted\n", vol.name,
               (unsigned)vol.fs.fsize);
        *no_volume = true;
        return ERR_IO;
    }
    disk_watch();
    printf("fat %s: mounted %s, %lu MiB%s\n", vol.name, type_name(),
           (unsigned long)((uint64_t)(vol.fs.n_fatent - 2) * vol.fs.csize / 2048),
           vol.read_only ? ", read-only" : "");
    return OK;
}

/* Wait (ONCE) for the fs channel's next request or its client's close. */
static status_t arm_fs(handle_t serve)
{
    return jam_port_bind(vol.port, serve, FAT_KEY_FS, SIG_READABLE | SIG_PEER_CLOSED,
                         PORT_BIND_ONCE);
}

/* Serve until the fs channel's client is gone or the disk is (OK), or
 * something fails (its status). Each channel gets FAT_BATCH requests per
 * turn, so one busy client can't starve the others. */
static status_t run(handle_t serve)
{
    status_t st = arm_fs(serve);
    if (st == OK)
        st = jam_port_bind(vol.port, vol.block, FAT_KEY_BLOCK, SIG_PEER_CLOSED, PORT_BIND_ONCE);
    while (st == OK && !vol.disk_gone) {
        struct port_packet pkt;
        st = jam_port_wait(vol.port, DEADLINE_NEVER, &pkt);
        if (st != OK)
            break;
        if (pkt.key == FAT_KEY_BLOCK) {
            vol.disk_gone = true;
        } else if (pkt.key & FAT_KEY_FILE_BIT) {
            files_event(pkt.key);
        } else if (pkt.key == FAT_KEY_FS) {
            for (unsigned i = 0; i < FAT_BATCH && st == OK && !vol.disk_gone; i++) {
                files_reap();
                st = fs_serve_one(serve, &fat_fs_ops, NULL);
            }
            if (st == ERR_PEER_CLOSED)
                return OK;
            if (st == OK || st == ERR_SHOULD_WAIT)
                st = arm_fs(serve);
        }
    }
    return st;
}

int main(int argc, char **argv)
{
    vol.name = argc >= 2 ? argv[1] : "fat";
    vol.may_format = argc >= 3 && !strcmp(argv[2], FAT_ARG_FORMAT);
    bool no_volume = false;
    handle_t serve = startup_handle(FAT_SR_SERVE), block = startup_handle(FAT_SR_BLOCK);
    if (serve == HANDLE_INVALID || block == HANDLE_INVALID) {
        printf("fat %s: no %s channel: nothing to do\n", vol.name,
               serve == HANDLE_INVALID ? "fs" : "block");
        return 1;
    }
    vol.rtc_root = startup_handle(SR_RESOURCE);
    status_t st = jam_port_create(&vol.port);
    if (st == OK)
        st = disk_open(block);
    if (st == OK)
        st = mount(&no_volume);
    if (st != OK) {
        printf("fat %s: not serving: %s\n", vol.name, status_str(st));
        return no_volume ? FAT_EXIT_NO_VOLUME : 1;
    }
    st = run(serve);
    files_close_all();
    if (vol.disk_gone) {
        printf("fat %s: the disk is gone: stopping\n", vol.name);
        return 0;
    }
    status_t st2 = disk_settle();
    if (st == OK)
        st = st2;
    if (st != OK)
        printf("fat %s: stopping: %s\n", vol.name, status_str(st));
    return st == OK ? 0 : 1;
}
