/* netstack: reaching the network card's driver, on a thread of its own.
 *
 * The loop writes one word on the thread's channel to ask for a session;
 * the thread asks devmgr for the driver's channel (GET_SERVICE on each of
 * our device channels in turn) and the driver for its card (netdev.info)
 * and a session (netdev.open), each call bounded by DEV_CALL_WAIT, and
 * writes back a struct dev_found with the session's five handles, or
 * with why there is none. It serves nothing else, so its calls may wait
 * (ARCHITECTURE.md "How a service waits"); it reads only d->cards and
 * d->ncards, which never change once it runs. It ends when the loop's end
 * of its channel closes. */
#include <devmgr.h>
#include <idl/netdev.h>
#include "dev.h"

static handle_t thread_end;            /* its end of the loop's channel */
static uint8_t  stack[16 * 1024] __attribute__((aligned(16)));

/* The driver's channel for card i, from devmgr. */
static status_t driver_channel(const struct dev *d, unsigned i, handle_t *out)
{
    struct devmgr_rep r;
    handle_t ch = HANDLE_INVALID;
    uint32_t nh = 0;
    status_t st = devmgr_call(d->cards[i], DEVMGR_GET_SERVICE, 0, 0, 0, &r, &ch, 1, &nh,
                              now() + DEV_CALL_WAIT);
    if (st == OK && nh != 1)
        st = ERR_INTERNAL;   /* not devmgr's format */
    if (st != OK) {
        if (nh)
            jam_handle_close(ch);
        return st;
    }
    *out = ch;
    return OK;
}

/* Card i: its info and a session, into f and hs. */
static status_t open_card(const struct dev *d, unsigned i, struct dev_found *f, handle_t *hs)
{
    handle_t svc;
    status_t st = driver_channel(d, i, &svc);
    if (st != OK)
        return st;
    uint16_t mtu;
    uint8_t chip[16];
    st = netdev_info_until(svc, now() + DEV_CALL_WAIT, f->mac, &f->vlan, &mtu, &f->link,
                           &f->speed, &f->changes, chip);
    if (st == OK && (mtu != NETDEV_MTU || !netframe_mode_ok(f->vlan)))
        st = ERR_NOT_SUPPORTED;   /* not the card netdev.idl describes */
    if (st == OK)
        st = netdev_open_until(svc, now() + DEV_CALL_WAIT, &hs[DEV_H_SESSION], &hs[DEV_H_TX],
                               &hs[DEV_H_RX], &hs[DEV_H_TO_DRIVER], &hs[DEV_H_TO_STACK]);
    jam_handle_close(svc);   /* the session is a channel of its own */
    if (st == OK) {
        for (unsigned k = 0; k < sizeof(f->chip); k++)   /* printable or NUL only */
            f->chip[k] = chip[k] >= ' ' && chip[k] < 0x7f ? (char)chip[k] : 0;
        f->chip[sizeof(f->chip) - 1] = 0;
        f->card = i;
    }
    return st;
}

/* The first card that gives a session; f->st says what happened. */
static void find(const struct dev *d, struct dev_found *f, handle_t *hs)
{
    f->st = ERR_NOT_FOUND;
    for (unsigned i = 0; i < d->ncards; i++) {
        status_t st = open_card(d, i, f, hs);
        f->st = st;
        if (st == OK || st == ERR_PEER_CLOSED)   /* a session, or devmgr is gone */
            return;
    }
}

static void thread_main(void *arg)
{
    const struct dev *d = arg;
    for (;;) {
        uint32_t word, n = 0, nh = 0;
        signals_t seen;
        status_t st = jam_object_wait_one(thread_end, SIG_READABLE | SIG_PEER_CLOSED,
                                          DEADLINE_NEVER, &seen);
        if (st == OK)
            st = drv_channel_read(thread_end, &word, sizeof(word), &n, NULL, 0, &nh);
        if (st == ERR_SHOULD_WAIT)
            continue;
        if (st != OK)
            break;   /* the loop's end is gone: netstack is ending */
        struct dev_found f = { 0 };
        handle_t hs[DEV_HANDLES] = { 0 };
        find(d, &f, hs);
        uint32_t nhs = f.st == OK ? DEV_HANDLES : 0;
        if (jam_channel_write(thread_end, &f, sizeof(f), hs, nhs) != OK)
            for (unsigned k = 0; k < nhs; k++)
                jam_handle_close(hs[k]);   /* not sent: still ours */
    }
    jam_handle_close(thread_end);
}

status_t connect_start(struct dev *d)
{
    status_t st = jam_channel_create(&d->to_thread, &thread_end);
    handle_t th;
    if (st == OK)
        st = thread_spawn("connect", thread_main, d, stack, sizeof(stack), &th);
    if (st != OK) {
        if (d->to_thread) {
            jam_handle_close(d->to_thread);
            jam_handle_close(thread_end);
        }
        d->to_thread = HANDLE_INVALID;
        return st;
    }
    jam_handle_close(th);   /* it runs for as long as netstack does */
    return OK;
}

void connect_ask(struct dev *d)
{
    uint32_t word = 1;
    if (d->asked || !d->to_thread)
        return;
    d->asked = jam_channel_write(d->to_thread, &word, sizeof(word), NULL, 0) == OK;
    d->retry_at = 0;
}
