/* libos: cooperative tasks (<jam/task.h> has the model and the rules).
 *
 * Switching: task_switch (assembly, below) pushes the callee-saved
 * registers on the current stack, saves its stack pointer, loads the
 * other's and pops its registers. A new task's stack is laid out so that
 * the first switch to it "returns" into libos_task_first. The loop runs on the
 * thread's own stack; while a task runs, the loop's stack pointer waits in
 * the set (loop_sp).
 *
 * Slots: a set is an array of max_tasks slots. A slot's stack is
 * allocated the first time a task uses it and kept for the slot's next
 * task, so a loop that starts a task per request allocates nothing once
 * warm. A slot is free again as soon as its task's function has returned
 * and the task has switched back to the loop for the last time. */
#include <jam/task.h>
#include <os.h>

#define CANARY_WORDS 8
#define CANARY       0x7a5c0de57ac0ffeeull

struct task {
    struct task_set *set;          /* the set the slot belongs to */
    bool     live;                 /* started and not ended: false is a free slot */
    bool     done;                 /* its function returned (it switches out one last time) */
    void   (*fn)(void *arg);       /* what it runs */
    void    *arg;
    uint64_t sp;                   /* its stack pointer while switched out */
    uint8_t *stack;                /* stack_bytes, malloc'd; kept for the slot's next task */
    uint64_t wake_at;              /* runs again at this uptime (ns), */
    uint64_t seen;                 /* or as soon as there was a task_kick since it last ran */
};

struct task_set {
    struct task_opts opts;         /* as created (stack_bytes filled in) */
    struct task *cur;              /* the task running, NULL: the loop */
    uint64_t     loop_sp;          /* the loop's stack pointer while a task runs */
    uint64_t     kicks;            /* bumped by task_kick: waiting tasks look again */
    bool         overflowed;       /* a canary was found damaged */
    struct task  slots[];          /* opts.max_tasks of them */
};

/* task_switch(save, to): save the callee-saved registers and the stack
 * pointer into *save, continue on the stack `to` (System V AMD64: rbx,
 * rbp, r12-r15 are the callee's to keep; the rest a call may change).
 * The FPU control words are callee-saved too, but nothing in Jam OS
 * changes them, so they are left alone. */
void libos_task_switch(uint64_t *save, uint64_t to);
__asm__(".text\n"
        ".globl libos_task_switch\n"
        ".type libos_task_switch, @function\n"
        "libos_task_switch:\n"
        "    push %rbp\n    push %rbx\n    push %r12\n"
        "    push %r13\n    push %r14\n    push %r15\n"
        "    mov %rsp, (%rdi)\n"
        "    mov %rsi, %rsp\n"
        "    pop %r15\n    pop %r14\n    pop %r13\n"
        "    pop %r12\n    pop %rbx\n    pop %rbp\n"
        "    ret\n"
        ".size libos_task_switch, .-libos_task_switch\n");

struct task_set *task_set_create(const struct task_opts *o)
{
    if (!o || o->max_tasks == 0 || o->max_tasks > TASK_MAX_TASKS)
        return NULL;
    uint32_t stack = o->stack_bytes ? o->stack_bytes : TASK_STACK_DEFAULT;
    if (stack < TASK_STACK_MIN || stack % 16)
        return NULL;
    struct task_set *s = calloc(1, sizeof(*s) + o->max_tasks * sizeof(struct task));
    if (!s)
        return NULL;
    s->opts = *o;
    s->opts.stack_bytes = stack;
    for (uint32_t i = 0; i < o->max_tasks; i++)
        s->slots[i].set = s;
    return s;
}

bool task_set_destroy(struct task_set *s)
{
    if (!s || s->cur)
        return false;
    bool all = true;
    for (uint32_t i = 0; i < s->opts.max_tasks; i++) {
        struct task *t = &s->slots[i];
        if (t->live) {
            all = false;   /* its frames are on that stack: keep it */
            continue;
        }
        free(t->stack);
        t->stack = NULL;
    }
    if (all)
        free(s);
    return all;
}

/* The first code a new task runs: task_switch "returns" into
 * libos_task_first (below), which passes the task (left in rbx by the
 * first switch's pops) on to here. Not static: the assembly jumps to it. */
_Noreturn void libos_task_entry(struct task *t);
void libos_task_first(void);
_Noreturn void libos_task_entry(struct task *t)
{
    t->fn(t->arg);
    t->done = true;
    t->set->cur = NULL;
    libos_task_switch(&t->sp, t->set->loop_sp);
    __builtin_unreachable();   /* a done task is never switched to again */
}
__asm__(".text\n"
        ".globl libos_task_first\n"
        ".type libos_task_first, @function\n"
        "libos_task_first:\n"
        "    mov %rbx, %rdi\n"
        "    jmp libos_task_entry\n"
        ".size libos_task_first, .-libos_task_first\n");

