/* The debug listing of where threads wait (kernel/debug/threads.c,
 * <jam/threadlook.h>): thread_look names each kind of wait with its object
 * and finds the waiting function in the kernel frames; the user
 * backtrace follows a frame-pointer chain, finds one again past a value
 * that isn't a frame, never loops, and the stack scan and aspace_peek see
 * only pages that are there. */
#include <jam/aspace.h>
#include <jam/channel.h>
#include <jam/event.h>
#include <jam/futex.h>
#include <jam/irq.h>
#include <jam/ksyms.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/object.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/threadlook.h>
#include <jam/time.h>
#include <jam/uentry.h>
#include <jam/usercopy.h>
#include <jam/vmo.h>

#define PG    PAGE_SIZE
#define SBASE 0x40000000ull   /* a test's stack pages (read and write) */
#define CBASE 0x50000000ull   /* its code page (read and run) */

static struct aspace *tl_space(void)
{
    struct aspace *as;
    struct vmo *s, *c;
    KT_EQ(aspace_create(&as), OK);
    KT_EQ(vmo_create(2 * PG, 0, &s), OK);
    KT_EQ(vmo_create(PG, 0, &c), OK);
    uint64_t a = SBASE;
    KT_EQ(aspace_map(as, s, 0, 2 * PG, ASPACE_READ | ASPACE_WRITE | ASPACE_FIXED, &a), OK);
    a = CBASE;
    KT_EQ(aspace_map(as, c, 0, PG, ASPACE_READ | ASPACE_EXEC | ASPACE_FIXED, &a), OK);
    kobject_unref(vmo_kobject(s));   /* the mappings keep them */
    kobject_unref(vmo_kobject(c));
    return as;
}

/* A thread of the test: takes on `as` (if any) as a user thread does, and
 * runs fn. */
struct tl_thread {
    struct aspace *as;
    void         (*fn)(struct tl_thread *);
    uint64_t       arg;
    void          *obj;
    int64_t        st;
    uint8_t        after;   /* its note once its wait was over */
};

static void tl_main(void *p)
{
    struct tl_thread *x = p;
    struct thread *t = current_thread();
    if (x->as) {
        aspace_ref(x->as);
        irq_disable();
        t->aspace = x->as;
        aspace_switch(NULL, x->as);
        irq_enable();
    }
    x->fn(x);
    x->after = t->wait_on;
    if (x->as) {
        irq_disable();
        t->aspace = NULL;
        aspace_switch(x->as, NULL);
        irq_enable();
        aspace_unref(x->as);
    }
}

static void on_futex(struct tl_thread *x)
{
    x->st = futex_wait(x->arg, 0, uptime_ns() + kt_patience_ms(20000) * NS_PER_MS, 0);
}

static void on_channel(struct tl_thread *x)
{
    char buf[16];
    struct chan_read r = { .buf = chan_kbytes(buf, sizeof(buf)) };
    x->st = channel_read_wait(x->obj, &r, DEADLINE_NEVER);
}

static void on_sleep(struct tl_thread *x)
{
    x->st = thread_sleep_cancellable(kt_patience_ms(20000) * NS_PER_MS);
}

/* A note of its own around the sleep: the sleep's note must give it back. */
static void on_nested(struct tl_thread *x)
{
    struct wait_note old = thread_wait_note(WAITON_PORT, 7, 8);
    x->st = thread_sleep_cancellable(kt_patience_ms(20000) * NS_PER_MS);
    struct thread *t = current_thread();
    if (t->wait_on != WAITON_PORT || t->wait_koid != 7 || t->wait_arg != 8)
        x->st = ERR_BAD_STATE;
    thread_wait_unnote(old);
}

static void on_event(struct tl_thread *x)
{
    x->st = object_wait_one(x->obj, SIG_SIGNALED, DEADLINE_NEVER, NULL);
}

/* Look at t until it is blocked with the note `on` (a broken kernel fails
 * here rather than hangs). */
static void until_noted(struct thread *t, uint8_t on, struct thread_look *o)
{
    uint64_t end = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    for (;;) {
        thread_look(t, o);
        if ((o->state == T_BLOCKED && !o->on_cpu && o->wait_on == on) || uptime_ns() > end)
            break;
        thread_sleep_ms(1);
    }
    KT_EQ(o->state, T_BLOCKED);
    KT_EQ(o->wait_on, on);
}

/* Is a kernel frame of o in a function whose name starts with fn? */
static bool in_frames(const struct thread_look *o, const char *fn)
{
    for (uint32_t i = 0; i < o->nk; i++) {
        uint64_t off;
        const char *name = ksym_lookup(o->kframes[i], &off);
        if (name && strlen(name) >= strlen(fn) && !memcmp(name, fn, strlen(fn)))
            return true;
    }
    return false;
}

