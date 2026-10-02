/* usb-bus: tasks, so ports and devices are served side by side in the
 * driver's one thread.
 *
 * A task is a function running on a stack of its own. It runs until it
 * waits (hc_wait and hc_sleep inside a task call task_wait), and the main
 * loop (serve.c) then goes on: it takes the controller's events and the
 * channels' packets, starts new tasks (work.c) and runs again every task
 * whose wait may be over. Only one thing runs at a time, and a task gives
 * the CPU up only where it waits, so what tasks share changes only across
 * a wait: every wait loop in usb-bus checks its condition again after
 * waking (a device gone, the driver stopping, a lock free).
 *
 * The tasks (work.c starts them):
 *   - one per port with work: a root port, a hub's port, or a hub's own
 *     change (port 0): debounce, reset, enumerate, detach;
 *   - one per device with requests on its interface channels or endpoint
 *     upkeep queued (serve.c, intr.c).
 * So a device that takes seconds to answer holds up only its own port,
 * and a class driver's request is answered while other ports are still
 * being enumerated.
 *
 * What a task holds across a wait is guarded:
 *   - the command ring: one command at a time (hc_command waits its
 *     turn), which is also how the controller runs them (xHCI 4.6.1);
 *   - a device's default endpoint: one control transfer at a time
 *     (usb_control);
 *   - a device's requests, its bulk transfers among them: one at a time,
 *     since one task serves them (other devices' go on meanwhile);
 *   - the default address: between its reset and Address Device a device
 *     answers at USB address 0, and so would any other device reset in the
 *     same tree (a hub repeats downstream packets to every enabled port).
 *     One device per root port's tree at a time (addr0_take): different
 *     root ports are separate links, so they go in parallel;
 *   - a device entry: dev_hold / dev_put (devices.c) keep it from being
 *     freed under a task that uses it.
 *
 * Waking: a task waits until a deadline or until something happens: any
 * event or packet (task_kick, from hc.c's wait), or a lock given back
 * (task_kick too). Its wait loop then looks again. That wakes a task more
 * often than it needs, but there are few tasks and only while devices
 * come and go or requests run.
 *
 * Switching: task_switch (assembly, below) pushes the callee-saved
 * registers on the current stack, saves its stack pointer, loads the
 * other's and pops its registers. A new task's stack is laid out so that
 * the first switch to it "returns" into task_entry. A canary at the bottom
 * of each stack is checked whenever its task switches back: an overflow
 * stops the driver (exit 7) instead of running on in damaged memory. */
#include "usbbus.h"

#define TASK_STACK   (32 * 1024)   /* the deepest path (a request's IDL buffers) needs ~4 KiB */
#define CANARY_WORDS 8
#define CANARY       0x7a5c0de57ac0ffeeull
#define WAKE_CAP     (50 * NS_PER_MS)   /* a waiting task looks at least this often */

static struct task tasks[MAX_TASKS];
static struct task *cur;            /* the task running, NULL: the main loop */
static uint64_t main_sp;            /* the main loop's stack pointer while a task runs */
static uint64_t kicks;              /* bumped by task_kick: waiting tasks look again */
static uint8_t addr0[256];          /* per root port: a device of its tree at address 0 */
bool g_task_overflow;

/* task_switch(save, to): save the callee-saved registers and the stack
 * pointer into *save, continue on the stack `to` (System V AMD64: rbx,
 * rbp, r12-r15 are the callee's to keep; the rest a call may change). */
void usbbus_task_switch(uint64_t *save, uint64_t to);
__asm__(".text\n"
        ".globl usbbus_task_switch\n"
        ".type usbbus_task_switch, @function\n"
        "usbbus_task_switch:\n"
        "    push %rbp\n    push %rbx\n    push %r12\n"
        "    push %r13\n    push %r14\n    push %r15\n"
        "    mov %rsp, (%rdi)\n"
        "    mov %rsi, %rsp\n"
        "    pop %r15\n    pop %r14\n    pop %r13\n"
        "    pop %r12\n    pop %rbx\n    pop %rbp\n"
        "    ret\n"
        ".size usbbus_task_switch, .-usbbus_task_switch\n");

void tasks_reset(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        uint8_t *stack = tasks[i].stack;   /* a restart is a new process: NULL then */
        zero(&tasks[i], sizeof(tasks[i]));
        tasks[i].stack = stack;
    }
    cur = NULL;
    kicks = 0;
    zero(addr0, sizeof(addr0));
    g_task_overflow = false;
}

void tasks_free(void)
{
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].stack && !tasks[i].kind) {
            drv_free(tasks[i].stack);
            tasks[i].stack = NULL;
        }
}

