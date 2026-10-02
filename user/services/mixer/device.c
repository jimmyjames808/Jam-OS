/* mixer: audioctl.device on a thread of its own.
 *
 * Reaching the driver (out_query) is a call to devmgr and one to the
 * driver, each with a 2 s deadline, and either may be slow to answer
 * (devmgr busy with a disk, a driver devmgr is restarting). The loop must mix a period every 42.7 ms
 * with 128 ms of lead, so it never makes those calls: it hands the
 * request (its txid and index) to this thread over a channel and goes on.
 * With the request goes a duplicate of the control channel it came on (the
 * shared one or an opener's, clients.c): the thread writes the answer on it
 * itself and closes it (a channel write is one message, whichever thread
 * writes it, and the caller's channel_call takes its reply by txid), so a
 * late answer reaches only the channel its caller asked on.
 *
 * At most DEVICE_QUEUE requests wait for the thread; more are answered
 * ERR_NO_RESOURCES at once. What the thread touches of struct mixer:
 * m->cards, set before it starts and never changed after; `waiting` is
 * the one variable both threads change (atomics). */
#include <devmgr.h>
#include <idl/audioctl.h>
#include "internal.h"

#define DEVICE_QUEUE 8
#define DEVICE_STACK (64u << 10)

/* What the loop hands the thread. */
struct device_req {
    uint32_t txid;    /* the caller's, for the answer */
    uint32_t index;   /* audioctl.device's argument */
};

static handle_t to_thread;      /* the loop's end (0: no thread: answered in the loop) */
static handle_t thread_end;     /* the thread's end */
static uint32_t waiting;        /* handed over, not answered yet: atomics */
static uint8_t  stack[DEVICE_STACK] __attribute__((aligned(16)));

/* The answer on ch: the query channel, or why not, with the caller's
 * txid. */
static void answer(struct mixer *m, handle_t ch, uint32_t txid, uint32_t index)
{
    handle_t h = HANDLE_INVALID;
    status_t st = out_query(m, index, &h);
    if (st == OK && !h)
        st = ERR_INTERNAL;
    struct audioctl_device_rep r = { .txid = txid, .status = st };
    if (jam_channel_write(ch, &r, sizeof(r), &h, st == OK ? 1 : 0) != OK && st == OK)
        jam_handle_close(h);   /* the caller is gone */
}

static void thread_main(void *arg)
{
    struct mixer *m = arg;
    for (;;) {
        struct device_req q;
        handle_t ch = HANDLE_INVALID;
        uint32_t n = 0, nh = 0;
        status_t st = drv_channel_read(thread_end, &q, sizeof(q), &n, &ch, 1, &nh);
        if (st == ERR_SHOULD_WAIT) {
            signals_t seen;
            st = jam_object_wait_one(thread_end, SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER,
                                     &seen);
            if (st != OK)
                return;
            continue;
        }
        if (st != OK)
            return;   /* the loop's end is gone: the mixer is ending */
        if (n == sizeof(q) && nh == 1)
            answer(m, ch, q.txid, q.index);
        if (nh)
            jam_handle_close(ch);
        __atomic_sub_fetch(&waiting, 1, __ATOMIC_RELAXED);
    }
}

void device_init(struct mixer *m)
{
    handle_t th;
    status_t st = jam_channel_create(&to_thread, &thread_end);
    if (st == OK)
        st = thread_spawn("device", thread_main, m, stack, sizeof(stack), &th);
    if (st == OK) {
        jam_handle_close(th);   /* it runs for as long as the mixer does */
        return;
    }
    printf("mixer: no thread for audioctl.device (%s): answered in the loop\n",
           status_str(st));
    if (to_thread) {
        jam_handle_close(to_thread);
        jam_handle_close(thread_end);
    }
    to_thread = thread_end = HANDLE_INVALID;
}

void device_ask(struct mixer *m, handle_t ch, const struct audioctl_device_req *q)
{
    if (!to_thread) {
        answer(m, ch, q->txid, q->index);
        return;
    }
    struct device_req d = { q->txid, q->index };
    handle_t dup = HANDLE_INVALID;
    bool sent = __atomic_add_fetch(&waiting, 1, __ATOMIC_RELAXED) <= DEVICE_QUEUE &&
                jam_handle_duplicate(ch, RIGHT_SAME, &dup) == OK &&
                jam_channel_write(to_thread, &d, sizeof(d), &dup, 1) == OK;   /* dup goes too */
    if (!sent) {
        if (dup)
            jam_handle_close(dup);
        __atomic_sub_fetch(&waiting, 1, __ATOMIC_RELAXED);
        idl_reply_status(ch, q, sizeof(*q), ERR_NO_RESOURCES);
    }
}
