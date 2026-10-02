/* The RESULTS box (report.h): result lines kept as they are printed and
 * repeated in one box at the end of the boot. */
#include <stdarg.h>
#include <jam/cmdline.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/report.h>
#include <jam/spinlock.h>

#define REPORT_LINES 64
#define REPORT_WIDTH 120

static char lines[REPORT_LINES][REPORT_WIDTH];
static unsigned nlines, dropped;
static spinlock_t report_lock = SPINLOCK_INIT("report");

/* Keep buf (one line) for the box. */
static void keep(const char *buf)
{
    uint64_t f = spin_lock_irqsave(&report_lock);
    if (nlines < REPORT_LINES) {
        char *d = lines[nlines++];
        unsigned i;
        for (i = 0; buf[i] && buf[i] != '\n' && i + 1 < REPORT_WIDTH; i++)
            d[i] = buf[i];
        d[i] = '\0';
    } else {
        dropped++;
    }
    spin_unlock_irqrestore(&report_lock, f);
}

void report(const char *fmt, ...)
{
    char buf[REPORT_WIDTH];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kprintf("%s\n", buf);
    keep(buf);
}

void report_from(uint64_t writer, const char *line)
{
    char buf[REPORT_WIDTH + 1];
    int n = ksnprintf(buf, REPORT_WIDTH, "%s", line);   /* cut as report() cuts */
    size_t len = (size_t)n < REPORT_WIDTH ? (size_t)n : REPORT_WIDTH - 1;
    buf[len] = '\n';
    klog_write_from(writer, buf, len + 1);
    keep(buf);   /* (keep stops at the newline) */
}

void report_print(const char *version)
{
    kprintf("\n==================== RESULTS (read these lines out) ====================\n");
    kprintf("  Jam OS %s, cmdline \"%s\"\n", version, cmdline_get());
    for (unsigned i = 0; i < nlines; i++)
        kprintf("  %s\n", lines[i]);
    if (dropped)
        kprintf("  (%u more result lines not kept)\n", dropped);
    kprintf("=========================================================================\n");
}