/* The first code a new task runs (task_switch "returns" here). */
static void task_entry(void)
{
    struct task *t = cur;
    t->fn(t);
    t->done = true;
    cur = NULL;
    usbbus_task_switch(&t->sp, main_sp);
    __builtin_unreachable();   /* a done task is never switched to again */
}

/* Run t until it waits or ends; then check its stack and free its slot
 * if it ended. */
static void run_one(struct task *t)
{
    t->seen = kicks;
    cur = t;
    usbbus_task_switch(&main_sp, t->sp);
    const uint64_t *c = (const uint64_t *)t->stack;
    for (int i = 0; i < CANARY_WORDS; i++)
        if (c[i] != CANARY && !g_task_overflow) {
            g_task_overflow = true;
            drv_report("FAILED: a task overflowed its %u-byte stack (port %u, device %u)",
                       TASK_STACK, t->port, t->dev_id);
        }
    if (t->done)
        t->kind = 0;   /* free; the stack stays for the next task in the slot */
}

struct task *task_start(uint8_t kind, uint32_t dev_id, uint8_t port, void (*fn)(struct task *t))
{
    struct task *t = NULL;
    for (int i = 0; i < MAX_TASKS && !t; i++)
        if (!tasks[i].kind)
            t = &tasks[i];
    if (!t)
        return NULL;
    if (!t->stack && !(t->stack = drv_malloc(TASK_STACK)))
        return NULL;
    uint64_t *c = (uint64_t *)t->stack;
    for (int i = 0; i < CANARY_WORDS; i++)
        c[i] = CANARY;
    /* From the top: a zero return address for task_entry (so it is entered
     * with rsp + 8 16-byte aligned, as after a call), task_entry for
     * task_switch's ret, and six zero registers for its pops. */
    uint64_t *top = (uint64_t *)(t->stack + TASK_STACK);
    top[-1] = 0;
    top[-2] = (uint64_t)(uintptr_t)task_entry;
    for (int i = 3; i <= 8; i++)
        top[-i] = 0;
    t->sp = (uint64_t)(uintptr_t)&top[-8];
    t->kind = kind;
    t->done = false;
    t->dev_id = dev_id;
    t->port = port;
    t->fn = fn;
    t->wake_at = 0;
    run_one(t);
    return t;
}

struct task *task_find(uint8_t kind, uint32_t dev_id, uint8_t port)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->kind == kind && !t->done && t->dev_id == dev_id && t->port == port)
            return t;
    }
    return NULL;
}

bool in_task(void)
{
    return cur != NULL;
}

void task_wait(uint64_t deadline)
{
    struct task *t = cur;
    if (!t)
        return;   /* the main loop never waits this way (hc_wait does it there) */
    uint64_t cap = drv_clock_ns() + WAKE_CAP;
    t->wake_at = deadline < cap ? deadline : cap;
    cur = NULL;
    usbbus_task_switch(&t->sp, main_sp);
}

void task_yield(void)
{
    task_wait(0);
}

void task_kick(void)
{
    kicks++;
}

/* May t go on: its deadline passed, or something happened since it ran? */
static bool ready(const struct task *t, uint64_t now)
{
    return t->kind && !t->done && (now >= t->wake_at || t->seen != kicks);
}

bool tasks_run(void)
{
    bool ran = false;
    uint64_t now = drv_clock_ns();
    for (int i = 0; i < MAX_TASKS; i++)
        if (ready(&tasks[i], now)) {
            run_one(&tasks[i]);
            ran = true;
        }
    return ran;
}

uint64_t tasks_next_wake(uint64_t next)
{
    uint64_t now = drv_clock_ns();
    for (int i = 0; i < MAX_TASKS; i++) {
        const struct task *t = &tasks[i];
        if (!t->kind || t->done)
            continue;
        if (ready(t, now))
            return now;
        if (t->wake_at < next)
            next = t->wake_at;
    }
    return next;
}

unsigned tasks_live(uint8_t kind)
{
    unsigned n = 0;
    for (int i = 0; i < MAX_TASKS; i++)
        n += tasks[i].kind == kind && !tasks[i].done;
    return n;
}

/* ---- the default address ------------------------------------------------------ */

void addr0_take(uint8_t root_port)
{
    while (addr0[root_port] && !g_hc.dead && !g_hc.stopping && in_task())
        task_wait(drv_clock_ns() + WAKE_CAP);
    addr0[root_port] = 1;
}

void addr0_give(uint8_t root_port)
{
    addr0[root_port] = 0;
    task_kick();
}
