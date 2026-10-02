/* System calls 130-133: read-only system information for the shell's
 * uname / free / lscpu / top / ps / date. Each needs a power on a RES_ROOT
 * handle (sysinfo_check_root): RIGHT_ROOT_SYSINFO for sys_info, cpu_stat
 * and proc_list, RIGHT_ROOT_CLOCK for rtc_read; none changes anything.
 * sysinfo_check_root is also every other root call's check (kernel/abi/
 * sysc_console.c, sysc_kexec.c, vmo_sys.c). The structs are in
 * <jam/abi.h>; the rules every sysc_* follows are in sysc.h. */
#include <jam/cpu.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/rtc.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/syscall_impl.h>
#include <jam/sysinfo.h>
#include <jam/time.h>
#include "sysc.h"

#define PROC_LIST_MAX 1024

status_t sysinfo_check_root(struct handle_table *t, handle_t root, rights_t need)
{
    struct kobject *obj;
    status_t st = handle_get(t, root, OBJ_RESOURCE, need, &obj, NULL);
    if (st != OK)
        return st;
    if (resource_kind(obj) != RES_ROOT)
        st = ERR_WRONG_TYPE;
    kobject_unref(obj);
    return st;
}

/* Copy src into dst[cap] without leading/trailing spaces (CPUID pads the
 * brand string). */
static void trimmed(char *dst, size_t cap, const char *src)
{
    while (*src == ' ')
        src++;
    size_t n = strlen(src);
    while (n && src[n - 1] == ' ')
        n--;
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void sysinfo_fill(struct sys_info *s)
{
    memset(s, 0, sizeof(*s));
    trimmed(s->version, sizeof(s->version), jamos_version);
    trimmed(s->cpu_vendor, sizeof(s->cpu_vendor), cpu_features.vendor);
    trimmed(s->cpu_brand, sizeof(s->cpu_brand), cpu_features.brand);
    s->uptime_ns = uptime_ns();
    s->tsc_hz = tsc_hz;
    pmm_stats(&s->mem_total_pages, &s->mem_free_pages);
    s->stack_cache_pages = sched_stack_cache_pages();
    for (uint32_t i = 0; i < cpu_count; i++)
        s->cpu_count += cpus[i] && cpu_online(cpus[i]);
    s->flags = cpu_features.hybrid ? SYSINFO_HYBRID : 0;
#ifndef JAM_NO_KTESTS
    s->flags |= SYSINFO_KTESTS;
#endif
}

void sysinfo_cpu(uint32_t i, struct cpu_stat *s)
{
    memset(s, 0, sizeof(*s));
    struct cpu *c = i < cpu_count ? cpus[i] : NULL;
    s->index = i;
    if (!c)
        return;
    s->apic_id = c->lapic_id;
    s->type = (uint32_t)c->type;
    s->core_id = c->core_id;
    s->smt_id = c->smt_id;
    s->online = cpu_online(c);
    s->idle_ns = tsc_to_ns(sched_cpu_idle_tsc(i));
    s->switches = __atomic_load_n(&c->switches, __ATOMIC_RELAXED);
}

int64_t sysc_sys_info(handle_t root, uint64_t out)
{
    SYSC_TABLE(t);
    status_t st = sysinfo_check_root(t, root, RIGHT_ROOT_SYSINFO);
    if (st != OK)
        return st;
    struct sys_info s;
    sysinfo_fill(&s);
    return copy_to_user(out, &s, sizeof(s)) == OK ? OK : ERR_INVALID_ARGS;
}

int64_t sysc_cpu_stat(handle_t root, uint32_t first, uint64_t out, uint32_t cap)
{
    SYSC_TABLE(t);
    status_t st = sysinfo_check_root(t, root, RIGHT_ROOT_SYSINFO);
    if (st != OK)
        return st;
    uint32_t n = 0;
    for (uint32_t i = first; i < cpu_count && n < cap; i++, n++) {
        struct cpu_stat s;
        sysinfo_cpu(i, &s);
        if (copy_to_user(out + (uint64_t)n * sizeof(s), &s, sizeof(s)) != OK)
            return ERR_INVALID_ARGS;
    }
    return n;
}

int64_t sysc_proc_list(handle_t root, uint64_t out, uint32_t cap)
{
    SYSC_TABLE(t);
    status_t st = sysinfo_check_root(t, root, RIGHT_ROOT_SYSINFO);
    if (st != OK)
        return st;
    if (cap > PROC_LIST_MAX)
        cap = PROC_LIST_MAX;
    if (!cap)
        return 0;
    struct proc_stat *buf = kmalloc((size_t)cap * sizeof(*buf));
    if (!buf)
        return ERR_NO_MEMORY;
    struct job *scope = job_root_of(job_current());
    uint32_t n = 0;
    if (scope)
        job_list_processes(scope, 0, buf, cap, &n);
    job_unref(scope);
    st = copy_out(out, buf, (size_t)n * sizeof(*buf));
    kfree(buf);
    return st == OK ? (int64_t)n : ERR_INVALID_ARGS;
}

int64_t sysc_rtc_read(handle_t root, uint64_t out)
{
    SYSC_TABLE(t);
    status_t st = sysinfo_check_root(t, root, RIGHT_ROOT_CLOCK);
    if (st != OK)
        return st;
    struct rtc_time r;
    st = rtc_read(&r);
    if (st != OK)
        return st;
    return copy_to_user(out, &r, sizeof(r)) == OK ? OK : ERR_INVALID_ARGS;
}
