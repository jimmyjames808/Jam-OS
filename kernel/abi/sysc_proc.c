/* System calls: processes, threads and jobs. The rules all sysc_* follow
 * are in sysc.h; the objects are in object/process.c and object/job.c.
 *
 * Rights: creating a process in a job, or a child job, needs RIGHT_WRITE on
 * the job; starting, killing and making threads in a process, and
 * becoming it (process_replace), needs RIGHT_WRITE on it; starting a thread or changing its priority needs
 * RIGHT_WRITE on the thread; interrupting a thread's wait needs
 * RIGHT_SIGNAL on it; reading info needs RIGHT_INSPECT. A thread's tid
 * word is its own business: no handle. Changing a
 * job's limits needs RIGHT_MANAGE, which only the job's creator gets
 * (JOB_RIGHTS from job_create); the processes IN a job see it through
 * JOB_RIGHTS_OWN, without it, so a program can't lift its own limits. Killing
 * a job needs RIGHT_MANAGE too. */
#include <jam/aspace.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/sched.h>
#include <jam/syscall_impl.h>
#include <jam/uentry.h>
#include <jam/vmar.h>
#include "sysc.h"

#define VMAR_HANDLE_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE)

/* A name argument (dst: PROCESS_NAME_MAX + 1 bytes): one longer than
 * PROCESS_NAME_MAX is ERR_OUT_OF_RANGE, nothing cut (libos cuts a name
 * itself); a fault ERR_INVALID_ARGS. */
static status_t copy_name(char *dst, uint64_t uname, uint64_t len)
{
    if (len > PROCESS_NAME_MAX)
        return ERR_OUT_OF_RANGE;
    if (copy_in(dst, uname, len) != OK)
        return ERR_INVALID_ARGS;
    dst[len] = '\0';
    return OK;
}

int64_t sysc_process_create(handle_t job, uint64_t name, uint64_t name_len, uint32_t flags,
                            uint64_t proc_out, uint64_t vmar_out)
{
    SYSC_TABLE(t);
    if (flags)
        return ERR_INVALID_ARGS;
    char kname[PROCESS_NAME_MAX + 1];
    status_t st = copy_name(kname, name, name_len);
    if (st != OK)
        return st;
    struct kobject *jo;
    st = handle_get(t, job, OBJ_JOB, RIGHT_WRITE, &jo, NULL);
    if (st != OK)
        return st;
    struct process *p;
    st = process_create(job_from_kobject(jo), kname, &p);
    kobject_unref(jo);
    if (st != OK)
        return st;
    struct aspace *as = process_aspace(p);   /* NULL if a job_kill already got it */
    struct vmar *v = NULL;
    st = as ? vmar_create_for(as, &v) : ERR_BAD_STATE;
    if (as)
        aspace_unref(as);
    if (st != OK) {
        kobject_unref(process_kobject(p));
        return st;
    }
    /* Both handles in, then both values out. On any failure both are closed
     * again; closing the last handle of a NEW process makes it die. */
    struct khandle kp = khandle_from_new(process_kobject(p), PROCESS_RIGHTS);
    struct khandle kv = khandle_from_new(vmar_kobject(v), VMAR_HANDLE_RIGHTS);
    handle_t ph = HANDLE_INVALID, vh = HANDLE_INVALID;
    st = handle_insert(t, &kp, &ph);
    if (st == OK)
        st = handle_insert(t, &kv, &vh);
    if (st == OK && (copy_to_user(proc_out, &ph, sizeof(ph)) != OK ||
                     copy_to_user(vmar_out, &vh, sizeof(vh)) != OK))
        st = ERR_INVALID_ARGS;
    if (st != OK) {
        if (vh != HANDLE_INVALID)
            handle_close(t, vh);
        if (ph != HANDLE_INVALID)
            handle_close(t, ph);
    }
    khandle_release(&kv);   /* no-ops once inserted */
    khandle_release(&kp);
    return st;
}

