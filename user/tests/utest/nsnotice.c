/* utest: a running program whose namespace changes many times while it
 * never looks at it (<os.h> "files", SR_NS).
 *
 * init keeps the shell's and logd's namespaces in step with its own, and
 * logd opens its log file once and never looks up a path again: every
 * change sent to it would stay queued on its SR_NS channel. ns_update
 * takes back the unread one before it sends the next. Here utest is the
 * starter: it mounts and unmounts /v (/boot's channel under another name)
 * hundreds of times and keeps a sleeping child ("ns-sleeper", nschild.c)
 * in step after each change as init does, then checks what waits for the
 * child and what the child sees when it finally looks. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

#define CYCLES 300   /* /v mounted and unmounted: 600 changes */
/* What one waiting message may cost our job: the message, its handles
 * and the kernel's 1 KiB per carried handle. More than that is a queue. */
#define ONE_MESSAGE (NS_MSG_SIZE(NS_MAX_MOUNTS) + NS_MAX_MOUNTS * 1100u + 256u)

static const char *const follow[] = { "/boot", "/v", "/w", NULL };

/* Start the sleeper with our /boot; *to gets our end of its SR_NS
 * channel, *back a duplicate of its end. */
static bool start_sleeper(handle_t go, handle_t *to, handle_t *back, handle_t *proc)
{
    handle_t job;
    CHECK_ST(new_job(&job), OK);
    const char *argv[] = { "utest", "ns-sleeper" };
    struct spawn_handle x = { SR_USER, go };
    struct spawn_args a = {
        .path = "bin/utest", .name = "utest-ns", .argc = 2, .argv = argv, .job = job,
        .extra = &x, .nextra = 1, .ns = follow, .ns_out = to,
        .ns_back_out = back,
    };
    CHECK_ST(spawn(&a, proc), OK);
    CHECK_ST(jam_handle_close(job), OK);   /* the process keeps it alive */
    return true;
}

bool t_ns_changes_stay_bounded(void)
{
    if (fs_stat("/boot", NULL, NULL, NULL) != OK) {
        printf("utest: %s: no /boot in our namespace: skipped\n", utest_cur);
        return true;
    }
    handle_t mine, theirs, to, back, proc, fs;
    struct job_info start, before, after;
    CHECK_ST(info_of(own_job(), &start), OK);
    CHECK_ST(jam_channel_create(&mine, &theirs), OK);
    CHECK(start_sleeper(theirs, &to, &back, &proc));
    /* It runs (its startup message is read) and says so. */
    uint32_t word = 0, n = 0;
    signals_t seen;
    CHECK_ST(jam_object_wait_one(mine, SIG_READABLE, now() + 10 * NS_PER_S, &seen), OK);
    struct channel_read_args ra = {
        .h = mine, .bytes_cap = sizeof(word), .bytes = (uint64_t)(uintptr_t)&word,
        .actual_bytes = (uint64_t)(uintptr_t)&n,
    };
    CHECK_ST(jam_channel_read(&ra), OK);
    CHECK_ST(info_of(own_job(), &before), OK);
    for (unsigned i = 0; i < CYCLES; i++) {
        CHECK_ST(ns_channel("/boot", &fs), OK);
        CHECK_ST(ns_mount("/v", fs), OK);
        CHECK_ST(ns_update(to, back, follow), OK);
        CHECK_ST(ns_unmount("/v"), OK);
        CHECK_ST(ns_update(to, back, follow), OK);
    }
    CHECK_ST(ns_channel("/boot", &fs), OK);   /* one that arrives and stays */
    CHECK_ST(ns_mount("/w", fs), OK);
    CHECK_ST(ns_update(to, back, follow), OK);
    CHECK_ST(info_of(own_job(), &after), OK);
    uint64_t grew = after.used[JOB_LIMIT_MSG_BYTES] - before.used[JOB_LIMIT_MSG_BYTES];
    printf("utest: %s: %u changes, %lu message bytes waiting for the child\n", utest_cur,
           2 * CYCLES + 1, (unsigned long)grew);
    CHECK(grew <= ONE_MESSAGE);
    /* Now it looks: /w is there, /v (which came and went 300 times) is not. */
    CHECK_ST(jam_channel_write(mine, &word, sizeof(word), NULL, 0), OK);
    CHECK(ns_child_exits(proc, 0));
    CHECK_ST(ns_unmount("/w"), OK);
    CHECK_ST(jam_handle_close(to), OK);
    CHECK_ST(jam_handle_close(back), OK);
    CHECK_ST(jam_handle_close(mine), OK);
    CHECK_ST(info_of(own_job(), &after), OK);
    CHECK_EQ(after.used[JOB_LIMIT_MSG_BYTES], start.used[JOB_LIMIT_MSG_BYTES]);
    CHECK_EQ(after.used[JOB_LIMIT_HANDLES], start.used[JOB_LIMIT_HANDLES]);
    return true;
}
