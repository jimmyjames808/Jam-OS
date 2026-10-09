/* Who pays for channel messages nobody has read: the writer's job, until
 * they are read or dropped (the reader's end closes), also after the
 * writer has closed its end or its job has no process left. A real user
 * program writes (bin/utest in its "fill-close" and "write-exit" modes,
 * user/tests/utest/child.c), since a kernel thread's messages are charged
 * to no job. The reading end sits alone in a table of a "server" job, as
 * svc.connect leaves a server's end, so a charge that moved to the reader
 * would show there.
 *
 * The hostile pattern these guard against: a client writes up to its own
 * limit on a connection, closes it, and does it again on a new one. If a
 * close handed the backlog to the reader, every round would free the
 * client's quota and fill the server's job instead. */
#include <jam/channel.h>
#include <jam/event.h>
#include <jam/handle.h>
#include <jam/ktest.h>
#include <jam/process.h>
#include <jam/startup.h>
#include <jam/time.h>
#include <jam/userboot.h>

#define CH(p) ((struct kobject *)(p))

#define PAYS_LIMIT  (64u << 10)   /* the client's message bytes: about 60 of its 1 KiB writes */
#define PAYS_ROUNDS 20            /* connections the hostile client fills and closes */

/* A "fill-close" client in a job of its own, and a server job whose
 * table holds the server's ends. */
struct pays {
    struct job          *jc;     /* the client's job (limit PAYS_LIMIT) */
    struct job          *js;     /* the server's job: its table's */
    struct handle_table  ts;     /* the server's table */
    struct channel      *conn;   /* ours: new ends go to the client on it */
    struct event        *done;   /* the client signals it after each close */
    struct process      *p;      /* the client */
};

static uint32_t job_refs(struct job *j)
{
    return __atomic_load_n(&job_kobject(j)->refs, __ATOMIC_RELAXED);
}

static uint64_t job_msg_bytes(struct job *j)
{
    return job_used(j, JOB_LIMIT_MSG_BYTES);
}

static uint64_t pays_deadline(uint64_t ms)
{
    return uptime_ns() + kt_patience_ms(ms) * NS_PER_MS;
}

static void pays_start(struct pays *f)
{
    f->jc = kt_fresh_job();
    f->js = kt_fresh_job();
    KT_EQ(job_set_limit(f->jc, JOB_LIMIT_MSG_BYTES, PAYS_LIMIT), OK);
    handle_table_init(&f->ts);
    f->ts.job = f->js;
    struct channel *theirs;
    KT_EQ(channel_create(&f->conn, &theirs), OK);
    KT_EQ(event_create(&f->done), OK);
    kobject_ref(CH(f->done));
    struct userboot_handle x[2] = {
        { SR_USER, khandle_from_new(CH(theirs), RIGHTS_BASIC | RIGHTS_IO) },
        { SR_USER + 1, khandle_from_new(CH(f->done), RIGHTS_BASIC | RIGHT_SIGNAL) },
    };
    const char *argv[] = { "utest", "fill-close" };
    KT_EQ(userboot_spawn("bin/utest", argv, 2, f->jc, x, 2, NULL, &f->p), OK);
}

/* One connection: a new pair, our end alone in the server's table, the
 * other end sent to the client, which fills it and closes it. Returns once
 * the client's close has finished: our end (a reference of the caller's)
 * and its handle in f->ts. */
static struct channel *pays_round(struct pays *f, handle_t *h)
{
    struct channel *ours, *theirs;
    KT_EQ(channel_create(&ours, &theirs), OK);
    kobject_ref(CH(ours));
    struct khandle kh = khandle_from_new(CH(ours), RIGHTS_BASIC | RIGHTS_IO);
    KT_EQ(handle_insert(&f->ts, &kh, h), OK);
    KT_EQ(event_signal(f->done, SIG_SIGNALED, 0), OK);
    struct khandle send = khandle_from_new(CH(theirs), RIGHTS_BASIC | RIGHTS_IO);
    KT_EQ(channel_write(f->conn, "c", 1, &send, 1), OK);
    KT_EQ(object_wait_one(CH(f->done), SIG_SIGNALED, pays_deadline(10000), NULL), OK);
    KT_ASSERT(kt_signals_of(&f->ts, *h) & SIG_PEER_CLOSED);
    return ours;
}

