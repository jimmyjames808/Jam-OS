#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/time.h>

extern const struct ktest __ktests_start[], __ktests_end[];

const char *ktest_current = "?";

int ktest_run(const char *prefix)
{
    size_t pl = strlen(prefix);
    int ran = 0;
    uint64_t total, free_before, free_after;
    for (const struct ktest *t = __ktests_start; t < __ktests_end; t++) {
        if (memcmp(t->name, prefix, pl))
            continue;
        ktest_current = t->name;
        pmm_stats(&total, &free_before);
        uint64_t t0 = uptime_ns();
        t->fn();
        uint64_t us = (uptime_ns() - t0) / 1000;
        pmm_stats(&total, &free_after);
        long leaked = (long)(free_before - free_after);
        kprintf("ktest: %-32s ok  %lu.%03lu ms%s\n", t->name, us / 1000, us % 1000,
                leaked > 32 ? "  (note: pages not returned)" : "");
        ran++;
    }
    kprintf("ktest: %d test(s) passed (%u of 64 lock classes in use)\n", ran,
            lockdep_class_count());
    return ran;
}
