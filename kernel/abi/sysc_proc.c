/* System calls: processes, threads and jobs. The rules all sysc_* follow
 * are in sysc.h; the objects are in object/process.c and object/job.c.
 *
 * Rights: creating a process in a job, or a child job, needs RIGHT_WRITE on
 * the job; starting, killing and making threads in a process needs
 * RIGHT_WRITE on it; starting a thread or changing its priority needs
 * RIGHT_WRITE on the thread; reading info needs RIGHT_INSPECT. */
#include <jam/aspace.h>
#include <jam/kprintf.h>
#include <jam/sched.h>
#include <jam/syscall_impl.h>
#include <jam/uentry.h>
#include <jam/vmar.h>
#include "sysc.h"

#define VMAR_HANDLE_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE)

/* A name argument: at most PROCESS_NAME_MAX - 1 bytes are kept. */
static status_t copy_name(char *dst, uint64_t uname, uint64_t len)
{
    if (len >= PROCESS_NAME_MAX)
        len = PROCESS_NAME_MAX - 1;
    status_t st = copy_in(dst, uname, len);
    dst[st == OK ? len : 0] = '\0';
    return st;
}

int64_t sysc_process_create(handle_t job, uint64_t name, uint64_t name_len, uint32_t flags,
                            uint64_t proc_out, uint64_t vmar_out)
{
    SYSC_TABLE(t);
    if (flags)
        return ERR_INVALID_ARGS;
    char kname[PROCESS_NAME_MAX];
    if (copy_name(kname, name, name_len) != OK)
        return ERR_INVALID_ARGS;
    struct kobject *jo;
    status_t st = handle_get(t, job, OBJ_JOB, RIGHT_WRITE, &jo, NULL);
    if (st != OK)
        return st;
    struct process *p;
    st = process_create(job_from_kobject(jo), kname, &p);
    kobject_unref(jo);
    if (st != OK)
        return st;
    struct aspace *as = process_aspace(p);
    struct vmar *v = NULL;
    st = vmar_create_for(as, &v);
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
    struct kobject *po, *to;
    status_t st = handle_get(t, proc, OBJ_PROCESS, RIGHT_WRITE, &po, NULL);
    if (st != OK)
        return st;
    st = handle_get(t, thread, OBJ_THREAD, RIGHT_WRITE, &to, NULL);
    if (st != OK) {
        kobject_unref(po);
        return st;
    }
    struct khandle kh = { NULL, 0 };
    if (arg0 != HANDLE_INVALID)
        st = handle_take(t, arg0, &kh);   /* reserved in our table until we know */
    if (st == OK) {
        st = process_start(process_from_kobject(po), uthread_from_kobject(to), entry, stack, &kh,
                           arg1, NULL);
        if (arg0 != HANDLE_INVALID) {
            handle_t back;
            if (st == OK)
                handle_commit(t, arg0);           /* it lives in the child now */
            else if (handle_untake(t, arg0, &kh, &back) != OK)
                khandle_release(&kh);
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
    process_get_info(process_from_kobject(po), &info);
    kobject_unref(po);
    return copy_to_user(out, &info, sizeof(info));
}

int64_t sysc_thread_create(handle_t proc, uint64_t name, uint64_t name_len, uint32_t flags,
                           uint64_t out)
{
    SYSC_TABLE(t);
    if (flags)
        return ERR_INVALID_ARGS;
    char kname[PROCESS_NAME_MAX];
    if (copy_name(kname, name, name_len) != OK)
        return ERR_INVALID_ARGS;
    struct kobject *po;
    status_t st = handle_get(t, proc, OBJ_PROCESS, RIGHT_WRITE, &po, NULL);
    if (st != OK)
        return st;
    struct uthread *u;
    st = uthread_create(process_from_kobject(po), kname, &u);
    kobject_unref(po);
    return st == OK ? sysc_publish(t, uthread_kobject(u), THREAD_RIGHTS, out) : st;
}

int64_t sysc_thread_start(handle_t thread, uint64_t entry, uint64_t stack, uint64_t arg0,
                          uint64_t arg1)
{
    SYSC_TABLE(t);
    struct kobject *to;
    status_t st = handle_get(t, thread, OBJ_THREAD, RIGHT_WRITE, &to, NULL);
    if (st != OK)
        return st;
    st = uthread_start(uthread_from_kobject(to), entry, stack, arg0, arg1, NULL);
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
    status_t st = handle_get(t, job, OBJ_JOB, RIGHT_WRITE, &jo, NULL);
    if (st != OK)
        return st;
    st = job_set_limit(job_from_kobject(jo), kind, value);
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
