/* The panic screen's code, JAM-<kind>-<4 hex> (<jam/panicscreen.h> has
 * the table): what kind of panic, and the low 16 bits of where. Pure:
 * reads its arguments, writes the struct; the panic path calls it with
 * nothing else working. */
#include <stddef.h>
#include <jam/kprintf.h>
#include <jam/panicscreen.h>

/* Each exception's mnemonic and name (Intel SDM vol. 3, 6.15); NULL for
 * the reserved vectors and 9 (the coprocessor segment overrun, which has
 * none): those are EX. */
static const struct {
    const char *kind, *what;
} exceptions[32] = {
    [0] = { "DE", "divide error" },          [1] = { "DB", "debug exception" },
    [2] = { "NMI", "non-maskable interrupt" }, [3] = { "BP", "breakpoint" },
    [4] = { "OF", "overflow" },              [5] = { "BR", "bound range exceeded" },
    [6] = { "UD", "invalid opcode" },        [7] = { "NM", "device not available" },
    [8] = { "DF", "double fault" },          [10] = { "TS", "invalid TSS" },
    [11] = { "NP", "segment not present" },  [12] = { "SS", "stack fault" },
    [13] = { "GP", "general protection fault" }, [14] = { "PF", "page fault" },
    [16] = { "MF", "x87 floating-point error" }, [17] = { "AC", "alignment check" },
    [18] = { "MC", "machine check" },        [19] = { "XM", "SIMD floating-point error" },
    [20] = { "VE", "virtualization exception" }, [21] = { "CP", "control protection" },
    [28] = { "HV", "hypervisor injection" }, [29] = { "VC", "VMM communication" },
    [30] = { "SX", "security exception" },
};

static void set(struct panic_cause *c, const char *kind, const char *what, const char *hex_of,
                uint64_t addr)
{
    c->kind = kind;
    c->what = what;
    c->hex_of = hex_of;
    c->addr = addr;
    ksnprintf(c->code, sizeof(c->code), "JAM-%s-%04lX", kind, addr & 0xffff);
}

void panic_cause_trap(struct panic_cause *c, uint64_t vector, uint64_t rip, uint64_t cr2)
{
    if (vector == 14) {
        set(c, "PF", exceptions[14].what, "the faulting address", cr2);
        return;
    }
    if (vector < 32 && exceptions[vector].kind) {
        set(c, exceptions[vector].kind, exceptions[vector].what, "RIP", rip);
        return;
    }
    set(c, "EX", vector < 32 ? "a reserved exception" : "an unexpected interrupt", "RIP", rip);
}

void panic_cause_watchdog(struct panic_cause *c, uint64_t rip)
{
    set(c, "WD", "a CPU stopped taking timer interrupts (the watchdog)", "RIP", rip);
}

static bool starts_with(const char *s, const char *p)
{
    for (; *p; s++, p++)
        if (*s != *p)
            return false;
    return true;
}

static bool contains(const char *s, const char *p)
{
    for (; *s; s++)
        if (starts_with(s, p))
            return true;
    return false;
}

void panic_cause_message(struct panic_cause *c, const char *msg, uint64_t caller)
{
    static const char *const where = "the address panic() was called from";
    if (starts_with(msg, "assertion failed"))
        set(c, "AS", "an assertion failed", where, caller);
    else if (contains(msg, "stuck for"))
        set(c, "WD", "a lock stayed taken (a lockup)", where, caller);
    else if (starts_with(msg, "lockdep:") || contains(msg, "mutex"))
        set(c, "LK", "a lock was used wrongly", where, caller);
    else if (contains(msg, "out of memory"))
        set(c, "OOM", "the kernel ran out of memory", where, caller);
    else
        set(c, "KP", "a kernel panic (a broken invariant)", where, caller);
}
