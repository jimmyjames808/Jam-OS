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
 * Each request handed over is written in a device slot of the state
 * (struct device_slot), so a successor whose predecessor died before its
 * thread answered hands it to its own thread (device_resume): the caller
 * is answered once, or, if the thread had written its answer and not yet
 * cleared the slot, twice, and its channel_call takes the first. At most
 * DEVICE_QUEUE requests wait for the thread; more are answered
 * ERR_NO_RESOURCES at once. What the thread touches: m->cards, set before
 * it starts and never changed after, and the device slots' `busy`, which
 * it clears (released) once it has answered (never the numbers: the
 * loop's thread owns them). */
#include <devmgr.h>
#include <idl/audioctl.h>
#include "internal.h"

#define DEVICE_STACK (64u << 10)

/* What the loop hands the thread. */
struct device_req {
    uint32_t txid;    /* the caller's, for the answer */
    uint32_t index;   /* audioctl.device's argument */
    uint32_t slot;    /* its device slot, cleared once answered */
};

static handle_t to_thread;      /* the loop's end (0: no thread: answered in the loop) */
static handle_t thread_end;     /* the thread's end */
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
        if (n == sizeof(q) && q.slot < DEVICE_QUEUE)   /* the loop sets it; ours to clear */
            __atomic_store_n(&m->saved->dev[q.slot].busy, 0, __ATOMIC_RELEASE);
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

/* Hand device slot i (filled, busy) to the thread with a duplicate of ch.
 * false: not handed over (the slot is free again). */
static bool hand(struct mixer *m, unsigned i, handle_t ch)
{
    struct device_slot *d = &m->saved->dev[i];
    struct device_req q = { d->txid, d->index, i };
    handle_t dup = HANDLE_INVALID;
    bool sent = jam_handle_duplicate(ch, RIGHT_SAME, &dup) == OK &&
                jam_channel_write(to_thread, &q, sizeof(q), &dup, 1) == OK;   /* dup goes too */
    if (sent)
        return true;
    if (dup)
        jam_handle_close(dup);
    __atomic_store_n(&d->busy, 0, __ATOMIC_RELEASE);
    return false;
}

status_t device_ask(struct mixer *m, handle_t ch, uint32_t key, unsigned slot,
                    const struct audioctl_device_req *q)
{
    if (!to_thread) {
        answer(m, ch, q->txid, q->index);
        return OK;
    }
    for (unsigned i = 0; i < DEVICE_QUEUE; i++) {
        struct device_slot *d = &m->saved->dev[i];
        if (__atomic_load_n(&d->busy, __ATOMIC_ACQUIRE))   /* the thread releases it */
            continue;
        *d = (struct device_slot){ .txid = q->txid, .index = q->index, .key = key,
                                   .seq = m->state.h->slot[slot].seq };
        __atomic_store_n(&d->busy, 1, __ATOMIC_RELEASE);   /* last: the slot is whole */
        return hand(m, i, ch) ? OK : ERR_NO_RESOURCES;
    }
    return ERR_NO_RESOURCES;
}

bool device_holds(const struct mixer *m, uint64_t seq)
{
    for (unsigned i = 0; i < DEVICE_QUEUE; i++)
        if (__atomic_load_n(&m->saved->dev[i].busy, __ATOMIC_ACQUIRE) &&
            m->saved->dev[i].seq == seq)
            return true;
    return false;
}

void device_resume(struct mixer *m)
{
    for (unsigned i = 0; i < DEVICE_QUEUE; i++) {
        struct device_slot *d = &m->saved->dev[i];
        if (!d->busy)
            continue;
        struct stream *s = NULL;
        uint32_t owner = 0;
        bool ctl = false;
        handle_t ch = key_channel(m, d->key, &s, &owner, &ctl);
        if (!ch || !ctl || !to_thread) {   /* its caller is gone, or no thread: never answered */
            if (ch && ctl)
                answer(m, ch, d->txid, d->index);
            d->busy = 0;
            continue;
        }
        if (!hand(m, i, ch))
            printf("mixer: an audioctl.device request in progress couldn't be handed on: "
                   "its caller waits until its deadline\n");
    }
}
