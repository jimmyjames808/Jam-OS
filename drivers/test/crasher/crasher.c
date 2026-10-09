/* crasher: a driver that dies on command, for the
 * supervision tests. devmgr starts it on request (devmgrctl.test_driver) as
 * a supervised driver of a software device: it gets only DR_SERVE. utest
 * crashes it and checks that devmgr restarts it with the right backoff,
 * that clients reconnect, and that devmgr gives up after too many deaths.
 *
 * Protocol on DR_SERVE: `crasher` (abi/idl/crasher.idl).
 *   ping  -> u64 started_ns: when this instance started (drv_clock_ns),
 *            so a client sees a restart and how long it took
 *   crash (an event) -> the driver faults (a write to address 0); the
 *            kernel kills it, like any crashing driver
 *   exit  (an event, u32 code) -> the driver returns `code`
 * It returns 0 when its client closes the channel. Built both ways like
 * every driver, but only ever run as a process (a crash in the kernel
 * build would be the kernel's). */
#include <idl/crasher.h>
#include <jam/driver.h>

struct crasher_state {
    uint64_t started;   /* when this instance started (uptime, ns) */
};

static status_t do_ping(void *ctx, uint64_t *out_started_ns)
{
    *out_started_ns = ((struct crasher_state *)ctx)->started;
    return OK;
}

static void do_crash(void *ctx)
{
    (void)ctx;
    drv_log("crashing on command");
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)8;   /* the null page: never mapped */
    __asm__ volatile("" : "+r"(p));   /* (the compiler may not see it's null) */
    *p = 0xdead;
    drv_exit(3);   /* not reached */
}

static void do_exit(void *ctx, uint32_t code)
{
    (void)ctx;
    drv_log("exiting with %u on command", code);
    drv_exit((int)code);
}

static const struct crasher_ops ops = {
    .ping = do_ping,
    .crash = do_crash,
    .exit = do_exit,
};

int driver_main(const struct driver_start *s)
{
    struct crasher_state state = { .started = drv_clock_ns() };
    handle_t ch = drv_handle(s, DR_SERVE);
    if (ch == HANDLE_INVALID) {
        drv_log("no DR_SERVE channel: nothing to serve");
        return 2;
    }
    /* Until the client closes the channel (OK: it is gone, we are done). */
    return crasher_serve(ch, &ops, &state) == OK ? 0 : 1;
}
