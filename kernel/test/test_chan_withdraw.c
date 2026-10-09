/* channel_withdraw: a writer takes back what its peer hasn't read. A
 * server pays for its unread answers until they are read or dropped, also
 * after it hangs up (test_chan_pays.c), so a client that keeps a dead end
 * open would otherwise hold the server's backlog to it for ever, and a
 * client that does it again and again fills the server's job. A server
 * that gives up on a client withdraws first, writes one small error, and
 * closes: only the error stays charged, and the client still reads it.
 * The server is a real user program (bin/utest "fill-hangup",
 * user/tests/utest/child.c), since a kernel thread's messages are charged
 * to no job. */
#include <jam/channel.h>
#include <jam/event.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/process.h>
#include <jam/startup.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/userboot.h>

#define CH(p) ((struct kobject *)(p))

#define WD_ROUNDS 50            /* clients the server fills and hangs up on */
#define WD_LIMIT  (256u << 10)  /* the server's message bytes: 16 rounds' backlogs */
#define WD_ERROR  256u          /* at most what one error costs it (header and bytes) */

static uint64_t wd_deadline(uint64_t ms)
{
    return uptime_ns() + kt_patience_ms(ms) * NS_PER_MS;
}

/* Read ch's next message: its bytes into buf (n of them), or the status. */
static status_t read_one(struct channel *ch, char *buf, uint32_t cap, uint32_t *n)
{
    return channel_read(ch, buf, cap, n, NULL, 0, NULL);
}

/* 50 clients, each filled with 16 KiB of answers it never reads, given up
 * on and kept open: the server's job holds the 50 errors and nothing more,
 * and every client still reads its error, then the close. Without the
 * withdraw the job fills after 16 and the rest get nothing, not even the
 * error. */
KTEST(chan_withdraw_hangup_repeat)
{
    uint64_t live = channel_live_count();
    struct job *js = kt_fresh_job();
    KT_EQ(job_set_limit(js, JOB_LIMIT_MSG_BYTES, WD_LIMIT), OK);
    struct channel *conn, *theirs;
    struct event *done;
    KT_EQ(channel_create(&conn, &theirs), OK);
    KT_EQ(event_create(&done), OK);
    kobject_ref(CH(done));
    struct userboot_handle x[2] = {
        { SR_USER, khandle_from_new(CH(theirs), RIGHTS_BASIC | RIGHTS_IO) },
        { SR_USER + 1, khandle_from_new(CH(done), RIGHTS_BASIC | RIGHT_SIGNAL) },
    };
    const char *argv[] = { "utest", "fill-hangup" };
    struct process *p;
    KT_EQ(userboot_spawn("bin/utest", argv, 2, js, x, 2, NULL, &p), OK);

    static struct channel *kept[WD_ROUNDS];   /* the clients' ends: never closed meanwhile */
    for (unsigned r = 0; r < WD_ROUNDS; r++) {
        struct channel *server;
        KT_EQ(channel_create(&kept[r], &server), OK);
        KT_EQ(event_signal(done, SIG_SIGNALED, 0), OK);
        struct khandle send = khandle_from_new(CH(server), RIGHTS_BASIC | RIGHTS_IO);
        KT_EQ(channel_write(conn, "c", 1, &send, 1), OK);
        KT_EQ(object_wait_one(CH(done), SIG_SIGNALED, wd_deadline(10000), NULL), OK);
    }
    uint64_t used = job_used(js, JOB_LIMIT_MSG_BYTES);
    kprintf("chan_withdraw: %u clients given up on: %lu bytes of the server's job held\n",
            WD_ROUNDS, (unsigned long)used);
    KT_ASSERT(used <= WD_ROUNDS * WD_ERROR);
    for (unsigned r = 0; r < WD_ROUNDS; r++) {
        char buf[1024];
        uint32_t n = 0;
        KT_EQ(read_one(kept[r], buf, sizeof(buf), &n), OK);
        KT_EQ(n, 3);
        KT_ASSERT(!memcmp(buf, "bye", 3));   /* the error, and nothing of the backlog */
        KT_EQ(read_one(kept[r], buf, sizeof(buf), &n), ERR_PEER_CLOSED);
        kobject_unref(CH(kept[r]));
    }
    KT_EQ(job_used(js, JOB_LIMIT_MSG_BYTES), 0);   /* read: the server is paid back */

    kobject_unref(CH(conn));   /* the server sees it close and exits 0 */
    KT_EQ(object_wait_one(process_kobject(p), SIG_TERMINATED, wd_deadline(10000), NULL), OK);
    struct process_info info;
    process_get_info(p, &info);
    KT_ASSERT(!info.killed);
    KT_EQ(info.exit_code, 0);
    kobject_unref(process_kobject(p));
    kobject_unref(CH(done));
    for (uint64_t d = wd_deadline(2000); uptime_ns() < d; thread_sleep_ms(1))
        if (!job_used(js, JOB_LIMIT_PAGES) && !job_used(js, JOB_LIMIT_THREADS) &&
            !job_used(js, JOB_LIMIT_HANDLES))
            break;
    kt_job_is_empty(js);
    job_unref(js);
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* The call itself, on kernel-made ends: it drops exactly what the end
 * wrote and the peer hasn't read (the peer's whole queue, handles closed),
 * leaves what the other way holds, makes the end writable again, and is
 * refused on a shared end; once the peer has closed there is nothing left
 * to take. */
