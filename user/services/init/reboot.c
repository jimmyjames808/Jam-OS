/* init's reboot by kexec (initctl.reboot, so also Ctrl+Alt+Del): a fresh
 * copy of Jam OS started without the firmware, Limine and the boot menu
 * (kernel/kexec/, <jam/kexec.h>).
 *
 * The kernel keeps a stored copy of the kernel and boot image this boot
 * started from, ready to run. It is used as it is unless the stick has
 * newer ones: when /esp is first mounted, init notes the size and
 * modification time of /esp/boot/jamos.elf and bootfs.img; at reboot, if
 * /esp is there and either differs (or nothing was noted), both files are
 * read and handed to the kernel (kexec_load) in place of the stored copy.
 * Else nothing is read at all. If they can't be read, or the kernel
 * refuses them (a flash cut short, a damaged copy), the stored copy, the
 * last good one, is started all the same: a short notice on the screen
 * says so first. Only without a stored copy at all does the reboot fall
 * back to the firmware (into whatever the stick holds).
 * initctl.kernel_load (the shell's `kernel
 * load`) reads and hands them over at once, and notes them as the stored
 * copy's: the reboot after it reads nothing. An update (update.c) makes a
 * fetched build the stored copy and notes /esp's files as they are then,
 * so the reboot keeps the fetched build until the stick changes.
 *
 * Then what a firmware reboot does too (/data synced, logd's last lines
 * written, the volume left clean), devmgr's shutdown (DEVMGR_SHUTDOWN: the
 * filesystems stopped clean, the class drivers, then the bus drivers'
 * final halt and reset, so no device is left writing memory), and
 * kexec_reboot. Any step that fails before the jump returns, and ctl.c
 * resets through the firmware instead. The screen already shows only the
 * splash background: the shell or the console blanked it (console.blank)
 * before asking for the reboot. */
#include <devmgr.h>
#include <idl/console.h>
#include <os.h>
#include "init.h"

#define ESP_MOUNT   "/esp"
#define KERNEL_FILE ESP_MOUNT "/boot/jamos.elf"
#define BOOTFS_FILE ESP_MOUNT "/boot/bootfs.img"
#define KERNEL_MAX  (64ull << 20)
#define BOOTFS_MAX  (256ull << 20)
#define LOG_WAIT    NS_PER_S          /* logd's flush, as before a firmware reboot */
#define STOP_WAIT   (30 * NS_PER_S)   /* devmgr's shutdown: every driver stopped */
#define NOTICE_SHOW (3 * NS_PER_S)    /* the notice on the screen before the reboot goes on */

/* A file as noted: its size and modification time. */
struct noted {
    uint64_t size, mtime;
};

static bool noted;                  /* /esp's files were noted (the first /esp) */
static struct noted kernel, bootfs;

static status_t stat_file(const char *path, struct noted *out)
{
    bool dir = false;
    status_t st = fs_stat(path, &out->size, &dir, &out->mtime);
    return st == OK && dir ? ERR_INVALID_ARGS : st;
}

void reboot_note_esp(void)
{
    if (noted || stat_file(KERNEL_FILE, &kernel) != OK || stat_file(BOOTFS_FILE, &bootfs) != OK)
        return;
    noted = true;
    printf("init: kexec: noted " KERNEL_FILE " (%lu bytes) and " BOOTFS_FILE " (%lu bytes): "
           "a reboot reads them only if they change\n", (unsigned long)kernel.size,
           (unsigned long)bootfs.size);
}

/* Does /esp hold another kernel or boot image than the one stored? false
 * also without /esp (there is nothing else to start). */
static bool esp_changed(void)
{
    struct noted k, b;
    if (stat_file(KERNEL_FILE, &k) != OK || stat_file(BOOTFS_FILE, &b) != OK) {
        printf("init: kexec: no " KERNEL_FILE " or " BOOTFS_FILE ": the stored kernel, "
               "no files read\n");
        return false;
    }
    if (noted && k.size == kernel.size && k.mtime == kernel.mtime && b.size == bootfs.size &&
        b.mtime == bootfs.mtime) {
        printf("init: kexec: /esp unchanged: the stored kernel, no files read\n");
        return false;
    }
    printf("init: kexec: %s: reading " KERNEL_FILE " and " BOOTFS_FILE "\n",
           noted ? "/esp's kernel or boot image changed" : "nothing noted of /esp at its mount");
    return true;
}

