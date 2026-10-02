/* utest: libos's cooperative tasks (<jam/task.h>): tasks that yield and
 * wait run in turn as a loop would run them, a wait ends at its deadline
 * or at a kick, a task can start another, the slots are a limit and come
 * back, a set's wake cap bounds every wait, and destroying a set abandons
 * what still runs. The stack canary is not tested here: overrunning a
 * stack on purpose would damage the heap next to it. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <jam/task.h>
#include <os.h>
#include "utest.h"

#define FAR       (now() + 3600 * NS_PER_S)   /* a deadline no test reaches */
#define SHORT_NS  (200 * NS_PER_MS)   /* long enough that no check races it */

static struct task_set *set;   /* the set of the test running */
static char trail[32];         /* what the tasks did, in order */
static unsigned trail_n;
static struct task *seen_self;   /* a task's own task_current */
static void *seen_arg;           /* and its task_arg */

static void mark(char c)
{
    if (trail_n < sizeof(trail) - 1)
        trail[trail_n++] = c;
    trail[trail_n] = 0;
}

static void reset_trail(void)
{
    trail_n = 0;
    trail[0] = 0;
}

/* a: runs, yields a round, waits SHORT_NS, ends. */
static void task_a(void *arg)
{
    seen_self = task_current(set);
    seen_arg = task_arg(seen_self);
    (void)arg;
    mark('a');
    task_yield(set);
    mark('A');
    task_wait(set, now() + SHORT_NS);
    mark('1');
}

/* b: runs, waits until a kick (its deadline is far away), ends. */
static void task_b(void *arg)
{
    (void)arg;
    mark('b');
    task_wait(set, FAR);
    mark('B');
}

bool t_tasks_yield_and_wait(void)
{
    struct task_opts o = { .max_tasks = 4, .stack_bytes = 8192 };
    set = task_set_create(&o);
    CHECK(set);
    reset_trail();
    CHECK(task_current(set) == NULL);
    task_wait(set, FAR);   /* from the loop: returns at once */
    struct task *a = task_start(set, task_a, (void *)0x1234);
    CHECK(a);
    CHECK(seen_self == a && seen_arg == (void *)0x1234);
    CHECK(task_start(set, task_b, NULL));
    CHECK(!strcmp(trail, "ab"));   /* each ran until it first waited */
    CHECK_EQ(task_live(set), 2);
    CHECK(task_current(set) == NULL);

    CHECK(task_run(set));          /* a's yield is over; b waits for a kick */
    CHECK(!strcmp(trail, "abA"));
    uint64_t t0 = now(), next = task_next_wake(set, UINT64_MAX);
    CHECK(next > t0 && next <= t0 + SHORT_NS);   /* a's deadline */
    CHECK(!task_run(set));         /* nobody's wait is over yet */

    CHECK_ST(jam_nanosleep(next + NS_PER_MS), OK);
    CHECK(task_run(set));
    CHECK(!strcmp(trail, "abA1"));
    CHECK_EQ(task_live(set), 1);
    CHECK(task_next_wake(set, 5) == 5);   /* b's far deadline is later */

    task_kick(set);
    CHECK(task_next_wake(set, UINT64_MAX) <= now());   /* b may go on now */
    CHECK(task_run(set));
    CHECK(!strcmp(trail, "abA1B"));
    CHECK_EQ(task_live(set), 0);
    CHECK(!task_run(set));
    CHECK(task_set_destroy(set));
    return true;
}

static void task_child(void *arg)
{
    (void)arg;
    mark('c');
}

/* p: starts a child from inside a task: it runs at the loop's next round. */
static void task_parent(void *arg)
{
    struct task **child = arg;
    mark('p');
    *child = task_start(set, task_child, NULL);
    mark('P');
}

/* Marks its letter, waits once with a far deadline, ends. */
static void task_sleeper(void *arg)
{
    mark(*(const char *)arg);
    task_wait(set, FAR);
}

/* Waits with a far deadline, marking each time it looks again. */
static void task_looker(void *arg)
{
    (void)arg;
    for (;;) {
        task_wait(set, FAR);
        mark('l');
    }
}

bool t_tasks_start_slots_cap(void)
{
    /* A task started from a task. */
    struct task_opts o = { .max_tasks = 2, .stack_bytes = TASK_STACK_MIN };
    set = task_set_create(&o);
    CHECK(set);
    reset_trail();
    struct task *child = NULL;
    CHECK(task_start(set, task_parent, &child));
    CHECK(child);
    CHECK(!strcmp(trail, "pP"));   /* the child hasn't run yet */
    CHECK_EQ(task_live(set), 1);
    CHECK(task_run(set));
    CHECK(!strcmp(trail, "pPc"));
    CHECK_EQ(task_live(set), 0);

    /* Two slots: a third task waits for one to come back. */
    reset_trail();
    CHECK(task_start(set, task_sleeper, "x"));
    CHECK(task_start(set, task_sleeper, "y"));
    CHECK(task_start(set, task_sleeper, "z") == NULL);
    task_kick(set);
    CHECK(task_run(set));   /* both end */
    CHECK_EQ(task_live(set), 0);
    CHECK(task_start(set, task_sleeper, "z"));
    CHECK(!strcmp(trail, "xyz"));
    CHECK(!task_set_destroy(set));   /* z never ended: abandoned */

    /* Bad options are refused. */
    struct task_opts bad = { .max_tasks = 0 };
    CHECK(task_set_create(&bad) == NULL);
    bad = (struct task_opts){ .max_tasks = 1, .stack_bytes = TASK_STACK_MIN + 8 };
    CHECK(task_set_create(&bad) == NULL);
    bad = (struct task_opts){ .max_tasks = TASK_MAX_TASKS + 1 };
    CHECK(task_set_create(&bad) == NULL);

    /* The wake cap: a far deadline still looks again after wake_cap_ns. */
    struct task_opts capped = { .max_tasks = 1, .wake_cap_ns = SHORT_NS };
    set = task_set_create(&capped);
    CHECK(set);
    reset_trail();
    uint64_t t0 = now();
    CHECK(task_start(set, task_looker, NULL));
    uint64_t next = task_next_wake(set, UINT64_MAX);
    CHECK(next > t0 && next <= now() + SHORT_NS);
    CHECK_ST(jam_nanosleep(next), OK);
    CHECK(task_run(set));   /* looked again, and waits on */
    CHECK(!strcmp(trail, "l"));
    CHECK_EQ(task_live(set), 1);
    CHECK(!task_set_destroy(set));
    return true;
}
