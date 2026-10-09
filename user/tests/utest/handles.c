/* utest: M12's K3 calls from user space: handle_info (Q11). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

/* handle_info names each kind of object we hold, the related koids (a
 * channel end's peer, our thread's process, our process's job) and what
 * waits on a channel's two sides; no right is needed. */
bool t_handle_info(void)
{
    struct handle_info i;
    struct process_info pi;
    struct job_info ji;
    handle_t self = startup_handle(SR_SELF_PROCESS);
    CHECK_ST(jam_process_get_info(self, &pi), OK);
    CHECK_ST(jam_job_get_info(own_job(), &ji), OK);

    CHECK_ST(jam_handle_info(self, &i), OK);
    CHECK_EQ(i.type, OBJ_PROCESS);
    CHECK_EQ(i.koid, pi.koid);
    CHECK_EQ(i.related_koid, ji.koid);   /* a process's job */
    CHECK_ST(jam_handle_info(startup_handle(SR_SELF_THREAD), &i), OK);
    CHECK_EQ(i.type, OBJ_THREAD);
    CHECK_EQ(i.related_koid, pi.koid);   /* a thread's process */
    CHECK_ST(jam_handle_info(own_job(), &i), OK);
    CHECK_EQ(i.type, OBJ_JOB);
    CHECK_EQ(i.koid, ji.koid);
    CHECK(!(i.rights & RIGHT_MANAGE));   /* our own job: not ours to manage */
    CHECK_ST(jam_handle_info(startup_handle(SR_SELF_VMAR), &i), OK);
    CHECK_EQ(i.type, OBJ_VMAR);
    CHECK_ST(jam_handle_info(startup_handle(SR_BOOTFS), &i), OK);
    CHECK_EQ(i.type, OBJ_VMO);
    CHECK(!(i.rights & RIGHT_WRITE));

    handle_t e, weak, a, b;
    CHECK_ST(jam_event_create(&e), OK);
    CHECK_ST(jam_handle_replace(e, RIGHT_TRANSFER, &weak), OK);   /* no right we'd need */
    CHECK_ST(jam_handle_info(weak, &i), OK);
    CHECK_EQ(i.type, OBJ_EVENT);
    CHECK_EQ(i.rights, RIGHT_TRANSFER);
    CHECK_EQ(i.related_koid + i.queued + i.peer_queued + i.peer_queued_bytes + i.reserved, 0);
    CHECK_ST(jam_handle_close(weak), OK);
    CHECK_ST(jam_handle_info(weak, &i), ERR_BAD_HANDLE);

    CHECK_ST(jam_channel_create(&a, &b), OK);
    CHECK_ST(jam_channel_write(a, "1234", 4, NULL, 0), OK);
    CHECK_ST(jam_channel_write(a, "123456789", 9, NULL, 0), OK);
    CHECK_ST(jam_channel_write(b, "abcd", 4, NULL, 0), OK);
    struct handle_info bi;
    CHECK_ST(jam_handle_info(a, &i), OK);
    CHECK_ST(jam_handle_info(b, &bi), OK);
    CHECK_EQ(i.type, OBJ_CHANNEL);
    CHECK_EQ(i.related_koid, bi.koid);
    CHECK_EQ(bi.related_koid, i.koid);
    CHECK(i.queued == 1 && i.peer_queued == 2 && i.peer_queued_bytes == 13);
    CHECK(bi.queued == 2 && bi.peer_queued == 1 && bi.peer_queued_bytes == 4);
    CHECK_ST(jam_handle_close(b), OK);
    CHECK_ST(jam_handle_info(a, &i), OK);
    CHECK(i.related_koid == 0 && i.peer_queued == 0 && i.queued == 1);
    CHECK_ST(jam_handle_info(a, (struct handle_info *)8), ERR_INVALID_ARGS);
    CHECK_ST(jam_handle_close(a), OK);
    return true;
}

static void quick_thread(void *arg)
{
    (void)arg;
}

