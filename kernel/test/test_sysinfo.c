/* System information for the shell (syscalls 130-133, kernel/abi/sysc_sysinfo.c)
 * and the CPU-time accounting under it (sched.c account_switch, process.c
 * process_cpu_tsc). The QEMU shell test (tools/shell-tests/cmds.txt) runs
 * the calls for real from the shell. */
#include <jam/abi.h>
#include <jam/event.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/percpu.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/rtc.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/sysinfo.h>
#include <jam/time.h>
#include <jam/userboot.h>


static handle_t put_handle(struct handle_table *t, struct kobject *obj, rights_t rights)
{
    struct khandle kh = khandle_from_new(obj, rights);
    handle_t h;
    KT_EQ(handle_insert(t, &kh, &h), OK);
    return h;
}

/* Every call of the block needs RIGHT_READ on a RES_ROOT. */
KTEST(sysinfo_rights)
{
    struct handle_table t;
    handle_table_init(&t);
    struct kobject *root = resource_root();
    kobject_ref(root);
    handle_t rd = put_handle(&t, root, RIGHTS_BASIC | RIGHT_READ);
    handle_t nord = put_handle(&t, root, RIGHTS_BASIC | RIGHT_MANAGE);
    struct kobject *pci;
    KT_EQ(resource_create(root, RES_PCI, 0, 0, &pci), OK);
    handle_t pcih = put_handle(&t, pci, RIGHTS_BASIC | RIGHT_READ);
    struct event *ev;
    KT_EQ(event_create(&ev), OK);
    handle_t evh = put_handle(&t, &ev->base, RIGHTS_BASIC | RIGHT_READ);

    KT_EQ(sysinfo_check_root(&t, rd), OK);
    KT_EQ(sysinfo_check_root(&t, nord), ERR_ACCESS_DENIED);
    KT_EQ(sysinfo_check_root(&t, pcih), ERR_WRONG_TYPE);
    KT_EQ(sysinfo_check_root(&t, evh), ERR_WRONG_TYPE);
    KT_EQ(sysinfo_check_root(&t, HANDLE_INVALID), ERR_BAD_HANDLE);
    handle_close(&t, rd);
    KT_EQ(sysinfo_check_root(&t, rd), ERR_BAD_HANDLE);
    handle_table_destroy(&t);
}

KTEST(sysinfo_fill_sane)
{
    struct sys_info s;
    sysinfo_fill(&s);
    kprintf("sysinfo_fill_sane: \"%s\" %s \"%s\", %u CPUs, %lu/%lu pages free, tsc %lu Hz\n",
            s.version, s.cpu_vendor, s.cpu_brand, s.cpu_count, s.mem_free_pages,
            s.mem_total_pages, s.tsc_hz);
    KT_ASSERT(s.version[0] == '0' || s.version[0] == '1');
    KT_EQ(s.version[sizeof(s.version) - 1], 0);
    KT_EQ(s.cpu_brand[sizeof(s.cpu_brand) - 1], 0);
    KT_ASSERT(s.cpu_brand[0] != ' ');
    KT_EQ(s.cpu_count, cpu_count);
    KT_ASSERT(s.mem_free_pages > 0 && s.mem_free_pages <= s.mem_total_pages);
    KT_EQ(s.tsc_hz, tsc_hz);
    KT_ASSERT(s.uptime_ns > 0);
    KT_ASSERT(s.flags & SYSINFO_KTESTS);
    struct cpu_stat c;
    sysinfo_cpu(0, &c);
    KT_EQ(c.index, 0);
    KT_EQ(c.online, 1);
    KT_EQ(c.apic_id, cpus[0]->lapic_id);
    sysinfo_cpu(cpu_count, &c);   /* past the end: zeros, no crash */
    KT_EQ(c.online, 0);
}

/* The decoder: BCD and binary, 12- and 24-hour. */
KTEST(sysinfo_rtc_decode)
{
    struct rtc_time r;
    /* 2026-09-29 23:59:58, BCD, 24-hour (register B = 0x02). */
    const uint8_t a[6] = { 0x58, 0x59, 0x23, 0x29, 0x09, 0x26 };
    rtc_decode(a, 0x02, &r);
    KT_EQ(r.year, 2026);
    KT_EQ(r.month, 9);
    KT_EQ(r.day, 29);
    KT_EQ(r.hour, 23);
    KT_EQ(r.minute, 59);
    KT_EQ(r.second, 58);
    /* 12-hour BCD: 0x92 = PM + 12 -> 12; 0x12 = 12 AM -> 0; 0x81 = 1 PM. */
    const uint8_t b[6] = { 0, 0, 0x92, 1, 1, 0 };
    rtc_decode(b, 0x00, &r);
    KT_EQ(r.hour, 12);
    KT_EQ(r.year, 2000);
    const uint8_t c[6] = { 0, 0, 0x12, 1, 1, 0 };
    rtc_decode(c, 0x00, &r);
    KT_EQ(r.hour, 0);
    const uint8_t d[6] = { 0, 0, 0x81, 1, 1, 0 };
    rtc_decode(d, 0x00, &r);
    KT_EQ(r.hour, 13);
    /* Binary, 24-hour (B = 0x06). */
    const uint8_t e[6] = { 7, 8, 21, 31, 12, 99 };
    rtc_decode(e, 0x06, &r);
    KT_EQ(r.year, 2099);
    KT_EQ(r.month, 12);
    KT_EQ(r.day, 31);
    KT_EQ(r.hour, 21);
    KT_EQ(r.minute, 8);
    KT_EQ(r.second, 7);
}

