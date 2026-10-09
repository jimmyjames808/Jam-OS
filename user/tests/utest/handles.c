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