/* One rule for a length past a call's limit (review K finding 20):
 * ERR_OUT_OF_RANGE, nothing cut and nothing done; libos cuts a process's
 * or a thread's name itself. */
bool t_length_limits(void)
{
    static const char name[] = "a-name-that-is-longer-than-thirty-one-bytes";
    handle_t job, p, pv, t;
    CHECK_ST(new_job(&job), OK);
    CHECK_ST(jam_process_create(job, name, PROCESS_NAME_MAX + 1, 0, &p, &pv), ERR_OUT_OF_RANGE);
    CHECK_ST(jam_process_create(job, name, PROCESS_NAME_MAX, 0, &p, &pv), OK);
    CHECK_ST(jam_thread_create(p, name, PROCESS_NAME_MAX + 1, 0, &t), ERR_OUT_OF_RANGE);
    CHECK_ST(jam_thread_create(p, name, PROCESS_NAME_MAX, 0, &t), OK);
    CHECK_ST(jam_handle_close(t), OK);
    CHECK_ST(jam_handle_close(pv), OK);
    CHECK_ST(jam_handle_close(p), OK);
    CHECK_ST(jam_handle_close(job), OK);

    /* libos's thread_spawn cuts the name before the call */
    static uint8_t stack[8192] __attribute__((aligned(16)));
    CHECK_ST(thread_spawn(name, quick_thread, NULL, stack, sizeof(stack), &t), OK);
    CHECK_ST(jam_object_wait_one(t, SIG_TERMINATED, now() + 5 * NS_PER_S, NULL), OK);
    CHECK_ST(jam_handle_close(t), OK);

    /* debug_write refuses rather than cuts: nothing read past the limit */
    CHECK_ST(jam_debug_write("x", DEBUG_WRITE_MAX + 1), ERR_OUT_OF_RANGE);
    CHECK_ST(jam_debug_report("x", DEBUG_WRITE_MAX + 1), ERR_OUT_OF_RANGE);

    /* debug_command's, where our starter gave us the power (nothing runs:
     * the length is refused first) */
    handle_t root = startup_handle(SR_RESOURCE), dbg;
    if (root && jam_handle_duplicate(root, RIGHT_ROOT_DEBUG, &dbg) == OK) {
        char cmd[DEBUG_COMMAND_MAX + 1];
        for (unsigned i = 0; i < sizeof(cmd); i++)
            cmd[i] = 'x';
        CHECK_EQ(jam_debug_command(dbg, cmd, sizeof(cmd)), ERR_OUT_OF_RANGE);
        CHECK_ST(jam_handle_close(dbg), OK);
    }
    return true;
}

/* handle_replace consumes h whatever the status (M12's K-C, review K
 * finding 5): rights it hasn't close it, and the job is charged nothing
 * for it after. */
bool t_replace_consumes(void)
{
    struct job_info before, after;
    CHECK_ST(info_of(own_job(), &before), OK);
    handle_t e, x;
    CHECK_ST(jam_event_create(&e), OK);
    CHECK_ST(jam_handle_replace(e, RIGHT_READ | RIGHT_WAIT, &x), ERR_INVALID_ARGS);
    CHECK_ST(jam_handle_close(e), ERR_BAD_HANDLE);   /* gone */
    CHECK_ST(info_of(own_job(), &after), OK);
    CHECK_EQ(after.used[JOB_LIMIT_HANDLES], before.used[JOB_LIMIT_HANDLES]);
    CHECK_ST(jam_handle_replace(e, RIGHT_SAME, &x), ERR_BAD_HANDLE);   /* nothing to take */
    CHECK_ST(jam_event_create(&e), OK);
    CHECK_ST(jam_handle_replace(e, RIGHT_WAIT, &x), OK);
    CHECK_ST(jam_handle_close(e), ERR_BAD_HANDLE);
    CHECK_ST(jam_handle_close(x), OK);
    return true;
}
