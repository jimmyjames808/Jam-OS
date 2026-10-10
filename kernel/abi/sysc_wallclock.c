/* System calls 134, 135 and 137: the wall clock (<jam/wallclock.h>) and the
 * time page. wallclock_get needs no handle: the time is the uptime, which
 * any thread can read, plus an offset; nor does clock_page, the same two
 * clocks as a page to read them from. wallclock_set needs RIGHT_ROOT_CLOCK
 * on the root resource (init holds it). The rules every sysc_* follows are
 * in sysc.h. */
#include <jam/syscall_impl.h>
#include <jam/sysinfo.h>
#include <jam/vmo.h>
#include <jam/wallclock.h>
#include "sysc.h"

int64_t sysc_wallclock_get(uint64_t out)
{
    struct wall_clock c;
    status_t st = wallclock_get(&c);
    if (st != OK)
        return st;
    return copy_to_user(out, &c, sizeof(c)) == OK ? OK : ERR_INVALID_ARGS;
}

int64_t sysc_wallclock_set(handle_t root, uint64_t in)
{
    SYSC_TABLE(t);
    status_t st = sysinfo_check_root(t, root, RIGHT_ROOT_CLOCK);
    if (st != OK)
        return st;
    struct wall_clock c;
    if (copy_from_user(&c, in, sizeof(c)) != OK)
        return ERR_INVALID_ARGS;
    return wallclock_set(&c);
}

int64_t sysc_clock_page(uint64_t out)
{
    SYSC_TABLE(t);
    struct vmo *v;
    status_t st = wallclock_page(&v);
    if (st != OK)
        return st;
    return sysc_publish(t, vmo_kobject(v), TIME_PAGE_RIGHTS, out);
}
