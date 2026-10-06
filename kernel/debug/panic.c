/* Panic: message, registers (for exceptions), a symbolised frame-pointer
 * backtrace, the panic's code (<jam/panicscreen.h>) and the tail of the
 * kernel log, into the log and onto the serial port. The other CPUs are
 * halted with an NMI first. The screen is the calm panic screen
 * (debug/panicscreen.c), never the log's text: "Jam OS hit a problem and
 * is restarting" while the stored kernel is started (kernel/kexec/
 * jump.c), whose boot saves this one's log and says what happened; or,
 * without a stored kernel to start (none, a damaged one, or a crash
 * loop), "... can't recover from", its details and a firmware reset. */
#include <stdarg.h>
#include <stdint.h>
#include <jam/console_svc.h>
#include <jam/fbcon.h>
#include <jam/ipi.h>
#include <jam/kexec.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/ksyms.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/panicscreen.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/serial.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/sysinfo.h>
#include <jam/time.h>
#include <jam/trap.h>
#include <jam/x86.h>

#define MAX_FRAMES   24
#define TAIL_BYTES   2048
#define KERNEL_SPACE 0xffff800000000000ull

#define NOTE_MAX     320
#define KEXEC_MESSAGE_LINE 160   /* an exception's one line */

int panic_in_progress;   /* set once, by the first CPU to panic */
static uint32_t panic_apic;   /* that CPU's APIC id (cpuid: needs no per-CPU state) */
static char note[NOTE_MAX];   /* panic_note_set's line; its last byte stays 0 */
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

/* One frame, printed; the first few also kept for the panic screen. */
static void print_addr(int index, uint64_t addr)
{
    if (index < PANIC_FRAMES) {
        panic_report.frames[index] = addr;
        panic_report.nframes = (unsigned)index + 1;
    }
    uint64_t off;
    const char *name = ksym_lookup(addr, &off);
    if (name)
        kprintf("  #%-2d %016lx  %s+0x%lx\n", index, addr, name, off);
    else
        kprintf("  #%-2d %016lx  ?\n", index, addr);
}

/* Walk saved frame pointers from rbp. first_rip, if nonzero, is printed as
 * frame #0 (the faulting instruction). */
/* Is [addr, addr+15] backed by a mapping? A corrupt rbp pointing at an
 * unmapped upper-half address would otherwise fault while the backtrace reads
 * it, nesting a #PF and leaving the panic screen half-drawn. */
static bool frame_readable(uint64_t addr)
{
    uint64_t pml4 = vmm_kernel_pml4();
    return vmm_translate(pml4, addr) != UINT64_MAX &&
           vmm_translate(pml4, addr + 8) != UINT64_MAX;
}

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
        if (!frame_readable((uint64_t)rbp))
            break;   /* rbp[0] and rbp[1] are mapped: safe to dereference */
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

static bool jumping;   /* the stored kernel will be started (the screen's first case) */

/* This CPU's APIC id from CPUID (leaf 0Bh's x2APIC id where there is one,
 * else leaf 1's initial id): no LAPIC mapping or GS needed, so it works at
 * any point of a panic. */
static uint32_t apic_id_cpuid(void)
{
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    if (a >= 0xb) {
        cpuid(0xb, 0, &a, &b, &c, &d);
        if (b)
            return d;
    }
    cpuid(1, 0, &a, &b, &c, &d);
    return b >> 24;
}

/* Common start of every panic: stop interrupts, stop recursion, decide
 * whether the stored kernel takes over, the screen dark (the kernel's
 * text console draws nothing more), grab the log tail. */
static void panic_begin(void)
{
    cli();
    if (__atomic_exchange_n(&panic_in_progress, 1, __ATOMIC_SEQ_CST)) {
        /* A fault inside this CPU's own panic after it decided to start the
         * stored kernel: halting would leave a screen that says it is
         * restarting; the second case and a reset instead. Otherwise (a
         * fault while the second case's screen is up, or a second CPU)
         * halt as always. */
        if (__atomic_load_n(&jumping, __ATOMIC_ACQUIRE) && panic_apic == apic_id_cpuid())
            kexec_panic_failed();
        halt_forever();
    }
    panic_apic = apic_id_cpuid();
    lockdep_off();

    /* Stop everyone else first so the screen is ours, then drop any log
     * lock a halted CPU (or this one) was holding. */
    uint32_t halted = ipi_halt_others();
    klog_force_unlock();
    /* No lock, no allocation; the panic's lines start here. */
    __atomic_store_n(&jumping, kexec_panic_begin(), __ATOMIC_RELEASE);
    fbcon_go_dark();   /* the text is kept, never drawn: the panic screen draws */
    panic_screen_begin();
    serial_panic();   /* queued output first, then everything synchronous */

    size_t n = klog_tail(tail, TAIL_BYTES);
    tail[n] = '\0';

    panic_report.cpu = this_cpu()->index;
    panic_report.note = note;
    kprintf("\n  *** JAM OS KERNEL PANIC *** on cpu %u", this_cpu()->index);
    if (this_cpu()->current)
        kprintf(", thread \"%s\"", this_cpu()->current->name);
    if (__atomic_load_n(&ipi_ready, __ATOMIC_ACQUIRE))
        kprintf(" (other CPUs halted: %u)", halted);
    kprintf("\n\n");
}

