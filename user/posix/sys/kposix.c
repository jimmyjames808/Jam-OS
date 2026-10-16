/* The kernel's calls for POSIX, in one place (layer.h, "the kernel's
 * calls for POSIX"): thread_set_tid_word, thread_get_info, the process's
 * CPU time (process_info.cpu_ns), process_get_maps (/proc) and
 * sys_public_info (docs/M13-PLAN.md,
 * "The new and changed interfaces"). The rest of the layer goes through
 * these, so a change to those calls' shapes is a change to this file. */
#include "layer.h"

int64_t kp_set_tid_word(uint32_t *word)
{
    return jam_thread_set_tid_word(word);
}

status_t kp_thread_tid(handle_t t, uint32_t *tid)
{
    struct thread_info info;
    status_t st = jam_thread_get_info(t, &info);
    if (st == OK)
        *tid = info.tid;
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

status_t kp_public_info(struct kp_public *out)
{
    struct sys_public_info info;
    status_t st = jam_sys_public_info(&info);
    if (st != OK)
        return st;
    _Static_assert(sizeof(out->version) == sizeof(info.version), "the same version field");
    memset(out, 0, sizeof(*out));
    memcpy(out->version, info.version, sizeof(out->version) - 1);   /* NUL kept by the memset */
    out->cpus = info.cpus_online;
    out->pages = info.mem_total_pages;
    out->free_pages = info.mem_free_pages;
    out->page_size = info.page_size;
    return OK;
}

long kp_process_maps(handle_t proc, uint64_t from, struct map_info *out, uint32_t cap)
{
    int64_t n = jam_process_get_maps(proc, from, out, cap);
    return n < 0 ? lx_err((status_t)n) : (long)n;
}
