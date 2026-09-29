/* crasher: a driver that dies on command, for the
 * supervision tests. devmgr starts it on request (DEVMGR_TEST_DRIVER) as a
 * supervised driver of a software device: it gets only DR_SERVE. utest
 * crashes it and checks that devmgr restarts it with the right backoff,
 * that clients reconnect, and that devmgr gives up after too many deaths.
 *
 * Protocol on DR_SERVE, hand-written (the IDL files are the foundation's;
 * <devmgr.h> has the same numbers for clients): a request is u32 txid,
 * u32 ordinal [, u32 argument]; a reply is u32 txid, i32 status, results.
 *   PING  -> u64 started_ns: when this instance started (drv_clock_ns),
 *            so a client sees a restart and how long it took
 *   CRASH -> no reply: the driver faults (a write to address 0); the
 *            kernel kills it, like any crashing driver
 *   EXIT  (u32 code) -> no reply: the driver returns `code`
 * It returns 0 when its client closes the channel. Built both ways like
 * every driver, but only ever run as a process (a crash in the kernel
 * build would be the kernel's). */
#include <jam/driver.h>

#define CRASHER_PING  0xc4a50001u
#define CRASHER_CRASH 0xc4a50002u
#define CRASHER_EXIT  0xc4a50003u

/* A request: CRASHER_* ordinal, arg = EXIT's code. */
struct req {
    uint32_t txid, ordinal, arg;   /* the caller's txid, CRASHER_*, EXIT's exit code */
};

/* PING's reply. */
struct ping_rep {
    uint32_t txid;         /* the request's */
    int32_t  status;       /* OK */
    uint64_t started_ns;   /* when this instance of the driver started (uptime) */
} __attribute__((packed));

static void crash(void)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)8;   /* the null page: never mapped */
    __asm__ volatile("" : "+r"(p));   /* (the compiler may not see it's null) */
    *p = 0xdead;
}

int driver_main(const struct driver_start *s)
{
    uint64_t started = drv_clock_ns();
    handle_t ch = drv_handle(s, DR_SERVE);
    if (ch == HANDLE_INVALID) {
        drv_log("no DR_SERVE channel: nothing to serve");
        return 2;
    }
    for (;;) {
        struct req q = { 0, 0, 0 };
        uint32_t n = 0, nh = 0;
        status_t st = drv_channel_read(ch, &q, sizeof(q), &n, NULL, 0, &nh);
        if (st == ERR_SHOULD_WAIT) {
            signals_t seen = 0;
            st = drv_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, UINT64_MAX, &seen);
            if (st != OK)
                return 1;
            continue;
        }
        if (st == ERR_PEER_CLOSED)
            return 0;   /* the client is gone */
        if (st == ERR_BUFFER_TOO_SMALL) {
            /* Not ours (too big, or with handles): off the queue. */
            void *big = drv_malloc(n ? n : 1);
            handle_t hs[64];
            if (!big || nh > 64)
                return 1;
            if (drv_channel_read(ch, big, n, &n, hs, nh, &nh) == OK)
                for (uint32_t i = 0; i < nh; i++)
                    drv_handle_close(hs[i]);
            drv_free(big);
            continue;
        }
        if (st != OK || n < 8)
            continue;   /* too small: not ours */
        switch (q.ordinal) {
        case CRASHER_PING: {
            struct ping_rep r = { q.txid, OK, started };
            drv_channel_write(ch, &r, sizeof(r), NULL, 0);
            break;
        }
        case CRASHER_CRASH:
            drv_log("crashing on command");
            crash();
            return 3;   /* not reached */
        case CRASHER_EXIT:
            drv_log("exiting with %u on command", n >= 12 ? q.arg : 0);
            return n >= 12 ? (int)q.arg : 0;
        default: {
            struct { uint32_t txid; int32_t status; } r = { q.txid, ERR_NOT_SUPPORTED };
            drv_channel_write(ch, &r, sizeof(r), NULL, 0);
            break;
        }
        }
    }
}