/* Has t's canary changed? */
static bool canary_broken(const struct task *t)
{
    const uint64_t *c = (const uint64_t *)t->stack;
    for (int i = 0; i < CANARY_WORDS; i++)
        if (c[i] != CANARY)
            return true;
    return false;
}

/* Run t until it waits or ends; then check its stack and free its slot
 * if it ended. */
static void run_one(struct task_set *s, struct task *t)
{
    t->seen = s->kicks;
    s->cur = t;
    libos_task_switch(&s->loop_sp, t->sp);
    if (canary_broken(t) && !s->overflowed) {
        s->overflowed = true;
        if (s->opts.overflow)
            s->opts.overflow(t->arg);
    }
    if (t->done)
        t->live = false;   /* free; the stack stays for the next task in the slot */
}

/* Lay out a fresh stack so that the first switch to it enters
 * libos_task_entry. */
static void prepare_stack(struct task_set *s, struct task *t)
{
    uint64_t *c = (uint64_t *)t->stack;
    for (int i = 0; i < CANARY_WORDS; i++)
        c[i] = CANARY;
    /* From the top: a zero return address for libos_task_entry (which
     * libos_task_first jumps to, so it is entered with rsp + 8 16-byte
     * aligned, as after a call), libos_task_first for task_switch's ret,
     * and the six registers it pops: rbp (-3) zero, ending backtraces;
     * rbx (-4) the task; r12-r15 zero. */
    uint64_t *top = (uint64_t *)(t->stack + s->opts.stack_bytes);
    top[-1] = 0;
    top[-2] = (uint64_t)(uintptr_t)libos_task_first;
    for (int i = 3; i <= 8; i++)
        top[-i] = 0;
    top[-4] = (uint64_t)(uintptr_t)t;
    t->sp = (uint64_t)(uintptr_t)&top[-8];
}

struct task *task_start(struct task_set *s, void (*fn)(void *arg), void *arg)
{
    struct task *t = NULL;
    for (uint32_t i = 0; i < s->opts.max_tasks && !t; i++)
        if (!s->slots[i].live)
            t = &s->slots[i];
    if (!t || !fn)
        return NULL;
    if (!t->stack && !(t->stack = malloc(s->opts.stack_bytes)))
        return NULL;
    prepare_stack(s, t);
    t->live = true;
    t->done = false;
    t->fn = fn;
    t->arg = arg;
    t->wake_at = 0;
    t->seen = s->kicks;
    if (!s->cur)
        run_one(s, t);   /* from a task: wake_at 0, so the next task_run runs it */
    return t;
}

/* May t go on: its deadline passed, or something happened since it ran? */
static bool ready(const struct task *t, uint64_t kicks, uint64_t now)
{
    return t->live && !t->done && (now >= t->wake_at || t->seen != kicks);
}

bool task_run(struct task_set *s)
{
    if (s->cur)
        return false;
    bool ran = false;
    uint64_t now_ns = now();
    for (uint32_t i = 0; i < s->opts.max_tasks; i++)
        if (ready(&s->slots[i], s->kicks, now_ns)) {
            run_one(s, &s->slots[i]);
            ran = true;
        }
    return ran;
}

uint64_t task_next_wake(const struct task_set *s, uint64_t next)
{
    uint64_t now_ns = now();
    for (uint32_t i = 0; i < s->opts.max_tasks; i++) {
        const struct task *t = &s->slots[i];
        if (!t->live || t->done)
            continue;
        if (ready(t, s->kicks, now_ns))
            return now_ns;
        if (t->wake_at < next)
            next = t->wake_at;
    }
    return next;
}

void task_wait(struct task_set *s, uint64_t deadline)
{
    struct task *t = s->cur;
    if (!t)
        return;   /* the loop never waits this way (it waits on its port) */
    if (s->opts.wake_cap_ns) {
        uint64_t cap = now() + s->opts.wake_cap_ns;
        if (cap < deadline)
            deadline = cap;
    }
    t->wake_at = deadline;
    s->cur = NULL;
    libos_task_switch(&t->sp, s->loop_sp);
}

void task_yield(struct task_set *s)
{
    task_wait(s, 0);
}

void task_kick(struct task_set *s)
{
    s->kicks++;
}

struct task *task_current(const struct task_set *s)
{
    return s->cur;
}

void *task_arg(const struct task *t)
{
    return t->arg;
}

unsigned task_live(const struct task_set *s)
{
    unsigned n = 0;
    for (uint32_t i = 0; i < s->opts.max_tasks; i++)
        n += s->slots[i].live && !s->slots[i].done;
    return n;
}

bool task_overflowed(const struct task_set *s)
{
    return s->overflowed;
}
