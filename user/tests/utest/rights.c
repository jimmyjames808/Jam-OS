/* utest: the root resource's powers (<jam/abi.h> RIGHT_ROOT_*) from user
 * space, and vmo_make_exec, the one way a VMO a process made gets
 * RIGHT_EXEC (a program from /data, docs/history/M8.6-SVC.md).
 *
 * Every call that takes the root is refused, ERR_ACCESS_DENIED, by a root
 * handle that has every right but its own power; the calls that would
 * change the machine (reboot, kexec, the debug commands) are only ever
 * made refused. What we hold of the root depends on our starter (init's
 * run: TEST_ROOT; the shell: our list's powers; none: those checks are
 * skipped). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

/* Our root without `power` (and without anything we don't hold), or 0. */
static handle_t root_without(rights_t power)
{
    handle_t root = startup_handle(SR_RESOURCE), h = HANDLE_INVALID;
    rights_t mine = RIGHT_WAIT | RIGHT_INSPECT | RIGHT_READ | RIGHT_WRITE | RIGHT_MANAGE |
                    RIGHTS_ROOT;
    /* Find what we may duplicate to: drop the rights we don't have, one by one. */
    for (unsigned bit = 0; root && bit < 32; bit++) {
        rights_t r = 1u << bit;
        if ((mine & r) && jam_handle_duplicate(root, r, &h) != OK)
            mine &= ~r;
        else if (mine & r)
            jam_handle_close(h);
    }
    h = HANDLE_INVALID;
    if (root && jam_handle_duplicate(root, mine & ~power, &h) != OK)
        h = HANDLE_INVALID;
    return h;
}

bool t_root_powers(void)
{
    if (!startup_handle(SR_RESOURCE)) {
        printf("utest: %s: no root resource (SR_RESOURCE): skipped\n", utest_cur);
        return true;
    }
    handle_t h, out, v;
    struct sys_info si;
    struct rtc_time rt;
    struct proc_stat ps;
    struct cpu_stat cs;
    CHECK_ST(jam_vmo_create(PAGE_SIZE, 0, HANDLE_INVALID, &v), OK);
    CHECK((h = root_without(RIGHT_ROOT_KLOG)) != HANDLE_INVALID);
    CHECK_ST(jam_klog_open(h, &out), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(h), OK);
    CHECK((h = root_without(RIGHT_ROOT_SERIAL)) != HANDLE_INVALID);
    CHECK_ST(jam_serial_open(h, &out), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(h), OK);
    CHECK((h = root_without(RIGHT_ROOT_SYSINFO)) != HANDLE_INVALID);
    CHECK_ST(jam_sys_info(h, &si), ERR_ACCESS_DENIED);
    CHECK_EQ(jam_cpu_stat(h, 0, &cs, 1), ERR_ACCESS_DENIED);
    CHECK_EQ(jam_proc_list(h, &ps, 1), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(h), OK);
    CHECK((h = root_without(RIGHT_ROOT_CLOCK)) != HANDLE_INVALID);
    CHECK_ST(jam_rtc_read(h, &rt), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(h), OK);
    CHECK((h = root_without(RIGHT_ROOT_REBOOT)) != HANDLE_INVALID);
    CHECK_ST(jam_reboot(h), ERR_ACCESS_DENIED);
    CHECK_ST(jam_kexec_reboot(h), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(h), OK);
    CHECK((h = root_without(RIGHT_ROOT_KEXEC)) != HANDLE_INVALID);
    CHECK_ST(jam_kexec_load(h, v, v, NULL, 0, 0), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(h), OK);
    CHECK((h = root_without(RIGHT_ROOT_DEBUG)) != HANDLE_INVALID);
    CHECK_EQ(jam_debug_command(h, "help", 4), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(h), OK);
    CHECK((h = root_without(RIGHT_ROOT_VMEX)) != HANDLE_INVALID);
    CHECK_ST(jam_vmo_make_exec(v, h, &out), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(h), OK);
    struct fb_info fbi;
    handle_t fbv, owner;
    CHECK((h = root_without(RIGHT_ROOT_SCREEN)) != HANDLE_INVALID);
    CHECK_ST(jam_framebuffer_take(h, &fbi, &fbv, &owner), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(h), OK);
    CHECK((h = root_without(RIGHT_ROOT_SERIAL_OUT)) != HANDLE_INVALID);
    CHECK_ST(jam_serial_write(h, "x", 1), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(h), OK);
    /* What we hold works: the figures and the clock where we have them. */
    handle_t root = startup_handle(SR_RESOURCE);
    if (jam_handle_duplicate(root, RIGHT_ROOT_SYSINFO, &h) == OK) {
        CHECK_ST(jam_sys_info(h, &si), OK);
        CHECK_ST(jam_handle_close(h), OK);
    }
    if (jam_handle_duplicate(root, RIGHT_ROOT_CLOCK, &h) == OK) {
        status_t st = jam_rtc_read(h, &rt);
        /* NOT_FOUND, TIMED_OUT: a machine without an RTC */
        CHECK(st == OK || st == ERR_NOT_FOUND || st == ERR_TIMED_OUT);
        CHECK_ST(jam_handle_close(h), OK);
    }
    CHECK_ST(jam_handle_close(v), OK);   /* never consumed: every call was refused */
    return true;
}

