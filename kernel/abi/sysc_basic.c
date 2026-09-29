/* System calls: debug output, exit, time, handles, waiting and signals,
 * events, timers and ports. The rules all sysc_* follow are in sysc.h.
 *
 * Blocking calls (nanosleep, object_wait_one, port_wait) use the
 * cancellable waits, so a thread whose process is killed stops waiting at
 * once (ERR_CANCELED) and leaves on its way back to user mode. */
#include <jam/abi.h>
#include <jam/port.h>
#include <jam/sched.h>
#include <jam/sys.h>
#include <jam/syscall_impl.h>
#include <jam/time.h>
#include "sysc.h"

/* debug_write copies at most this much per call (the rest is dropped). */
#define DEBUG_WRITE_MAX 4096
#define DEBUG_CHUNK     256

static int64_t debug_out(uint64_t buf, uint64_t len, bool report_it)
{
    struct process *p = process_current();
    if (!p)
        return ERR_BAD_STATE;
    if (len > DEBUG_WRITE_MAX)
        len = DEBUG_WRITE_MAX;
    char chunk[DEBUG_CHUNK];
    while (len) {
        uint64_t n = len < sizeof(chunk) ? len : sizeof(chunk);
        if (copy_from_user(chunk, buf, n) != OK)
            return ERR_INVALID_ARGS;
        process_debug_write(p, chunk, n, report_it);
        buf += n;
        len -= n;
    }
    return OK;
}

int64_t sysc_debug_write(uint64_t buf, uint64_t len)
{
    return debug_out(buf, len, false);
}

int64_t sysc_debug_report(uint64_t buf, uint64_t len)
{
    return debug_out(buf, len, true);
}

int64_t sysc_process_exit(int64_t code)
{
    struct process *p = process_current();
    if (!p)
        return ERR_BAD_STATE;
    process_kill(p, code, false);   /* cancels every thread, this one too */
    uthread_exit_current();
}

int64_t sysc_thread_exit(void)
{
    if (!process_current())
        return ERR_BAD_STATE;
    uthread_exit_current();
}

int64_t sysc_clock_get(void)
{
    return (int64_t)uptime_ns();
}

int64_t sysc_nanosleep(uint64_t deadline_ns)
{
    while (uptime_ns() < deadline_ns)
        if (thread_block_cancellable(NULL, NULL, deadline_ns) != OK)
            return ERR_CANCELED;
    return OK;
}

/* ---- handles ---------------------------------------------------------------- */

int64_t sysc_handle_close(handle_t h)
{
    SYSC_TABLE(t);
    return handle_close(t, h);
}

int64_t sysc_handle_duplicate(handle_t h, rights_t rights, uint64_t out)
{
    SYSC_TABLE(t);
    handle_t nh;
    status_t st = handle_duplicate(t, h, rights, &nh);
    return st == OK ? sysc_put_handle(t, out, nh) : st;
}

int64_t sysc_handle_replace(handle_t h, rights_t rights, uint64_t out)
{
    SYSC_TABLE(t);
    handle_t nh;
    status_t st = handle_replace(t, h, rights, &nh);
    /* h is gone either way once this succeeded; if the new value can't be
     * delivered the new handle is closed too (the caller passed a bad
     * pointer and loses the object, but nothing leaks). */
    return st == OK ? sysc_put_handle(t, out, nh) : st;
}

/* ---- waiting and signals ------------------------------------------------------ */

int64_t sysc_object_wait_one(handle_t h, signals_t mask, uint64_t deadline_ns, uint64_t observed)
{
    SYSC_TABLE(t);
    signals_t seen = 0;
    status_t st = sys_object_wait_one(t, h, mask, deadline_ns, &seen);
    if (observed && (st == OK || st == ERR_TIMED_OUT || st == ERR_CANCELED) &&
        copy_to_user(observed, &seen, sizeof(seen)) != OK)
        return ERR_INVALID_ARGS;
    return st;
}

int64_t sysc_object_signal(handle_t h, signals_t clear, signals_t set)
{
    SYSC_TABLE(t);
    return sys_object_signal(t, h, clear, set);
}

/* ---- events and timers ------------------------------------------------------------ */

int64_t sysc_event_create(uint64_t out)
{
    SYSC_TABLE(t);
    handle_t h;
    status_t st = sys_event_create(t, &h);
    return st == OK ? sysc_put_handle(t, out, h) : st;
}

int64_t sysc_event_signal(handle_t h, signals_t clear, signals_t set)
{
    SYSC_TABLE(t);
    return sys_event_signal(t, h, clear, set);
}

int64_t sysc_timer_create(uint64_t out)
{
    SYSC_TABLE(t);
    handle_t h;
    status_t st = sys_timer_create(t, &h);
    return st == OK ? sysc_put_handle(t, out, h) : st;
}

int64_t sysc_timer_set(handle_t h, uint64_t deadline_ns)
{
    SYSC_TABLE(t);
    return sys_timer_set(t, h, deadline_ns);
}

int64_t sysc_timer_cancel(handle_t h)
{
    SYSC_TABLE(t);
    return sys_timer_cancel(t, h);
}

/* ---- ports --------------------------------------------------------------------------- */

int64_t sysc_port_create(uint64_t out)
{
    SYSC_TABLE(t);
    handle_t h;
    status_t st = sys_port_create(t, &h);
    return st == OK ? sysc_put_handle(t, out, h) : st;
}

int64_t sysc_port_bind(handle_t port, handle_t obj, uint64_t key, signals_t mask, uint32_t flags)
{
    SYSC_TABLE(t);
    return sys_port_bind(t, port, obj, key, mask, flags);
}

int64_t sysc_port_unbind(handle_t port, handle_t obj, uint64_t key)
{
    SYSC_TABLE(t);
    return sys_port_unbind(t, port, obj, key);
}

int64_t sysc_port_queue(handle_t port, uint64_t pkt)
{
    SYSC_TABLE(t);
    struct port_packet k;
    if (copy_from_user(&k, pkt, sizeof(k)) != OK)
        return ERR_INVALID_ARGS;
    return sys_port_queue(t, port, &k);
}

int64_t sysc_port_wait(handle_t port, uint64_t deadline_ns, uint64_t out)
{
    SYSC_TABLE(t);
    struct port_packet k;
    status_t st = sys_port_wait(t, port, deadline_ns, &k);
    /* A packet taken off the port can't be put back: a bad `out` loses it
     * (the caller's own packet, so that's its problem, not a leak). */
    if (st == OK && copy_to_user(out, &k, sizeof(k)) != OK)
        return ERR_INVALID_ARGS;
    return st;
}
