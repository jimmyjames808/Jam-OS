/* Panic screen. M0 prints the message, a raw frame-pointer backtrace and the
 * last lines of the kernel log. M1 adds registers and symbol names, and M3
 * halts the other CPUs with an IPI first. */
#include <stdarg.h>
#include <stdint.h>
#include <jam/fbcon.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/panic.h>
#include <jam/string.h>
#include <jam/x86.h>

#define MAX_FRAMES   24
#define TAIL_BYTES   2048
#define KERNEL_SPACE 0xffff800000000000ull

static volatile int panicking;

_Noreturn void halt_forever(void)
{
    for (;;) {
        cli();
        hlt();
    }
}

static void backtrace(void)
{
    uint64_t *rbp = __builtin_frame_address(0);
    kprintf("backtrace:\n");
    for (int i = 0; i < MAX_FRAMES; i++) {
        if ((uint64_t)rbp < KERNEL_SPACE || ((uint64_t)rbp & 7))
            break;
        uint64_t ret = rbp[1];
        if (!ret)
            break;
        kprintf("  #%-2d %016lx\n", i, ret);
        uint64_t *next = (uint64_t *)rbp[0];
        if (next <= rbp)   /* stacks grow down, so callers are higher up */
            break;
        rbp = next;
    }
}

_Noreturn void panic(const char *fmt, ...)
{
    cli();
    if (__atomic_exchange_n(&panicking, 1, __ATOMIC_SEQ_CST)) {
        halt_forever();    /* panic inside panic: stop, don't recurse */
    }

    static char tail[TAIL_BYTES + 1];
    size_t n = klog_tail(tail, TAIL_BYTES);
    tail[n] = '\0';

    fbcon_force_unlock();
    fbcon_set_colors(0xffffff, 0x8b0000);
    fbcon_clear();

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    kprintf("\n  *** JAM OS KERNEL PANIC ***\n\n  %s\n\n", msg);
    backtrace();

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