int64_t sysc_process_start(handle_t proc, handle_t thread, uint64_t entry, uint64_t stack,
                           handle_t arg0, uint64_t arg1)
{
    SYSC_TABLE(t);
    /* arg0 leaves our table whatever the status (M12's Q1, as a channel
     * send's handles): moved into the child, or closed. */
    struct kobject *po, *to;
    status_t st = handle_get(t, proc, OBJ_PROCESS, RIGHT_WRITE, &po, NULL);
    if (st != OK) {
        handle_close(t, arg0);
        return st;
    }
    st = handle_get(t, thread, OBJ_THREAD, RIGHT_WRITE, &to, NULL);
    if (st != OK) {
        handle_close(t, arg0);
        kobject_unref(po);
        return st;
    }
    struct khandle kh = { NULL, 0 };
    if (arg0 != HANDLE_INVALID)
        st = handle_take(t, arg0, &kh);   /* reserved in our table until we know */
    if (st != OK)
        handle_close(t, arg0);   /* no RIGHT_TRANSFER: closed all the same */
    if (st == OK) {
        st = process_start(process_from_kobject(po), uthread_from_kobject(to), entry, stack, &kh,
                           arg1, NULL);
        if (arg0 != HANDLE_INVALID) {
            handle_commit(t, arg0);   /* it lives in the child now, or ... */
            khandle_release(&kh);     /* ... it is closed (a no-op if the child has it) */
        }
    }
    kobject_unref(to);
    kobject_unref(po);
    return st;
}

int64_t sysc_process_kill(handle_t proc)
{
    SYSC_TABLE(t);
    struct kobject *po;
    status_t st = handle_get(t, proc, OBJ_PROCESS, RIGHT_WRITE, &po, NULL);
    if (st != OK)
        return st;
    struct process *p = process_from_kobject(po);
    struct process_info info;
    process_get_info(p, &info);
    if (info.state < PROCESS_DYING)
        kprintf("user: process \"%s\" killed by \"%s\"\n", process_name(p),
                process_name(process_current()));
    process_kill(p, PROCESS_KILLED_CODE, true);
    kobject_unref(po);
    return OK;
}

int64_t sysc_process_get_info(handle_t proc, uint64_t out)
{
    SYSC_TABLE(t);
    struct kobject *po;
    status_t st = handle_get(t, proc, OBJ_PROCESS, RIGHT_INSPECT, &po, NULL);
    if (st != OK)
        return st;
    struct process_info info;
    struct process *p = process_from_kobject(po);
    process_get_info(p, &info);
    struct aspace *as = process_aspace(p);
    if (as) {
        info.mappings = aspace_mapping_count(as);   /* the region lock: not under any spinlock */
        aspace_unref(as);
    }
    kobject_unref(po);
    return copy_to_user(out, &info, sizeof(info));
}

int64_t sysc_process_get_maps(handle_t proc, uint64_t from, uint64_t out, uint32_t cap)
{
    SYSC_TABLE(t);
    if (!cap || cap > MAP_INFO_MAX)
        return ERR_OUT_OF_RANGE;
    struct kobject *po;
    status_t st = handle_get(t, proc, OBJ_PROCESS, RIGHT_INSPECT, &po, NULL);
    if (st != OK)
        return st;
    struct map_info *buf = kmalloc((size_t)cap * sizeof(*buf));
    struct aspace *as = buf ? process_aspace(process_from_kobject(po)) : NULL;
    kobject_unref(po);
    if (!buf)
        return ERR_NO_MEMORY;
    uint32_t n = 0;
    if (as) {   /* NULL once the process is torn down */
        n = aspace_get_maps(as, from, buf, cap);   /* the region lock: may sleep */
        aspace_unref(as);
    }
    st = copy_out(out, buf, (size_t)n * sizeof(*buf));
    kfree(buf);
    return st == OK ? (int64_t)n : ERR_INVALID_ARGS;
}

/* Exec in place (docs/FORK-PLAN.md section 2; abi/syscalls.def has the
 * contract, process_replace the work). Returns only on an error, with
 * proc and arg0 still the caller's. */
int64_t sysc_process_replace(handle_t proc, uint64_t entry, uint64_t stack, handle_t arg0,
                             uint64_t arg1)
{
    SYSC_TABLE(t);
    struct kobject *po;
    status_t st = handle_get(t, proc, OBJ_PROCESS, RIGHT_WRITE, &po, NULL);
    if (st != OK)
        return st;
    return process_replace(process_from_kobject(po), entry, stack, arg0, arg1);   /* takes po */
}

int64_t sysc_thread_create(handle_t proc, uint64_t name, uint64_t name_len, uint32_t flags,
                           uint64_t out)
{
    SYSC_TABLE(t);
    if (flags)
        return ERR_INVALID_ARGS;
    char kname[PROCESS_NAME_MAX + 1];
    status_t st = copy_name(kname, name, name_len);
    if (st != OK)
        return st;
    struct kobject *po;
    st = handle_get(t, proc, OBJ_PROCESS, RIGHT_WRITE, &po, NULL);
    if (st != OK)
        return st;
    struct uthread *u;
    st = uthread_create(process_from_kobject(po), kname, &u);
    kobject_unref(po);
    return st == OK ? sysc_publish(t, uthread_kobject(u), THREAD_RIGHTS, out) : st;
}