KTEST(chan_withdraw_rules)
{
    uint64_t live = channel_live_count();
    struct channel *a, *b, *c, *d;
    KT_EQ(channel_create(&a, &b), OK);
    KT_EQ(channel_create(&c, &d), OK);
    for (unsigned i = 0; i < 5; i++)
        KT_EQ(channel_write(a, "abcd", 4, NULL, 0), OK);
    KT_EQ(channel_write(b, "back", 4, NULL, 0), OK);
    struct khandle kh = khandle_from_new(CH(c), RIGHTS_BASIC | RIGHTS_IO);   /* carried by a message */
    KT_EQ(channel_write(a, "h", 1, &kh, 1), OK);
    KT_EQ(channel_withdraw(a), 6);
    char buf[16];
    uint32_t n = 0;
    KT_EQ(read_one(b, buf, sizeof(buf), &n), ERR_SHOULD_WAIT);   /* nothing left for b */
    KT_ASSERT(!(kobject_signals(CH(b)) & SIG_READABLE));
    KT_ASSERT(kobject_signals(CH(a)) & SIG_WRITABLE);
    KT_ASSERT(kobject_signals(CH(d)) & SIG_PEER_CLOSED);   /* the carried end's handle closed */
    KT_EQ(read_one(a, buf, sizeof(buf), &n), OK);   /* the other way is untouched */
    KT_EQ(n, 4);
    KT_EQ(channel_withdraw(a), 0);   /* nothing more */

    /* A shared end (two handles): refused, nothing dropped. */
    struct job *j = kt_fresh_job();
    struct handle_table t;
    handle_table_init(&t);
    t.job = j;
    handle_t h1, h2;
    kobject_ref(CH(a));
    kobject_ref(CH(a));
    struct khandle k1 = khandle_from_new(CH(a), RIGHTS_BASIC | RIGHTS_IO);
    struct khandle k2 = khandle_from_new(CH(a), RIGHTS_BASIC | RIGHTS_IO);
    KT_EQ(handle_insert(&t, &k1, &h1), OK);
    KT_EQ(handle_insert(&t, &k2, &h2), OK);
    KT_EQ(channel_write(a, "x", 1, NULL, 0), OK);
    KT_EQ(channel_withdraw(a), ERR_BAD_STATE);
    KT_EQ(handle_close(&t, h2), OK);
    KT_EQ(channel_withdraw(a), 1);   /* its sole holder again */
    handle_table_destroy(&t);
    kt_job_is_empty(j);
    job_unref(j);

    KT_EQ(channel_write(a, "y", 1, NULL, 0), OK);
    kobject_unref(CH(b));   /* the peer closes: its queue went with it */
    KT_EQ(channel_withdraw(a), 0);
    kobject_unref(CH(a));
    kobject_unref(CH(d));
    KT_GLOBAL_EQ(channel_live_count(), live);
}
