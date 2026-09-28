/* Panic screen: message, registers (for exceptions), a symbolised
 * frame-pointer backtrace and the tail of the kernel log. M3 halts the other
 * CPUs with an IPI first. */
#include <stdarg.h>
#include <stdint.h>
#include <jam/fbcon.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/ksyms.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/string.h>
#include <jam/trap.h>
#include <jam/x86.h>

#define MAX_FRAMES   24
#define TAIL_BYTES   2048
#define KERNEL_SPACE 0xffff800000000000ull

static volatile int panicking;
static char tail[TAIL_BYTES + 1];

static const char *const exception_names[32] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range",
    "invalid opcode", "device not available", "double fault",
    "coprocessor overrun", "invalid TSS", "segment not present",
    "stack fault", "general protection fault", "page fault", "reserved",
    "x87 FP error", "alignment check", "machine check", "SIMD FP error",
    "virtualization", "control protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved", "hypervisor injection",
    "VMM communication", "security", "reserved",
};

_Noreturn void halt_forever(void)
{
    for (;;) {
        cli();
        hlt();
    }
}

static void print_addr(int index, uint64_t addr)
{
    uint64_t off;
    const char *name = ksym_lookup(addr, &off);
    if (name)
        kprintf("  #%-2d %016lx  %s+0x%lx\n", index, addr, name, off);
    else
        kprintf("  #%-2d %016lx  ?\n", index, addr);
}

/* Walk saved frame pointers from rbp. first_rip, if nonzero, is printed as
 * frame #0 (the faulting instruction). */
static void backtrace_from(uint64_t first_rip, uint64_t rbp_val)
{
    kprintf("backtrace:\n");
    int i = 0;
    if (first_rip)
        print_addr(i++, first_rip);
    uint64_t *rbp = (uint64_t *)rbp_val;
    uint64_t last = 0, repeats = 0;
    /* Deep recursion would fill the screen with one line; collapse runs.
     * The walk itself is bounded so a corrupt chain can't loop forever. */
    for (int walked = 0; walked < 4096 && i < MAX_FRAMES; walked++) {
        if ((uint64_t)rbp < KERNEL_SPACE || ((uint64_t)rbp & 7))
            break;
        uint64_t ret = rbp[1];
        if (!ret)
            break;
        if (ret == last) {
            repeats++;
        } else {
            if (repeats)
                kprintf("       ... same frame %lu more times\n", repeats);
            repeats = 0;
            print_addr(i++, ret);
            last = ret;
        }
        uint64_t *next = (uint64_t *)rbp[0];
        if (next <= rbp)   /* stacks grow down, so callers are higher up */
            break;
        rbp = next;
    }
    if (repeats)
        kprintf("       ... same frame %lu more times\n", repeats);
}

/* Common start of every panic: stop interrupts, stop recursion, grab the
 * log tail before we overwrite the screen, then paint it red. */
static void panic_begin(void)
{
    cli();
    if (__atomic_exchange_n(&panicking, 1, __ATOMIC_SEQ_CST))
        halt_forever();   /* panic inside panic: stop, don't recurse */

    size_t n = klog_tail(tail, TAIL_BYTES);
    tail[n] = '\0';

    fbcon_force_unlock();
    fbcon_set_colors(0xffffff, 0x8b0000);
    fbcon_clear();
    kprintf("\n  *** JAM OS KERNEL PANIC ***\n\n");
}

_Noreturn static void panic_end(void)
{
    /* Show the tail of the log starting at a line boundary. */
    const char *start = tail;
    for (const char *p = tail; *p; p++)
        if (*p == '\n' && p[1]) {
            start = p + 1;
            break;
        }
    /* Written directly: the tail is longer than kprintf's line buffer. */
    kprintf("\nlast log lines:\n");
    klog_write(start, strlen(start));
    kprintf("\n\nsystem halted.\n");
    halt_forever();
}

_Noreturn void panic(const char *fmt, ...)
{
    panic_begin();

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    kprintf("  %s\n\n", msg);

    backtrace_from(0, (uint64_t)__builtin_frame_address(0));
    panic_end();
}

static void describe_page_fault(const struct trap_frame *f, uint64_t cr2)
{
    uint64_t e = f->error;
    kprintf("  %s-mode %s of %s address %016lx%s%s\n",
            (e & 4) ? "user" : "kernel",
            (e & 16) ? "instruction fetch" : (e & 2) ? "write" : "read",
            (e & 1) ? "protected" : "unmapped", cr2,
            (e & 8) ? " (reserved bit set)" : "",
            (e & 32) ? " (protection key)" : "");
    if (cr2 < 0x10000)
        kprintf("  (address is near zero: probably a NULL pointer)\n");
}

_Noreturn void panic_trap(const struct trap_frame *f)
{
    uint64_t cr2 = read_cr2();
    panic_begin();

    const char *name = f->vector < 32 ? exception_names[f->vector] : "interrupt";
    uint64_t off;
    const char *sym = ksym_lookup(f->rip, &off);
    kprintf("  %s (vector %lu, error %lx) at %s+0x%lx\n\n", name, f->vector,
            f->error, sym ? sym : "?", sym ? off : f->rip);
    if (f->vector == 14)
        describe_page_fault(f, cr2);
    /* A fault on the guard page can't push its frame, so it escalates to a
     * double fault; CR2 still holds the address that faulted. */
    if (f->vector == 8 && cr2 + PAGE_SIZE >= f->rsp && cr2 < f->rsp + PAGE_SIZE)
        kprintf("  kernel stack overflow: hit the guard page at %016lx\n", cr2);

    kprintf("\n  RIP %016lx  RSP %016lx  RFLAGS %016lx\n", f->rip, f->rsp, f->rflags);
    kprintf("  RAX %016lx  RBX %016lx  RCX %016lx\n", f->rax, f->rbx, f->rcx);
    kprintf("  RDX %016lx  RSI %016lx  RDI %016lx\n", f->rdx, f->rsi, f->rdi);
    kprintf("  RBP %016lx  R8  %016lx  R9  %016lx\n", f->rbp, f->r8, f->r9);
    kprintf("  R10 %016lx  R11 %016lx  R12 %016lx\n", f->r10, f->r11, f->r12);
    kprintf("  R13 %016lx  R14 %016lx  R15 %016lx\n", f->r13, f->r14, f->r15);
    kprintf("  CS  %04lx  SS %04lx  CR0 %016lx  CR2 %016lx\n", f->cs, f->ss,
            read_cr0(), cr2);
    kprintf("  CR3 %016lx  CR4 %016lx\n\n", read_cr3(), read_cr4());

    backtrace_from(f->rip, f->rbp);
    panic_end();
}
