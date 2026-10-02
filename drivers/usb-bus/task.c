/* usb-bus: tasks, so ports and devices are served side by side in the
 * driver's one thread, on libos's cooperative tasks (<jam/task.h> has
 * the model: a task runs until it waits, the loop runs every task whose
 * wait may be over).
 *
 * hc_wait and hc_sleep inside a task call task_wait, and the main loop
 * (serve.c) then goes on: it takes the controller's events and the
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
 * being enumerated. This file keeps what usb-bus adds to a task (its kind,
 * port and device, so work.c can find a port's or a device's task) and
 * the default address.
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
 * (task_kick too). Its wait loop then looks again, and it looks at least
 * every WAKE_CAP anyway. That wakes a task more often than it needs, but
 * there are few tasks and only while devices come and go or requests run.
 *
 * A task that runs over its stack stops the driver (exit 7) instead of
 * running on in damaged memory (the set's overflow callback). */
#include "usbbus.h"

#define TASK_STACK   (32 * 1024)   /* the deepest path (a request's IDL buffers) needs ~4 KiB */
#define WAKE_CAP     (50 * NS_PER_MS)   /* a waiting task looks at least this often */

struct task_set *g_tasks;
static struct usb_task tasks[MAX_TASKS];
static uint8_t addr0[256];          /* per root port: a device of its tree at address 0 */
bool g_task_overflow;

static void overflowed(void *arg)
{
    const struct usb_task *u = arg;
    g_task_overflow = true;
    drv_report("FAILED: a task overflowed its %u-byte stack (port %u, device %u)", TASK_STACK,
               u->port, u->dev_id);
}

bool tasks_reset(void)
{
    if (g_tasks)
        tasks_free();   /* a restart is a new process: never, today */
    zero(tasks, sizeof(tasks));
    zero(addr0, sizeof(addr0));
    g_task_overflow = false;
    struct task_opts o = { .max_tasks = MAX_TASKS, .stack_bytes = TASK_STACK,
                           .wake_cap_ns = WAKE_CAP, .overflow = overflowed };
    g_tasks = task_set_create(&o);
    return g_tasks != NULL;
}

void tasks_free(void)
{
    if (g_tasks)
        (void)task_set_destroy(g_tasks);   /* a task still live keeps its stack */
    g_tasks = NULL;
}

/* What every usb-bus task runs: its work, then the slot is free. */
static void entry(void *arg)
{
    struct usb_task *u = arg;
    u->fn(u);
    u->kind = 0;
}

struct usb_task *usb_task_start(uint8_t kind, uint32_t dev_id, uint8_t port,
                                void (*fn)(struct usb_task *t))
{
    struct usb_task *u = NULL;
    for (int i = 0; i < MAX_TASKS && !u; i++)
        if (!tasks[i].kind)
            u = &tasks[i];
    if (!u)
        return NULL;
    u->kind = kind;
    u->dev_id = dev_id;
    u->port = port;
    u->fn = fn;
    if (!task_start(g_tasks, entry, u)) {
        u->kind = 0;
        return NULL;
    }
    return u;   /* it may have ended already: then kind is 0 again */
}

struct usb_task *usb_task_find(uint8_t kind, uint32_t dev_id, uint8_t port)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        struct usb_task *u = &tasks[i];
        if (u->kind == kind && u->dev_id == dev_id && u->port == port)
            return u;
    }
    return NULL;
}

bool in_task(void)
{
    return g_tasks && task_current(g_tasks);
}

unsigned tasks_live(uint8_t kind)
{
    unsigned n = 0;
    for (int i = 0; i < MAX_TASKS; i++)
        n += tasks[i].kind == kind;
    return n;
}

/* ---- the default address ------------------------------------------------------ */

void addr0_take(uint8_t root_port)
{
    while (addr0[root_port] && !g_hc.dead && !g_hc.stopping && in_task())
        task_wait(g_tasks, drv_clock_ns() + WAKE_CAP);
    addr0[root_port] = 1;
}

void addr0_give(uint8_t root_port)
{
    addr0[root_port] = 0;
    task_kick(g_tasks);
}