/* Read both files and hand them to the kernel in place of the stored copy;
 * their sizes into *kb and *bb, the read's time into *ms. */
static status_t load(uint64_t *kb, uint64_t *bb, uint32_t *ms)
{
    handle_t k = HANDLE_INVALID, b = HANDLE_INVALID;
    uint64_t ks = 0, bs = 0, t0 = now();
    status_t st = file_read_vmo(KERNEL_FILE, KERNEL_MAX, &k, &ks);
    if (st == OK)
        st = file_read_vmo(BOOTFS_FILE, BOOTFS_MAX, &b, &bs);
    uint64_t read_ms = (now() - t0) / NS_PER_MS;
    if (st == OK)
        st = jam_kexec_load(shell_root(), k, b, NULL, 0, 0);   /* the next kernel's usual line */
    if (k)
        jam_handle_close(k);
    if (b)
        jam_handle_close(b);
    printf("init: kexec: %lu + %lu KiB read in %lu ms, kexec_load: %s\n",
           (unsigned long)(ks >> 10), (unsigned long)(bs >> 10), (unsigned long)read_ms,
           status_str(st));
    *kb = ks;
    *bb = bs;
    *ms = read_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)read_ms;
    return st;
}

status_t init_kernel_load(uint64_t *kb, uint64_t *bb, uint32_t *ms)
{
    struct noted k, b;
    if (stat_file(KERNEL_FILE, &k) != OK || stat_file(BOOTFS_FILE, &b) != OK) {
        printf("init: kernel load: no " KERNEL_FILE " or " BOOTFS_FILE "\n");
        return ERR_NOT_FOUND;
    }
    status_t st = load(kb, bb, ms);
    if (st != OK)
        return st;
    /* The stored copy is these files now: a reboot reads them again only
     * if they change once more. */
    kernel = k;
    bootfs = b;
    noted = true;
    return OK;
}

void reboot_keep_stored(void)
{
    struct noted k, b;
    if (stat_file(KERNEL_FILE, &k) != OK || stat_file(BOOTFS_FILE, &b) != OK)
        return;   /* no /esp now: its first mount notes it (reboot_note_esp) */
    kernel = k;
    bootfs = b;
    noted = true;
}

/* The stick's files didn't load (why): the stored copy is started
 * instead. Said in the log and, for NOTICE_SHOW, on the screen the shell
 * (or the console, for Ctrl+Alt+Del) blanked before asking. */
static void say_stored_instead(status_t why)
{
    char line[160];
    int n = snprintf(line, sizeof(line), "\r\n\033[93mthe stick's kernel didn't load (%s): "
                     "restarting the one in memory\033[0m\r\n", status_str(why));
    printf("init: kexec: the stick's kernel didn't load (%s): restarting the one in memory\n",
           status_str(why));
    handle_t c = shell_console();
    if (!c)
        return;
    uint8_t text[2048] = { 0 };
    memcpy(text, line, (size_t)n);
    uint64_t deadline = now() + NS_PER_S;
    if (console_blank_until(c, deadline, 0) != OK ||
        console_write_until(c, deadline, (uint16_t)n, text) != OK)
        return;
    jam_nanosleep(now() + NOTICE_SHOW);
    (void)console_blank_until(c, now() + NS_PER_S, 1);   /* dark again until the next splash */
}

status_t init_reboot_kexec(void)
{
    if (esp_changed()) {
        uint64_t kb, bb;
        uint32_t ms;
        status_t st = load(&kb, &bb, &ms);
        if (st == ERR_NOT_SUPPORTED)
            return st;   /* no stored copy at all: the firmware it is */
        if (st != OK)
            say_stored_instead(st);   /* a refused image left the stored one armed */
    }
    mounts_sync();
    shell_flush_log(now() + LOG_WAIT);
    mounts_settle();
    uint64_t t0 = now();
    status_t st = shell_stop_devmgr(now() + STOP_WAIT);
    if (st != OK)   /* its drivers' DMA caps are closed either way: bus mastering is off */
        printf("init: kexec: devmgr didn't stop in order (%s): its job was killed\n",
               status_str(st));
    /* Seen only on the serial port: logd has stopped, the screen is dark. */
    printf("init: kexec: devmgr stopped in %lu ms, jumping\n",
           (unsigned long)((now() - t0) / NS_PER_MS));
    st = jam_kexec_reboot(shell_root());   /* returns only if it failed */
    printf("init: kexec: the jump failed (%s)\n", status_str(st));
    return st;
}