void panic_note_set(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(note, NOTE_MAX - 1, fmt, ap);
    va_end(ap);
}

/* The panic's one line, for the panic screen's details. */
static void report_message(const char *msg)
{
    kexec_message_clean(panic_report.message, sizeof(panic_report.message), msg);
}

_Noreturn static void panic_end(void)
{
    if (note[0])
        kprintf("\n  %s\n", note);
    const struct panic_cause *c = &panic_report.cause;
    kprintf("\n  code %s: %s; its digits are the low 16 bits of %s (%016lx)\n", c->code,
            c->what, c->hex_of, c->addr);
    kprintf("  build Jam OS %s, %s\n", jamos_version, panic_screen_build());
    if (jumping) {
        /* The log already has the lines before the panic: the next boot
         * saves all of it. */
        kprintf("\nstarting the stored kernel: the next boot saves this log\n");
        panic_screen_restarting();
        kexec_panic_jump();
    }
    /* Show the tail of the log starting at a line boundary. */
    const char *start = tail;
    for (const char *p = tail; *p; p++)
        if (*p == '\n' && p[1]) {
            start = p + 1;
            break;
        }
    /* Written directly: the tail is longer than kprintf's line buffer. */
    kprintf("\nlast log lines:\n");
    klog_write_raw(start, strlen(start));
    const char *why = kexec_panic_why_not();
    if (!why)
        why = "there is no stored kernel (crashkernel=0, not loaded yet, or it couldn't be)";
    kprintf("\n\nno restart: %s\n", why);
    panic_screen_stuck(why);
}

_Noreturn void panic(const char *fmt, ...)
{
    panic_begin();

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    kexec_panic_message(msg);
    report_message(msg);
    panic_report.at = (uint64_t)__builtin_return_address(0);
    panic_cause_message(&panic_report.cause, msg, panic_report.at);
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
    /* A kernel-mode protection fault on a lower-half address can only be
     * SMEP or SMAP: user pages are the only present lower-half pages. */
    if (!(e & 4) && (e & 1) && cr2 < 0x0000800000000000ull) {
        if (e & 16)
            kprintf("  SMEP: the kernel tried to execute a user page\n");
        else
            kprintf("  SMAP: the kernel touched a user page outside copy_from_user/"
                    "copy_to_user\n");
    }
}

static void dump_frame(const struct trap_frame *f, uint64_t cr2);

_Noreturn void panic_watchdog(const struct trap_frame *f)
{
    uint64_t cr2 = read_cr2();
    panic_begin();
    kexec_panic_message("watchdog: a CPU stopped taking timer interrupts for 5 s");
    kprintf("  watchdog: this CPU stopped taking timer interrupts for 5 s\n");
    kprintf("  (interrupts disabled too long, or spinning in a loop with IF=0)\n\n");
    dump_frame(f, cr2);
    report_message("watchdog: a CPU stopped taking timer interrupts for 5 s");
    panic_cause_watchdog(&panic_report.cause, f->rip);
    panic_end();
}

_Noreturn void panic_trap(const struct trap_frame *f)
{
    uint64_t cr2 = read_cr2();
    panic_begin();
    dump_frame(f, cr2);
    panic_end();
}

static void dump_frame(const struct trap_frame *f, uint64_t cr2)
{
    const char *name = f->vector < 32 ? exception_names[f->vector] : "interrupt";
    uint64_t off;
    const char *sym = ksym_lookup(f->rip, &off);
    char msg[KEXEC_MESSAGE_LINE];
    ksnprintf(msg, sizeof(msg), "%s (vector %lu, error %lx) at %s+0x%lx", name, f->vector,
              f->error, sym ? sym : "?", sym ? off : f->rip);
    kexec_panic_message(msg);
    report_message(msg);
    panic_report.at = f->rip;
    panic_cause_trap(&panic_report.cause, f->vector, f->rip, cr2);
    kprintf("  %s\n\n", msg);
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
}
