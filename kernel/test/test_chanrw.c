/* A server thread killed inside channel_reply_wait loses no request: each
 * is either still queued or wholly in the server's buffer with its
 * length, and a reply went out exactly when its mark is set. The reasons
 * are channel_read's (test_chanread.c: a killed thread acts on the kill
 * only on its way back to ring 3, so a system call finishes what it took)
 * plus channel_read_wait's: a request handed straight to the waiting
 * reader is no longer queued anywhere, so a cancelled wait that was handed
 * one delivers it all the same (kernel/object/channel.c), and the mark is
 * written in the same call as the reply, before the wait.
 *
 * The server is a real user program, bin/utest in its mode "rw-reader"
 * (user/tests/utest/replywait.c, which has the shared layout below): each
 * call answers the request before (its txid word and its entry's index,
 * the mark in that entry) and takes the next straight into the next entry
 * of a VMO, the length written by the kernel into the entry's first word;
 * it counts it in the VMO's header from user code once the call returned.
 * We write big requests one by one, mostly while it waits (so most are
 * handed over, some queued), and kill it at a random point, then check:
 * the entries filled are whole and in order, the ones after are empty, the
 * rest is still queued in order, the replies that came are exactly those
 * whose marks are set, and nothing is missing or there twice. A round
 * whose last filled entry was never counted shows the kill landed inside
 * the call after it had taken a request; the test requires some. */
#include <jam/channel.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/startup.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/userboot.h>
#include <jam/vmo.h>

/* The log VMO, as user/tests/utest/replywait.c lays it out: a header page
 * (uint32 counted; uint32 ready), then RW_ENTRIES entries of RW_STRIDE
 * bytes: the length at RW_LEN, the mark of the reply to it at RW_MARK, the
 * request's bytes at RW_DATA. */
#define RW_ENTRIES 48
#define RW_STRIDE  (20u << 10)
#define RW_LEN     0
#define RW_MARK    8
#define RW_DATA    64
#define RW_MSG_MAX (16u << 10)
#define RW_SIZE    (PAGE_SIZE + RW_ENTRIES * RW_STRIDE)

#define REQUESTS 32   /* written per round at most (fewer than the entries) */
#define ROUNDS   24

static uint64_t entry_off(unsigned k)
{
    return PAGE_SIZE + (uint64_t)k * RW_STRIDE;
}

/* Request seq's length (4 KiB to 16 KiB) and its byte i: its first word is
 * seq, which the reply carries back as its txid. */
static uint32_t req_len(uint32_t seq)
{
    return 4096 + (seq * 2654435761u) % (RW_MSG_MAX - 4096 + 1);
}

static uint8_t req_byte(uint32_t seq, uint32_t i)
{
    return i < 4 ? (uint8_t)(seq >> (8 * i)) : (uint8_t)(seq * 131 + i * 7);
}

static bool req_is(const uint8_t *buf, uint32_t len, uint32_t seq)
{
    if (len != req_len(seq))
        return false;
    for (uint32_t i = 0; i < len; i++)
        if (buf[i] != req_byte(seq, i))
            return false;
    return true;
}

static uint32_t log_u32(struct vmo *v, uint64_t off)
{
    uint32_t w = 0;
    KT_EQ(vmo_read(v, off, &w, sizeof(w)), OK);
    return w;
}

static uint64_t log_u64(struct vmo *v, uint64_t off)
{
    uint64_t w = 0;
    KT_EQ(vmo_read(v, off, &w, sizeof(w)), OK);
    return w;
}

/* One round's objects. */
struct round {
    struct channel *mine;      /* we write requests here and read the replies */
    struct channel *theirs;    /* the server's end; we hold a handle too */
    struct khandle  keep;      /* our handle on theirs: its queue outlives the server */
    struct vmo     *log;       /* the server's entries */
    struct process *p;         /* the server */
    uint8_t        *buf;       /* RW_MSG_MAX bytes of scratch */
    uint32_t        written;   /* requests written */
};

static void round_start(struct round *r, struct job *j, uint32_t cpu)
{
    KT_EQ(channel_create(&r->mine, &r->theirs), OK);
    kobject_ref((struct kobject *)r->theirs);
    r->keep = khandle_from_new((struct kobject *)r->theirs, RIGHTS_BASIC | RIGHTS_IO);
    KT_EQ(vmo_create(RW_SIZE, 0, &r->log), OK);
    KT_EQ(vmo_commit(r->log, 0, RW_SIZE), OK);
    kobject_ref(vmo_kobject(r->log));
    struct userboot_handle x[2] = {
        { SR_USER, khandle_from_new((struct kobject *)r->theirs, RIGHTS_BASIC | RIGHTS_IO) },
        { SR_USER + 1, khandle_from_new(vmo_kobject(r->log), RIGHTS_BASIC | RIGHTS_IO |
                                                             RIGHT_MAP) },
    };
    r->buf = kmalloc(RW_MSG_MAX);
    KT_ASSERT(r->buf);
    r->written = 0;
    const char *argv[] = { "utest", "rw-reader" };
    cpumask_t m;
    cpumask_one(&m, cpu);
    KT_EQ(userboot_spawn("bin/utest", argv, 2, j, x, 2, &m, &r->p), OK);
    uint64_t d = uptime_ns() + kt_patience_ms(10000) * NS_PER_MS;
    while (!log_u32(r->log, 4)) {   /* ready: mapped, about to take the first */
        KT_ASSERT(uptime_ns() < d);
        thread_sleep_ms(1);
    }
}