/* A copy of bin/utest in a VMO of our own, the only handle to it. */
static bool own_copy(handle_t *vmo, uint64_t *size)
{
    const struct bootfs_view *img;
    const void *data;
    CHECK_ST(bootfs_default(&img), OK);
    CHECK_ST(bootfs_lookup(img, "bin/utest", &data, size), OK);
    CHECK_ST(jam_vmo_create((*size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1), 0, HANDLE_INVALID, vmo),
             OK);
    CHECK_ST(jam_vmo_write(*vmo, 0, data, *size), OK);
    return true;
}

/* Run "utest exit7" from vmo (consumed): it must exit 7, or the spawn
 * fails with `want`. */
static bool run_from(handle_t vmo, uint64_t size, status_t want)
{
    handle_t job, proc;
    CHECK_ST(new_job(&job), OK);
    const char *argv[] = { "utest", "exit7" };
    struct spawn_args a = {
        .path = "utest-exec", .vmo = vmo, .offset = 0, .size = size, .argc = 2, .argv = argv,
        .job = job,
    };
    status_t st = spawn(&a, &proc);
    jam_handle_close(vmo);
    CHECK_ST(st, want);
    if (st == OK)
        CHECK(ns_child_exits(proc, 7));
    CHECK_ST(jam_handle_close(job), OK);
    return true;
}

bool t_vmo_make_exec(void)
{
    handle_t root = startup_handle(SR_RESOURCE), vmex, v, x, second, w;
    uint64_t size, addr = 0;
    if (!root || jam_handle_duplicate(root, RIGHT_ROOT_VMEX, &vmex) != OK) {
        printf("utest: %s: no RIGHT_ROOT_VMEX on our root: skipped\n", utest_cur);
        return true;
    }
    /* Without it the code is refused (no RIGHT_EXEC on a VMO of ours). */
    if (!own_copy(&v, &size) || !run_from(v, size, ERR_ACCESS_DENIED))
        return false;
    /* With it: it runs. The new handle can't write, map writable or grow
     * it, nor duplicate itself back to RIGHT_WRITE. */
    if (!own_copy(&v, &size))
        return false;
    CHECK_ST(jam_vmo_make_exec(v, vmex, &x), OK);
    CHECK_ST(jam_handle_close(v), ERR_BAD_HANDLE);   /* the old one is gone */
    CHECK_ST(jam_vmo_write(x, 0, "x", 1), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmo_set_size(x, 2 * PAGE_SIZE), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmo_decommit(x, 0, PAGE_SIZE), ERR_ACCESS_DENIED);
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), x, 0, PAGE_SIZE, VMAR_READ | VMAR_WRITE,
                          &addr), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_duplicate(x, RIGHT_READ | RIGHT_WRITE, &w), ERR_INVALID_ARGS);
    if (!run_from(x, size, OK))
        return false;
    /* A second handle: someone else could still write it. Refused, and
     * the handle is gone either way. */
    if (!own_copy(&v, &size))
        return false;
    CHECK_ST(jam_handle_duplicate(v, RIGHT_SAME, &second), OK);
    CHECK_ST(jam_vmo_make_exec(v, vmex, &x), ERR_BAD_STATE);
    CHECK_ST(jam_handle_close(v), ERR_BAD_HANDLE);
    CHECK_ST(jam_handle_close(second), OK);
    /* Mapped (even read-only): refused. */
    if (!own_copy(&v, &size))
        return false;
    CHECK_ST(jam_vmar_map(startup_handle(SR_SELF_VMAR), v, 0, PAGE_SIZE, VMAR_READ, &addr), OK);
    CHECK_ST(jam_vmo_make_exec(v, vmex, &x), ERR_BAD_STATE);
    CHECK_ST(jam_vmar_unmap(startup_handle(SR_SELF_VMAR), addr, PAGE_SIZE), OK);
    /* Not a VMO, a VMO without RIGHT_READ: refused, and kept. */
    CHECK_ST(jam_vmo_make_exec(vmex, vmex, &x), ERR_WRONG_TYPE);
    if (!own_copy(&v, &size))
        return false;
    CHECK_ST(jam_handle_replace(v, RIGHT_MAP, &v), OK);
    CHECK_ST(jam_vmo_make_exec(v, vmex, &x), ERR_ACCESS_DENIED);
    CHECK_ST(jam_handle_close(v), OK);
    CHECK_ST(jam_handle_close(vmex), OK);
    return true;
}
