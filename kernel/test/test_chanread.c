/* A thread killed inside channel_read loses no message: the request a
 * service reads straight into its state (<svcstate.h>'s slots) is either
 * still queued or wholly in the reader's buffer with its length, never
 * gone. The reason is in kernel/arch/x86_64/uentry.c (return_to_user_work):
 * a cancelled thread acts on the cancel only on its way back to ring 3,
 * so a system call that has taken a message off the queue always finishes
 * copying it out; channel_read never waits, and a copy's page fault
 * doesn't take a cancellable wait.
 *
 * The reader is a real user program, bin/utest in its mode
 * "svcstate-chanread" (user/tests/utest/svcstate.c, which has the shared
 * layout below): it reads each message straight into the next entry of a
 * VMO (the bytes, and its length written by the kernel's read into the
 * entry's first word), then counts it in the VMO's header from user code.
 * We queue a backlog of big messages (the copies are long, so a kill
 * usually lands inside a read) and kill the program once its count
 * reaches a random point, then check: the entries filled are whole and in
 * order, the ones after are empty, the rest of the backlog is still
 * queued, in order, and nothing is missing or there twice. A round whose
 * last filled entry was never counted by the program shows the kill
 * landed inside that read (its user code never ran after it); the test
 * requires some such rounds, so it really tests the window. */
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

/* The log VMO, as user/tests/utest/svcstate.c lays it out: a header page
 * (uint32 taken: entries the program counted; uint32 ready), then
 * CR_ENTRIES entries of CR_STRIDE bytes: the length at offset 0, the
 * message's bytes at CR_DATA. */
#define CR_ENTRIES 48
#define CR_STRIDE  (20u << 10)
#define CR_DATA    64
#define CR_MSG_MAX (16u << 10)
#define CR_SIZE    (PAGE_SIZE + CR_ENTRIES * CR_STRIDE)

#define BACKLOG 32   /* messages queued per round (fewer than the entries) */
#define ROUNDS  24

static uint64_t entry_off(unsigned k)
{
    return PAGE_SIZE + (uint64_t)k * CR_STRIDE;
}

/* Message seq's length (4 KiB to 16 KiB) and its byte i. */
static uint32_t msg_len(uint32_t seq)
{
    return 4096 + (seq * 2654435761u) % (CR_MSG_MAX - 4096 + 1);
}

static uint8_t msg_byte(uint32_t seq, uint32_t i)
{
    return i < 4 ? (uint8_t)(seq >> (8 * i)) : (uint8_t)(seq * 131 + i * 7);
}

/* buf holds message seq exactly (len bytes). */
static bool msg_is(const uint8_t *buf, uint32_t len, uint32_t seq)
{
    if (len != msg_len(seq))
        return false;
    for (uint32_t i = 0; i < len; i++)
        if (buf[i] != msg_byte(seq, i))
            return false;
    return true;
}

static uint32_t header_word(struct vmo *v, unsigned i)
{
    uint32_t w = 0;
    KT_EQ(vmo_read(v, i * 4, &w, sizeof(w)), OK);
    return w;
}

/* One round's objects. */
struct round {
    struct channel *mine;     /* we write here */
    struct channel *theirs;   /* the program reads here; we hold a handle too */
    struct khandle  keep;     /* our handle on theirs: its queue outlives the program */
    struct vmo     *log;      /* the program's entries */
    struct process *p;        /* the reader */
    uint8_t        *buf;      /* CR_MSG_MAX bytes of scratch */
};

static void round_start(struct round *r, struct job *j, uint32_t cpu)
{
    KT_EQ(channel_create(&r->mine, &r->theirs), OK);
    kobject_ref((struct kobject *)r->theirs);
    r->keep = khandle_from_new((struct kobject *)r->theirs, RIGHTS_BASIC | RIGHTS_IO);
    KT_EQ(vmo_create(CR_SIZE, 0, &r->log), OK);
    KT_EQ(vmo_commit(r->log, 0, CR_SIZE), OK);   /* the pages are there before the reads */
    kobject_ref(vmo_kobject(r->log));
    struct userboot_handle x[2] = {
        { SR_USER, khandle_from_new((struct kobject *)r->theirs, RIGHTS_BASIC | RIGHTS_IO) },
        { SR_USER + 1, khandle_from_new(vmo_kobject(r->log), RIGHTS_BASIC | RIGHTS_IO |
                                                             RIGHT_MAP) },
    };
    /* The whole backlog is queued before the reader starts: it never
     * waits (and so is never killed in a wait) until it has read it all. */
    r->buf = kmalloc(CR_MSG_MAX);
    KT_ASSERT(r->buf);
    for (uint32_t seq = 1; seq <= BACKLOG; seq++) {
        for (uint32_t i = 0; i < msg_len(seq); i++)
            r->buf[i] = msg_byte(seq, i);
        KT_EQ(channel_write(r->mine, r->buf, msg_len(seq), NULL, 0), OK);
    }
    const char *argv[] = { "utest", "svcstate-chanread" };
    cpumask_t m;
    cpumask_one(&m, cpu);
    KT_EQ(userboot_spawn("bin/utest", argv, 2, j, x, 2, &m, &r->p), OK);
}

