/* init's reboot by kexec (initctl.reboot, so also Ctrl+Alt+Del): into
 * the kernel and boot image on the stick's ESP without going through the
 * firmware, Limine and the boot menu (kernel/kexec/, <jam/kexec.h>).
 *
 * In order: the two files read whole into VMOs while /esp is still there,
 * kexec_load (the kernel checks them and lays the new kernel out in its
 * reserved region; the crash kernel is gone from then on), then what a
 * firmware reboot does too (/data synced, logd's last lines written, the
 * volume left clean), then devmgr's shutdown (DEVMGR_SHUTDOWN: the
 * filesystems stopped clean, the class drivers, then the bus drivers'
 * final halt and reset, so no device is left writing memory), then
 * kexec_reboot. Any step that fails before the jump returns, and ctl.c
 * resets through the firmware instead. */
#include <devmgr.h>
#include <os.h>
#include "init.h"

#define KERNEL_FILE "/esp/boot/jamos.elf"
#define BOOTFS_FILE "/esp/boot/bootfs.img"
#define KERNEL_MAX  (64ull << 20)
#define BOOTFS_MAX  (256ull << 20)
#define LOG_WAIT    NS_PER_S          /* logd's flush, as before a firmware reboot */
#define STOP_WAIT   (30 * NS_PER_S)   /* devmgr's shutdown: every driver stopped */

/* Read both files and hand them to the kernel. */
static status_t load(void)
{
    handle_t k = HANDLE_INVALID, b = HANDLE_INVALID;
    uint64_t ks = 0, bs = 0, t0 = now();
    printf("init: kexec: reading %s and %s\n", KERNEL_FILE, BOOTFS_FILE);
    status_t st = file_read_vmo(KERNEL_FILE, KERNEL_MAX, &k, &ks);
    if (st == OK)
        st = file_read_vmo(BOOTFS_FILE, BOOTFS_MAX, &b, &bs);
    uint64_t read_ms = (now() - t0) / NS_PER_MS;
    if (st == OK)
        st = jam_kexec_load(shell_root(), k, b, NULL, 0, 0);   /* this boot's command line */
    if (k)
        jam_handle_close(k);
    if (b)
        jam_handle_close(b);
    printf("init: kexec: %s and %s (%lu + %lu KiB) read in %lu ms: %s\n", KERNEL_FILE,
           BOOTFS_FILE, (unsigned long)(ks >> 10), (unsigned long)(bs >> 10),
           (unsigned long)read_ms, status_str(st));
    return st;
}

status_t init_reboot_kexec(void)
{
    status_t st = load();
    if (st != OK)
        return st;
    mounts_sync();
    shell_flush_log(now() + LOG_WAIT);
    mounts_settle();
    uint64_t t0 = now();
    st = shell_stop_devmgr(now() + STOP_WAIT);
    if (st != OK)   /* its drivers' DMA caps are closed either way: bus mastering is off */
        printf("init: kexec: devmgr didn't stop in order (%s): its job was killed\n",
               status_str(st));
    /* Seen only on the serial port and the screen: logd has stopped. */
    printf("init: kexec: devmgr stopped in %lu ms, jumping\n",
           (unsigned long)((now() - t0) / NS_PER_MS));
    st = jam_kexec_reboot(shell_root());   /* returns only if it failed */
    printf("init: kexec: the jump failed (%s)\n", status_str(st));
    return st;
}