int64_t sysc_thread_start(handle_t thread, uint64_t entry, uint64_t stack, uint64_t arg0,
                          uint64_t arg1, uint64_t fs_base)
{
    SYSC_TABLE(t);
    struct kobject *to;
    status_t st = handle_get(t, thread, OBJ_THREAD, RIGHT_WRITE, &to, NULL);
    if (st != OK)
        return st;
    st = uthread_start(uthread_from_kobject(to), entry, stack, arg0, arg1, fs_base, NULL);
    kobject_unref(to);
    return st;
}

int64_t sysc_thread_set_priority(handle_t thread, int32_t prio)
{
    SYSC_TABLE(t);
    if (prio < PRIO_MIN || prio > PRIO_MAX)
        return ERR_INVALID_ARGS;
    if (prio > PRIO_USER_MAX)
        return ERR_ACCESS_DENIED;   /* above 24 is the kernel's */
    struct kobject *to;
    status_t st = handle_get(t, thread, OBJ_THREAD, RIGHT_WRITE, &to, NULL);
    if (st != OK)
        return st;
    st = uthread_set_priority(uthread_from_kobject(to), prio);
    kobject_unref(to);
    return st;
}

int64_t sysc_thread_set_tid_word(uint64_t addr)
{
    return uthread_set_tid_word(addr);
}

int64_t sysc_thread_interrupt(handle_t thread)
{
    SYSC_TABLE(t);
    struct kobject *to;
    status_t st = handle_get(t, thread, OBJ_THREAD, RIGHT_SIGNAL, &to, NULL);
    if (st != OK)
        return st;
    st = uthread_interrupt(uthread_from_kobject(to));
    kobject_unref(to);
    return st;
}

int64_t sysc_thread_raise(handle_t thread)
{
    SYSC_TABLE(t);
    struct kobject *to;
    status_t st = handle_get(t, thread, OBJ_THREAD, RIGHT_SIGNAL, &to, NULL);
    if (st != OK)
        return st;
    st = uthread_raise(uthread_from_kobject(to));
    kobject_unref(to);
    return st;
}

int64_t sysc_thread_get_info(handle_t thread, uint64_t out)
{
    SYSC_TABLE(t);
    struct kobject *to;
    status_t st = handle_get(t, thread, OBJ_THREAD, RIGHT_INSPECT, &to, NULL);
    if (st != OK)
        return st;
    struct thread_info info;
    uthread_get_info(uthread_from_kobject(to), &info);
    kobject_unref(to);
    return copy_to_user(out, &info, sizeof(info)) == OK ? OK : ERR_INVALID_ARGS;
}

/* ---- jobs ----------------------------------------------------------------------- */

int64_t sysc_job_create(handle_t parent, uint32_t flags, uint64_t out)
{
    SYSC_TABLE(t);
    if (flags)
        return ERR_INVALID_ARGS;
    struct kobject *jo;
    status_t st = handle_get(t, parent, OBJ_JOB, RIGHT_WRITE, &jo, NULL);
    if (st != OK)
        return st;
    struct job *j;
    st = job_create(job_from_kobject(jo), &j);
    kobject_unref(jo);
    return st == OK ? sysc_publish(t, job_kobject(j), JOB_RIGHTS, out) : st;
}

int64_t sysc_job_set_limit(handle_t job, uint32_t kind, uint64_t value)
{
    SYSC_TABLE(t);
    struct kobject *jo;
    status_t st = handle_get(t, job, OBJ_JOB, RIGHT_MANAGE, &jo, NULL);
    if (st != OK)
        return st;
    st = job_set_limit(job_from_kobject(jo), kind, value);
    kobject_unref(jo);
    return st;
}

int64_t sysc_job_kill(handle_t job)
{
    SYSC_TABLE(t);
    struct kobject *jo;
    status_t st = handle_get(t, job, OBJ_JOB, RIGHT_MANAGE, &jo, NULL);
    if (st != OK)
        return st;
    unsigned killed = 0;
    st = job_kill(job_from_kobject(jo), &killed);
    kprintf("user: job koid %lu killed by \"%s\" (%u process%s)\n", jo->koid,
            process_name(process_current()), killed, killed == 1 ? "" : "es");
    kobject_unref(jo);
    return st;
}

int64_t sysc_job_get_info(handle_t job, uint64_t out)
{
    SYSC_TABLE(t);
    struct kobject *jo;
    status_t st = handle_get(t, job, OBJ_JOB, RIGHT_INSPECT, &jo, NULL);
    if (st != OK)
        return st;
    struct job_info info;
    job_get_info(job_from_kobject(jo), &info);
    kobject_unref(jo);
    return copy_to_user(out, &info, sizeof(info));
}