/* What waits on ours, and what it is charged. */
static uint64_t queued_charge(struct channel *ours, uint32_t *n)
{
    uint64_t charged;
    channel_queued(ours, n, &charged);
    return charged;
}

/* Read everything on ours (its writer has closed): how many there were. */
static uint32_t read_all(struct channel *ours)
{
    static uint8_t buf[1024];
    uint32_t n = 0, nb;
    status_t st;
    while ((st = channel_read(ours, buf, sizeof(buf), &nb, NULL, 0, NULL)) == OK)
        n++;
    KT_EQ(st, ERR_PEER_CLOSED);
    return n;
}

/* Wait until j's dead processes have given everything back but message
 * bytes (dead is not yet freed). */
static void settle(struct job *j)
{
    for (uint64_t d = pays_deadline(2000); uptime_ns() < d; thread_sleep_ms(1))
        if (!job_used(j, JOB_LIMIT_PAGES) && !job_used(j, JOB_LIMIT_THREADS) &&
            !job_used(j, JOB_LIMIT_HANDLES))
            break;
}

/* The client sees conn close and exits 0; then everything is gone. */
static void pays_stop(struct pays *f)
{
    kobject_unref(CH(f->conn));
    KT_EQ(object_wait_one(process_kobject(f->p), SIG_TERMINATED, pays_deadline(10000), NULL), OK);
    struct process_info info;
    process_get_info(f->p, &info);
    KT_ASSERT(!info.killed);
    KT_EQ(info.exit_code, 0);
    kobject_unref(process_kobject(f->p));
    kobject_unref(CH(f->done));
    handle_table_destroy(&f->ts);
    settle(f->jc);
    kt_job_is_empty(f->jc);
    kt_job_is_empty(f->js);
    job_unref(f->jc);
    job_unref(f->js);
}

/* The hostile client fills a connection and closes it, PAYS_ROUNDS times.
 * Its first backlog stays its own: the server's job pays nothing, and the
 * client's quota stays used, so each later connection gets nothing at
 * all, until the server reads the first; then the client can fill one
 * again. */
