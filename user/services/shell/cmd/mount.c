/* mount: the mounts and whether each can be written; `mount -w /usb0`
 * makes another stick's mount writable, `mount -r /usb0` read-only again.
 *
 * The change goes through init (abi/idl/initctl.idl) to devmgr, which
 * restarts the mount's filesystem service on a `block` channel opened the
 * new way: a read-only mount is one whose disk refuses writes, not one
 * that is merely asked not to write. The mount goes away for a moment and
 * comes back, so files open on it are closed.
 *
 * /boot (the boot image, in memory) and /esp (the boot files) are never
 * writable, and /data always is: asking otherwise is refused here, by
 * init and by devmgr. A stick is never formatted, in either mode. */
#include <idl/initctl.h>
#include "sh.h"

#define MOUNT_WAIT (30 * NS_PER_S)    /* devmgr syncs, then stops the service */
#define BACK_WAIT  (10 * NS_PER_S)    /* the mount back in our namespace */
#define POLL       (50 * NS_PER_MS)

static int list(void)
{
    char point[NS_NAME_MAX];
    int status = 0;
    sh_say("%-10s %-11s %s\n", "Mount", "Access", "Volume");
    for (unsigned i = 0; ns_mount_at(i, point); i++) {
        uint64_t total, free_bytes;
        bool ro;
        char label[17];
        status_t st = fs_statfs(point, &total, &free_bytes, &ro, label);
        if (st != OK) {
            sh_say("%-10s %s\n", point, sh_why(st));
            status = 1;
            continue;
        }
        sh_say("%-10s %-11s %s\n", point, ro ? "read-only" : "read-write", label);
    }
    return status;
}

/* Why a mount that isn't another stick's can't be changed, or NULL. */
static const char *fixed(const char *path)
{
    if (!strcmp(path, "/boot"))
        return "is the boot image in memory: always read-only";
    if (!strcmp(path, "/esp"))
        return "holds the boot files: never writable";
    if (!strcmp(path, "/data"))
        return "is the system's own: always writable";
    return NULL;
}

SH_CMD(mount)
{
    if (argc == 1)
        return list();
    bool writable = !strcmp(argv[1], "-w");
    if (argc != 3 || (!writable && strcmp(argv[1], "-r") != 0)) {
        sh_tty("usage: mount            the mounts\n"
               "       mount -w /usbN   make another stick's mount writable\n"
               "       mount -r /usbN   read-only again\n");
        return 2;
    }
    const char *path = argv[2];
    const char *how = writable ? "read-write" : "read-only";
    uint64_t total, free_bytes;
    bool ro;
    char label[17];
    uint8_t field[16] = { 0 };
    if (path[0] != '/' || strlen(path) >= sizeof(field) || strchr(path + 1, '/')) {
        sh_tty("mount: %s: not a mount point (try `mount`)\n", path);
        return 1;
    }
    if (fixed(path)) {
        sh_tty("mount: %s %s\n", path, fixed(path));
        return 1;
    }
    status_t st = sh_is_mount(path) ? fs_statfs(path, &total, &free_bytes, &ro, label)
                                    : ERR_NOT_FOUND;
    if (st != OK) {
        sh_tty("mount: %s: %s\n", path, st == ERR_NOT_FOUND ? "not mounted" : sh_why(st));
        return 1;
    }
    if (ro != writable) {
        sh_say("mount: %s is %s already\n", path, how);
        return 0;
    }
    if (!sh_initctl()) {
        sh_tty("mount: no init to ask\n");   /* a shell init didn't start */
        return 1;
    }
    memcpy(field, path, strlen(path));
    sh_flush();
    st = initctl_mount_until(sh_initctl(), now() + MOUNT_WAIT, field, writable);
    if (st != OK) {
        sh_tty("mount: %s: %s\n", path,
               st == ERR_ACCESS_DENIED ? "only another stick's mounts (/usbN) can be changed"
               : st == ERR_NOT_FOUND   ? "not mounted"
                                       : status_str(st));
        return 1;
    }
    /* The service was started again: the mount is back once its volume is
     * mounted and init has passed us the new channel. */
    for (uint64_t until = now() + BACK_WAIT; now() < until; jam_nanosleep(now() + POLL)) {
        if (fs_statfs(path, &total, &free_bytes, &ro, label) == OK && ro != writable) {
            sh_say("mount: %s is now %s\n", path, how);
            return 0;
        }
    }
    sh_tty("mount: %s did not come back %s (see `log`)\n", path, how);
    return 1;
}
