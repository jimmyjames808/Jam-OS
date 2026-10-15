/* utest: posixtest's links section on an extfs volume (docs/M13.6-PLAN.md's
 * U6: libc on a filesystem with links and modes). extfs over a RAM disk it
 * formats is mounted twice in our namespace, at /u6 and (a view of the
 * same channel) at /u6v, and bin/posixtest runs `links` with both: what
 * POSIX gains there (pt's links.c lists it). Its lines are in the log; the
 * disk is saved for e2fsck as the other extfs cases' are. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <fs_idl.h>
#include <os.h>
#include "extfstest.h"
#include "utest.h"

bool t_extfs_posix(void)
{
    static struct ramdisk xd;
    static const char *const grants[] = { "/u6", "/u6v", "/boot", NULL };
    struct fatrun r;
    handle_t mine, view;
    CHECK(ramdisk_create(&xd, EXT_DISK_SECTORS));
    CHECK(extfs_start(&r, &xd, false, NULL, 0));
    CHECK_ST(jam_handle_duplicate(r.fs, RIGHT_SAME, &mine), OK);
    CHECK_ST(ns_mount("/u6", mine), OK);
    CHECK_ST(fs_view_until(r.fs, now() + FAT_CALL_NS, 0, &view), OK);
    CHECK_ST(ns_mount("/u6v", view), OK);

    const char *argv[] = { "posixtest", "links" };
    handle_t proc;
    struct spawn_args a = { .path = "bin/posixtest", .argc = 2, .argv = argv,
                            .job = startup_handle(SR_JOB), .ns = grants };
    struct process_info info = { 0 };
    status_t st = spawn(&a, &proc);
    if (st == OK) {
        st = spawn_wait(proc, 120 * NS_PER_S, &info);
        jam_handle_close(proc);
    }
    CHECK_ST(ns_unmount("/u6v"), OK);
    CHECK_ST(ns_unmount("/u6"), OK);
    CHECK_ST(st, OK);
    if (info.killed || info.exit_code)
        FAIL("posixtest links ended with %ld (its lines are in the log)", (long)info.exit_code);
    CHECK(fat_stop(&r));
    CHECK(extfs_save(&xd, "posix"));
    return ramdisk_destroy(&xd);
}