/* Kill the reader as soon as it has counted `target` messages (BACKLOG:
 * it has read them all and waits for more), after `spin` more loops. */
static void round_kill(struct round *r, uint32_t target, uint32_t spin)
{
    uint64_t deadline = uptime_ns() + kt_patience_ms(10000) * NS_PER_MS;
    while (header_word(r->log, 0) < target) {
        KT_ASSERT(uptime_ns() < deadline);
        if (cpu_count == 1)
            thread_sleep_ms(1);   /* the reader shares our CPU */
    }
    for (volatile uint32_t i = 0; i < spin; i++)
        ;
    process_kill(r->p, PROCESS_KILLED_CODE, true);
    KT_EQ(object_wait_one(process_kobject(r->p), SIG_TERMINATED,
                          uptime_ns() + kt_patience_ms(10000) * NS_PER_MS, NULL),
          OK);
}

/* Check everything is either in an entry or still queued; true if the
 * kill landed inside a read. */
static bool round_check(struct round *r)
{
    uint32_t taken = header_word(r->log, 0), filled = 0;
    for (unsigned k = 0; k < CR_ENTRIES; k++) {
        uint32_t len = 0;
        KT_EQ(vmo_read(r->log, entry_off(k), &len, sizeof(len)), OK);
        if (!len)
            break;
        KT_ASSERT(len <= CR_MSG_MAX);
        KT_EQ(vmo_read(r->log, entry_off(k) + CR_DATA, r->buf, len), OK);
        KT_ASSERT(msg_is(r->buf, len, k + 1));   /* whole, in order */
        filled++;
    }
    for (unsigned k = filled; k < CR_ENTRIES; k++) {
        uint32_t len = 1;
        KT_EQ(vmo_read(r->log, entry_off(k), &len, sizeof(len)), OK);
        KT_EQ(len, 0);
    }
    KT_ASSERT(filled == taken || filled == taken + 1);
    uint32_t seq = filled, nb = 0;
    status_t st;
    while ((st = channel_read(r->theirs, r->buf, CR_MSG_MAX, &nb, NULL, 0, NULL)) == OK)
        KT_ASSERT(msg_is(r->buf, nb, ++seq));   /* the rest, in order */
    KT_EQ(st, ERR_SHOULD_WAIT);   /* the queue's end (our end of the pair is still open)... */
    KT_EQ(seq, BACKLOG);          /* ...and nothing lost or there twice */
    return filled == taken + 1;
}

static void round_end(struct round *r)
{
    khandle_release(&r->keep);
    kobject_unref((struct kobject *)r->mine);
    kobject_unref(vmo_kobject(r->log));
    kobject_unref(process_kobject(r->p));
    kfree(r->buf);
}

KTEST(chanread_kill_loses_nothing)
{
    struct job *j = kt_fresh_job();
    uint32_t me = kt_pin_self(0), cpu = cpu_count > 1 ? 1 : me;
    uint64_t rng = 0x5eed0001ull + uptime_ns();
    unsigned inside = 0;
    for (unsigned i = 0; i < ROUNDS; i++) {
        struct round r = { 0 };
        round_start(&r, j, cpu);
        /* Mostly in the middle of the backlog; every eighth round once it
         * is all read and the reader waits for more. */
        uint32_t target = i % 8 == 7 ? BACKLOG : 1 + (uint32_t)(kt_rng(&rng) % (BACKLOG - 1));
        round_kill(&r, target, (uint32_t)(kt_rng(&rng) % 200));
        inside += round_check(&r);
        round_end(&r);
    }
    kt_unpin_self();
    kprintf("ktest chanread: %u of %u kills landed inside a read\n", inside, ROUNDS);
    KT_IDLE_ASSERT(inside > 0);
    for (uint64_t deadline = uptime_ns() + kt_patience_ms(2000) * NS_PER_MS;
         job_used(j, JOB_LIMIT_PAGES) && uptime_ns() < deadline;)
        thread_sleep_ms(1);   /* dead is not yet freed */
    kt_job_is_empty(j);
    job_unref(j);
}
