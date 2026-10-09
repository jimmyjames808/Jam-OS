/* The kernel's calls for POSIX, in one place (layer.h, "the kernel's
 * calls for POSIX"): thread_set_tid_word, thread_get_info, the process's
 * CPU time and sys_public_info (docs/M13-PLAN.md, "The new and changed
 * interfaces").
 *
 * Each is used only when the kernel's call list (<jam/syscall_nums.h>)
 * has it. Without thread_set_tid_word nothing could tell a joiner that a
 * thread is off its stack, and musl's thread list lock (the tid word of
 * every thread) would never be given back, so no thread but the first is
 * made: kp_threads is false and __clone answers -EAGAIN. The first
 * thread's id is then 1 (it is the only one). */
#include "layer.h"

#ifdef SYS_thread_set_tid_word

bool kp_threads(void)
{
    return true;
}

int64_t kp_set_tid_word(uint32_t *word)
{
    return jam_thread_set_tid_word(word);
}

#else

bool kp_threads(void)
{
    return false;
}

int64_t kp_set_tid_word(uint32_t *word)
{
    (void)word;
    return 1;
}

#endif

#ifdef SYS_thread_get_info

status_t kp_thread_tid(handle_t t, uint32_t *tid)
{
    struct thread_info info;
    status_t st = jam_thread_get_info(t, &info);
    if (st == OK)
        *tid = (uint32_t)info.tid;
    return st;
}

status_t kp_cpu_ns(handle_t proc, handle_t thread, uint64_t *ns)
{
    if (thread != HANDLE_INVALID) {
        struct thread_info info;
        status_t st = jam_thread_get_info(thread, &info);
        if (st == OK)
            *ns = info.cpu_ns;
        return st;
    }
    struct process_info info;
    status_t st = jam_process_get_info(proc, &info);
    if (st == OK)
        *ns = info.cpu_ns;
    return st;
}

#else

status_t kp_thread_tid(handle_t t, uint32_t *tid)
{
    (void)t;
    (void)tid;
    return ERR_NOT_SUPPORTED;
}

status_t kp_cpu_ns(handle_t proc, handle_t thread, uint64_t *ns)
{
    (void)proc;
    (void)thread;
    (void)ns;
    return ERR_NOT_SUPPORTED;
}

#endif

#ifdef SYS_sys_public_info

status_t kp_public_info(struct kp_public *out)
{
    struct sys_public_info info;
    status_t st = jam_sys_public_info(&info);
    if (st != OK)
        return st;
    memset(out, 0, sizeof(*out));
    memcpy(out->version, info.version, sizeof(out->version) - 1 < sizeof(info.version)
                                           ? sizeof(out->version) - 1 : sizeof(info.version));
    out->cpus = info.cpus_online;
    out->pages = info.total_pages;
    out->page_size = info.page_size;
    return OK;
}

#else

status_t kp_public_info(struct kp_public *out)
{
    (void)out;
    return ERR_NOT_SUPPORTED;
}

#endif