KTEST(chan_writer_pays_fill_close_repeat)
{
    uint64_t live = channel_live_count();
    struct pays f;
    pays_start(&f);
    struct channel *ours[PAYS_ROUNDS + 1];
    handle_t h[PAYS_ROUNDS + 1];
    uint64_t full = 0;
    for (unsigned r = 0; r < PAYS_ROUNDS; r++) {
        ours[r] = pays_round(&f, &h[r]);
        uint32_t n;
        uint64_t charged = queued_charge(ours[r], &n);
        KT_EQ(job_msg_bytes(f.js), 0);   /* the reader's job pays nothing, ever */
        if (r == 0) {
            KT_ASSERT(n >= 32);
            full = job_msg_bytes(f.jc);
            KT_EQ(full, charged);    /* the client still pays all of it */
        } else {
            KT_EQ(n, 0);             /* nothing more fits... */
            KT_EQ(job_msg_bytes(f.jc), full);   /* ...as the close freed nothing */
        }
    }
    KT_ASSERT(read_all(ours[0]) >= 32);
    KT_EQ(job_msg_bytes(f.jc), 0);   /* read: the client is paid back */
    ours[PAYS_ROUNDS] = pays_round(&f, &h[PAYS_ROUNDS]);
    uint32_t n;
    KT_EQ(queued_charge(ours[PAYS_ROUNDS], &n), job_msg_bytes(f.jc));
    KT_ASSERT(n >= 32);          /* and it can fill a connection again */
    KT_EQ(job_msg_bytes(f.js), 0);
    for (unsigned r = 0; r <= PAYS_ROUNDS; r++)
        kobject_unref(CH(ours[r]));
    pays_stop(&f);
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* The server reads the backlog: each message read pays the client back. */
KTEST(chan_writer_pays_until_read)
{
    uint64_t live = channel_live_count();
    struct pays f;
    pays_start(&f);
    handle_t h;
    struct channel *ours = pays_round(&f, &h);
    uint32_t n;
    uint64_t charged = queued_charge(ours, &n);
    KT_ASSERT(n >= 32 && charged > 0);
    KT_EQ(job_msg_bytes(f.jc), charged);
    KT_EQ(job_msg_bytes(f.js), 0);
    uint8_t buf[1024];
    uint32_t nb;
    KT_EQ(channel_read(ours, buf, sizeof(buf), &nb, NULL, 0, NULL), OK);
    KT_EQ(job_msg_bytes(f.jc), charged - charged / n);   /* the messages are all alike */
    KT_EQ(read_all(ours), n - 1);
    KT_EQ(job_msg_bytes(f.jc), 0);
    KT_EQ(job_msg_bytes(f.js), 0);
    kobject_unref(CH(ours));
    pays_stop(&f);
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* The server closes its end without reading: the backlog is dropped, and
 * the client paid back at once. */
KTEST(chan_writer_pays_until_reader_closes)
{
    uint64_t live = channel_live_count();
    struct pays f;
    pays_start(&f);
    handle_t h;
    struct channel *ours = pays_round(&f, &h);
    uint32_t n;
    uint64_t charged = queued_charge(ours, &n);
    KT_ASSERT(n >= 32);
    KT_EQ(job_msg_bytes(f.jc), charged);
    KT_EQ(job_msg_bytes(f.js), 0);
    KT_EQ(handle_close(&f.ts, h), OK);
    KT_EQ(job_msg_bytes(f.jc), 0);
    KT_EQ(job_msg_bytes(f.js), 0);
    kobject_unref(CH(ours));
    pays_stop(&f);
    KT_GLOBAL_EQ(channel_live_count(), live);
}

/* utest's "write-exit" in job j writes 8 KiB on a channel whose other end
 * is alone in ts, and exits. Returns that end (a reference of ours) and
 * its handle, once j holds nothing but what the 8 messages cost. */
static struct channel *exited_writer(struct job *j, struct handle_table *ts, handle_t *h)
{
    struct channel *ours, *theirs;
    KT_EQ(channel_create(&ours, &theirs), OK);
    kobject_ref(CH(ours));
    struct khandle kh = khandle_from_new(CH(ours), RIGHTS_BASIC | RIGHTS_IO);
    KT_EQ(handle_insert(ts, &kh, h), OK);
    struct userboot_handle x = { SR_USER, khandle_from_new(CH(theirs), RIGHTS_BASIC | RIGHTS_IO) };
    const char *argv[] = { "utest", "write-exit" };
    struct process *p;
    KT_EQ(userboot_spawn("bin/utest", argv, 2, j, &x, 1, NULL, &p), OK);
    KT_EQ(object_wait_one(process_kobject(p), SIG_TERMINATED, pays_deadline(10000), NULL), OK);
    struct process_info info;
    process_get_info(p, &info);
    KT_EQ(info.exit_code, 0);
    kobject_unref(process_kobject(p));
    settle(j);
    /* Ours, and one per message: nothing else keeps the job now. */
    for (uint64_t d = pays_deadline(2000); job_refs(j) > 9 && uptime_ns() < d;)
        thread_sleep_ms(1);
    KT_EQ(job_refs(j), 9);
    uint32_t n;
    KT_EQ(job_msg_bytes(j), queued_charge(ours, &n));
    KT_EQ(n, 8);
    KT_ASSERT(job_msg_bytes(j) >= 8 * 1024);
    return ours;
}

/* A writer's job whose last process has exited still pays for what it
 * left unread, kept alive by those messages alone; reading them all, or
 * closing the reading end, releases it, and nothing is left. */
KTEST(chan_writer_pays_after_job_exits)
{
    uint64_t live = channel_live_count();
    struct job *js = kt_fresh_job();
    struct handle_table ts;
    handle_table_init(&ts);
    ts.job = js;

    struct job *j = kt_fresh_job();
    handle_t h;
    struct channel *ours = exited_writer(j, &ts, &h);
    KT_EQ(job_msg_bytes(js), 0);
    KT_EQ(read_all(ours), 8);   /* drained */
    KT_EQ(job_refs(j), 1);
    kt_job_is_empty(j);
    job_unref(j);
    KT_EQ(handle_close(&ts, h), OK);
    kobject_unref(CH(ours));

    j = kt_fresh_job();
    ours = exited_writer(j, &ts, &h);
    KT_EQ(job_msg_bytes(js), 0);
    KT_EQ(handle_close(&ts, h), OK);   /* closed unread */
    KT_EQ(job_refs(j), 1);
    kt_job_is_empty(j);
    job_unref(j);
    kobject_unref(CH(ours));

    handle_table_destroy(&ts);
    kt_job_is_empty(js);
    job_unref(js);
    KT_GLOBAL_EQ(channel_live_count(), live);
}