KTEST(threadlook_names_each_wait)
{
    struct aspace *as = tl_space();
    struct channel *a, *b;
    KT_EQ(channel_create(&a, &b), OK);
    struct event *ev;
    KT_EQ(event_create(&ev), OK);

    struct tl_thread xs[5] = {
        { .as = as, .fn = on_futex, .arg = SBASE + 0x40 },
        { .fn = on_channel, .obj = a },
        { .fn = on_sleep },
        { .fn = on_nested },
        { .fn = on_event, .obj = &ev->base },
    };
    struct thread *ts[5];
    for (int i = 0; i < 5; i++) {
        ts[i] = thread_create("kt-look", tl_main, &xs[i], PRIO_DEFAULT);
        KT_ASSERT(ts[i]);
    }

    struct thread_look o;
    uint64_t now = uptime_ns();
    until_noted(ts[0], WAITON_FUTEX, &o);
    KT_EQ(o.wait_arg, SBASE + 0x40);
    KT_ASSERT(o.deadline_ns != DEADLINE_NEVER && o.deadline_ns > now);
    KT_ASSERT(in_frames(&o, "futex_wait"));
    KT_ASSERT(!o.have_user);   /* a kernel thread: no user frame to read */

    until_noted(ts[1], WAITON_READ, &o);
    KT_EQ(o.wait_koid, ((struct kobject *)a)->koid);
    KT_ASSERT(o.deadline_ns == DEADLINE_NEVER);
    KT_ASSERT(in_frames(&o, "channel_read_wait"));

    until_noted(ts[2], WAITON_SLEEP, &o);
    KT_ASSERT(o.deadline_ns != DEADLINE_NEVER && o.deadline_ns > now);
    KT_EQ(o.wait_arg, o.deadline_ns);
    KT_ASSERT(in_frames(&o, "thread_sleep_cancellable"));

    until_noted(ts[3], WAITON_SLEEP, &o);   /* the inner note shows */

    until_noted(ts[4], WAITON_OBJECT, &o);
    KT_EQ(o.wait_koid, ev->base.koid);
    KT_EQ(o.wait_arg, SIG_SIGNALED);
    KT_ASSERT(in_frames(&o, "object_wait_one") || in_frames(&o, "wait_one"));   /* a tail call */

    /* End every wait; each note is gone after. */
    thread_cancel(ts[0]);
    KT_EQ(channel_write(b, "hi", 2, NULL, 0), OK);
    thread_cancel(ts[2]);
    thread_cancel(ts[3]);
    KT_EQ(event_signal(ev, 0, SIG_SIGNALED), OK);
    for (int i = 0; i < 5; i++)
        thread_join(ts[i]);
    KT_EQ(xs[0].st, ERR_CANCELED);
    KT_EQ(xs[1].st, OK);
    KT_EQ(xs[2].st, ERR_CANCELED);
    KT_EQ(xs[3].st, ERR_CANCELED);
    KT_EQ(xs[4].st, OK);
    for (int i = 0; i < 5; i++)
        KT_EQ(xs[i].after, WAITON_NONE);

    kobject_unref(&ev->base);
    kobject_unref((struct kobject *)a);
    kobject_unref((struct kobject *)b);
    aspace_unref(as);
}

/* ---- the user stack -------------------------------------------------------- */

/* The fake stack: written through the address space (which installs its
 * page's entry), and the code page read once (which installs its). The
 * second stack page is never touched. */
struct fake_word {
    uint64_t off, val;
};

static const struct fake_word fake[] = {
    { 0x100, 0x1234 },                 /* rsp: not a frame record */
    { 0x108, CBASE + 0x10 },           /* a return address with no record */
    { 0x110, 0x1234 },
    { 0x200, SBASE + 0x300 },          /* a record: the next one */
    { 0x208, CBASE + 0x20 },
    { 0x300, 0 },                      /* the last record */
    { 0x308, CBASE + 0x30 },
    { 0x400, SBASE + 0x500 },          /* two records that point at each other */
    { 0x408, CBASE + 0x40 },
    { 0x500, SBASE + 0x400 },
    { 0x508, CBASE + 0x50 },
    { 0xe00, 0x9999 },                 /* a record whose saved rbp isn't one */
    { 0xe08, CBASE + 0x60 },
    { 0xe40, 0 },                      /* stale words between: a zero over */
    { 0xe48, CBASE + 0x68 },           /* a code address isn't a record */
    { 0xe80, SBASE + 0xec0 },          /* the next record */
    { 0xe88, CBASE + 0x70 },
    { 0xec0, 0 },                      /* the last */
    { 0xec8, CBASE + 0x78 },
};
/* The loop's walk scans on from 0x510 and must find nothing. */
_Static_assert(0x510 + LOOK_SCAN * 8 <= 0xe00, "the loop case's scan reaches the next case");