/* Write requests 1.. until `target` is written, after each mostly waiting
 * (bounded) until the server has counted all but `lag` of them, so the
 * next usually finds it waiting; then let it run `spin` loops and kill it. */
static void round_run(struct round *r, uint32_t target, uint32_t lag, uint32_t spin)
{
    for (uint32_t seq = 1; seq <= target; seq++) {
        for (uint32_t i = 0; i < req_len(seq); i++)
            r->buf[i] = req_byte(seq, i);
        KT_EQ(channel_write(r->mine, r->buf, req_len(seq), NULL, 0), OK);
        r->written = seq;
        uint64_t d = uptime_ns() + kt_patience_ms(10000) * NS_PER_MS;
        while (seq < target && log_u32(r->log, 0) + lag < seq) {
            KT_ASSERT(uptime_ns() < d);
            if (cpu_count == 1)
                thread_sleep_ms(1);   /* the server shares our CPU */
        }
    }
    for (volatile uint32_t i = 0; i < spin; i++)
        ;
    process_kill(r->p, PROCESS_KILLED_CODE, true);
    KT_EQ(object_wait_one(process_kobject(r->p), SIG_TERMINATED,
                          uptime_ns() + kt_patience_ms(10000) * NS_PER_MS, NULL),
          OK);
}

/* The replies on mine: exactly those to entries 0 .. n-1, in order. */
static uint32_t round_replies(struct round *r)
{
    uint32_t n = 0, rep[2], nb = 0;
    status_t st;
    while ((st = channel_read(r->mine, rep, sizeof(rep), &nb, NULL, 0, NULL)) == OK) {
        KT_EQ(nb, sizeof(rep));
        KT_EQ(rep[0], n + 1);   /* the txid word of request n + 1 ... */
        KT_EQ(rep[1], n);       /* ... which was in entry n */
        n++;
    }
    KT_EQ(st, ERR_SHOULD_WAIT);   /* (we still hold the server's end) */
    return n;
}

/* Check everything is in an entry or still queued, and the replies match
 * the marks. Bit 0 of the result: the kill landed inside a call after it
 * took a request; bit 1: after a reply, in the wait for the next. */
static unsigned round_check(struct round *r)
{
    uint32_t counted = log_u32(r->log, 0), filled = 0;
    for (unsigned k = 0; k < RW_ENTRIES; k++) {
        uint32_t len = log_u32(r->log, entry_off(k) + RW_LEN);
        if (!len)
            break;
        KT_ASSERT(len <= RW_MSG_MAX);
        KT_EQ(vmo_read(r->log, entry_off(k) + RW_DATA, r->buf, len), OK);
        KT_ASSERT(req_is(r->buf, len, k + 1));   /* whole, in order */
        filled++;
    }
    for (unsigned k = filled; k < RW_ENTRIES; k++)
        KT_EQ(log_u32(r->log, entry_off(k) + RW_LEN), 0);
    KT_ASSERT(filled == counted || filled == counted + 1);
    uint32_t seq = filled, nb = 0;
    status_t st;
    while ((st = channel_read(r->theirs, r->buf, RW_MSG_MAX, &nb, NULL, 0, NULL)) == OK)
        KT_ASSERT(req_is(r->buf, nb, ++seq));   /* the rest, in order */
    KT_EQ(st, ERR_SHOULD_WAIT);
    KT_EQ(seq, r->written);   /* nothing lost or there twice */
    uint32_t replies = round_replies(r);
    KT_ASSERT(replies <= filled && replies + 1 >= filled);
    for (unsigned k = 0; k < RW_ENTRIES; k++)
        KT_EQ(log_u64(r->log, entry_off(k) + RW_MARK), k < replies ? 1 : 0);
    return (filled == counted + 1) | (filled && replies == filled ? 2 : 0);
}

static void round_end(struct round *r)
{
    khandle_release(&r->keep);
    kobject_unref((struct kobject *)r->mine);
    kobject_unref(vmo_kobject(r->log));
    kobject_unref(process_kobject(r->p));
    kfree(r->buf);
}

KTEST(chanread_reply_wait_kill_loses_nothing)
{
    struct job *j = kt_fresh_job();
    uint32_t me = kt_pin_self(0), cpu = cpu_count > 1 ? 1 : me;
    uint64_t rng = 0x5eed0002ull + uptime_ns();
    unsigned inside = 0, waiting = 0;
    for (unsigned i = 0; i < ROUNDS; i++) {
        struct round r = { 0 };
        round_start(&r, j, cpu);
        uint32_t target = 1 + (uint32_t)(kt_rng(&rng) % REQUESTS);
        uint32_t lag = (uint32_t)(kt_rng(&rng) % 3);   /* 0: it waits for each one */
        round_run(&r, target, lag, (uint32_t)(kt_rng(&rng) % 4000));
        unsigned how = round_check(&r);
        inside += how & 1;
        waiting += how >> 1;
        round_end(&r);
    }
    kt_unpin_self();
    kprintf("ktest chanread_reply_wait: %u of %u kills inside a call after it took a request, "
            "%u in the wait after a reply\n", inside, ROUNDS, waiting);
    KT_IDLE_ASSERT(inside > 0);
    for (uint64_t deadline = uptime_ns() + kt_patience_ms(2000) * NS_PER_MS;
         job_used(j, JOB_LIMIT_PAGES) && uptime_ns() < deadline;)
        thread_sleep_ms(1);   /* dead is not yet freed */
    kt_job_is_empty(j);
    job_unref(j);
}