/* The real chip: a sane date, and it ticks. */
KTEST(sysinfo_rtc_read)
{
    struct rtc_time a, b;
    KT_EQ(rtc_read(&a), OK);
    kprintf("sysinfo_rtc_read: %04u-%02u-%02u %02u:%02u:%02u (register B %#x)\n", a.year,
            a.month, a.day, a.hour, a.minute, a.second, a.status_b);
    KT_ASSERT(a.year >= 2024 && a.year <= 2099);
    thread_sleep_ms(1100);
    KT_EQ(rtc_read(&b), OK);
    uint32_t sa = a.hour * 3600u + a.minute * 60u + a.second;
    uint32_t sb = b.hour * 3600u + b.minute * 60u + b.second;
    if (sb < sa)
        sb += 86400;   /* midnight */
    KT_ASSERT(sb - sa >= 1 && sb - sa <= 3);
    KT_ASSERT(b.uptime_ns > a.uptime_ns);
}

static void spin_for(void *arg)
{
    uint64_t end = uptime_ns() + (uint64_t)(uintptr_t)arg;
    while (uptime_ns() < end)
        __asm__ volatile("pause");
}

static void sleep_for(void *arg)
{
    thread_sleep_ns((uint64_t)(uintptr_t)arg);
}

/* A spinning thread is charged about its run time; a sleeping one almost
 * nothing; while this thread sleeps, the CPUs' idle time grows. */
KTEST(sysinfo_thread_cpu_time)
{
    uint64_t idle0 = 0, idle1 = 0;
    for (uint32_t i = 0; i < cpu_count; i++)
        idle0 += sched_cpu_idle_tsc(i);
    struct thread *sp = thread_create("ktspin", spin_for, (void *)(uintptr_t)(100 * NS_PER_MS),
                                      PRIO_DEFAULT);
    struct thread *sl = thread_create("ktsleep", sleep_for, (void *)(uintptr_t)(100 * NS_PER_MS),
                                      PRIO_DEFAULT);
    thread_sleep_ms(50);
    uint64_t mid = tsc_to_ns(thread_cpu_tsc(sp));   /* while it runs: the current run counts */
    thread_sleep_ms(100);
    for (uint32_t i = 0; i < cpu_count; i++)
        idle1 += sched_cpu_idle_tsc(i);
    /* Joined threads are still readable until the join drops the reference. */
    uint64_t spin_ns = tsc_to_ns(sp->run_tsc), sleep_ns = tsc_to_ns(sl->run_tsc);
    thread_join(sp);
    thread_join(sl);
    uint64_t idle_ns = tsc_to_ns(idle1 - idle0);
    kprintf("sysinfo_thread_cpu_time: spinner %lu us (%lu us at 50 ms), sleeper %lu us, "
            "idle %lu us over %u CPUs\n", spin_ns / 1000, mid / 1000, sleep_ns / 1000,
            idle_ns / 1000, cpu_count);
    KT_ASSERT(spin_ns >= 60 * NS_PER_MS && spin_ns <= 400 * NS_PER_MS);
    KT_ASSERT(mid >= 10 * NS_PER_MS);
    KT_ASSERT(sleep_ns < 20 * NS_PER_MS);
    if (cpu_count >= 3)
        KT_ASSERT(idle_ns >= 50 * NS_PER_MS);
}

/* A process's CPU time: live (its spinning thread) and after it died
 * (its threads' time kept); proc_list's walk finds it with that time. */
KTEST(sysinfo_process_cpu_time)
{
    struct job *root, *j;
    KT_EQ(userboot_root_job(&root), OK);
    KT_EQ(job_create(root, &j), OK);
    const char *argv[] = { "utest", "spin" };
    struct process *p;
    KT_EQ(userboot_spawn("bin/utest", argv, 2, j, NULL, 0, NULL, &p), OK);
    thread_sleep_ms(200);
    uint64_t live = tsc_to_ns(process_cpu_tsc(p));

    struct proc_stat ps[4];
    uint32_t n = 0;
    job_list_processes(root, 0, ps, 4, &n);
    KT_EQ(n, 1);
    KT_ASSERT(!strcmp(ps[0].name, "utest"));
    KT_EQ(ps[0].depth, 1);
    KT_EQ(ps[0].job_koid, job_kobject(j)->koid);
    KT_EQ(ps[0].state, PROCESS_RUNNING);
    KT_EQ(ps[0].threads, 1);
    KT_ASSERT(ps[0].cpu_ns >= live);
    KT_ASSERT(ps[0].job_pages > 0);

    process_kill(p, PROCESS_KILLED_CODE, true);
    KT_EQ(object_wait_one(process_kobject(p), SIG_TERMINATED, uptime_ns() + 10 * NS_PER_S, NULL),
          OK);
    uint64_t dead = tsc_to_ns(process_cpu_tsc(p));
    kprintf("sysinfo_process_cpu_time: %lu us after 200 ms spinning, %lu us once dead\n",
            live / 1000, dead / 1000);
    KT_ASSERT(live >= 50 * NS_PER_MS && live <= 1000 * NS_PER_MS);
    KT_ASSERT(dead >= live);
    n = 0;
    job_list_processes(root, 0, ps, 4, &n);
    KT_EQ(n, 0);   /* dead processes are off the list */
    kobject_unref(process_kobject(p));
    job_unref(j);
    job_unref(root);
}