static void fill_stack(struct tl_thread *x)
{
    for (size_t i = 0; i < sizeof(fake) / sizeof(fake[0]); i++)
        if (copy_to_user(SBASE + fake[i].off, &fake[i].val, 8) != OK)
            x->st = ERR_IO;
    uint64_t w;
    if (copy_from_user(&w, CBASE, 8) != OK)
        x->st = ERR_IO;
}

KTEST(threadlook_user_stack)
{
    struct aspace *as = tl_space();
    struct tl_thread x = { .as = as, .fn = fill_stack };
    thread_join(thread_create("kt-look", tl_main, &x, PRIO_DEFAULT));
    KT_EQ(x.st, OK);

    uint64_t w = 0;
    KT_ASSERT(aspace_peek(as, SBASE + 0x200, &w));
    KT_EQ(w, SBASE + 0x300);
    KT_ASSERT(!aspace_peek(as, SBASE + 0x201, &w));     /* unaligned */
    KT_ASSERT(!aspace_peek(as, SBASE + PG, &w));        /* never touched */
    KT_ASSERT(!aspace_peek(as, 0x60000000ull, &w));     /* nothing mapped */
    KT_ASSERT(!aspace_peek(as, USER_TOP + 0x1000, &w)); /* not a user address */
    KT_ASSERT(aspace_peek_exec(as, CBASE + 0x20));
    KT_ASSERT(!aspace_peek_exec(as, SBASE + 0x200));

    uint64_t f[LOOK_UFRAMES];
    bool s[LOOK_UFRAMES];
    /* rbp a record: rip, then each record's return address. */
    uint32_t n = user_backtrace(as, CBASE + 5, SBASE + 0x100, SBASE + 0x200, f, s,
                                LOOK_UFRAMES);
    KT_EQ(n, 3);
    KT_EQ(f[0], CBASE + 5);
    KT_EQ(f[1], CBASE + 0x20);
    KT_EQ(f[2], CBASE + 0x30);
    KT_ASSERT(!s[0] && !s[1] && !s[2]);
    /* rbp not a record (code without frame pointers used it): the first
     * record above rsp is found again. */
    memset(f, 0, sizeof(f));
    n = user_backtrace(as, CBASE + 5, SBASE + 0x100, 0x4242, f, s, LOOK_UFRAMES);
    KT_EQ(n, 3);
    KT_EQ(f[1], CBASE + 0x20);
    KT_EQ(f[2], CBASE + 0x30);
    KT_ASSERT(s[1] && !s[2]);
    /* A loop of records ends the walk where it turns back down (nothing
     * above to go on from). */
    n = user_backtrace(as, CBASE + 5, SBASE + 0x400, SBASE + 0x400, f, NULL, LOOK_UFRAMES);
    KT_EQ(n, 3);
    KT_EQ(f[1], CBASE + 0x40);
    KT_EQ(f[2], CBASE + 0x50);
    /* A record whose saved rbp isn't one: the walk goes on from the next
     * record above it (not from a zero over a code address), then along
     * its chain. */
    n = user_backtrace(as, CBASE + 5, SBASE + 0xe00, SBASE + 0xe00, f, s, LOOK_UFRAMES);
    KT_EQ(n, 4);
    KT_EQ(f[1], CBASE + 0x60);
    KT_EQ(f[2], CBASE + 0x70);
    KT_EQ(f[3], CBASE + 0x78);
    KT_ASSERT(!s[1] && s[2] && !s[3]);
    /* cap is honoured. */
    KT_EQ(user_backtrace(as, CBASE + 5, SBASE + 0x100, SBASE + 0x200, f, s, 2), 2);

    /* The scan: the words in the LOOK_WORDS above rsp that point at code. */
    uint32_t off[16];
    uint64_t val[16];
    n = user_stack_scan(as, SBASE + 0x100, LOOK_WORDS, off, val, 8);
    KT_EQ(n, 2);
    KT_EQ(off[0], 0x8);
    KT_EQ(val[0], CBASE + 0x10);
    KT_EQ(off[1], 0x108);
    KT_EQ(val[1], CBASE + 0x20);
    /* It stops at a page that isn't there. */
    KT_EQ(user_stack_scan(as, SBASE + PG - 8, LOOK_WORDS, off, val, 8), 0);
    /* Its window and its cap. */
    KT_EQ(user_stack_scan(as, SBASE + 0x100, 2, off, val, 8), 1);
    KT_EQ(user_stack_scan(as, SBASE + 0x100, LOOK_WORDS, off, val, 1), 1);
    KT_EQ(user_stack_scan(as, SBASE + 0x100, 512, off, val, 16), 9);   /* to the page's end */

    aspace_unref(as);
}
