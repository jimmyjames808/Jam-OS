/* The RESULTS box (report.h): result lines kept as they are printed and
 * repeated in one box at the end of the boot.
 *
 * 256 lines in all (the real PC's drivers and tests report well past the 64
 * the box once held): the first REPORT_HEAD lines are kept as they come, and
 * after that only the newest REPORT_TAIL, in a ring. So the start of the boot
 * (the CPU and device lines) and the end (every test's summary, init's
 * "exited with code" lines, the verdict) both survive a boot that reports
 * too much; the lines between are the ones lost, and the box says how many. */
#include <stdarg.h>
#include <jam/cmdline.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/report.h>
#include <jam/spinlock.h>

#define REPORT_HEAD  192
#define REPORT_TAIL  64
#define REPORT_WIDTH 120

/* One box: the real one below, or a private one a ktest fills. */
struct report_box {
    char     head[REPORT_HEAD][REPORT_WIDTH];
    char     tail[REPORT_TAIL][REPORT_WIDTH];   /* a ring: line n is in slot (n - HEAD) % TAIL */
    unsigned total;                             /* lines kept so far */
};

static struct report_box box;
static unsigned refused;   /* lines a program reported past its cap (report_refused) */
static spinlock_t report_lock = SPINLOCK_INIT("report");

/* Keep buf (one line, cut at its newline or the width) in b. The caller
 * serialises. */
static void box_keep(struct report_box *b, const char *buf)
{
    char *d = b->total < REPORT_HEAD ? b->head[b->total]
                                     : b->tail[(b->total - REPORT_HEAD) % REPORT_TAIL];
    unsigned i;
    for (i = 0; buf[i] && buf[i] != '\n' && i + 1 < REPORT_WIDTH; i++)
        d[i] = buf[i];
    d[i] = '\0';
    b->total++;
}

/* How many lines of b are in the tail, and how many fell between. */
static unsigned box_tail(const struct report_box *b, unsigned *lost)
{
    unsigned after_head = b->total > REPORT_HEAD ? b->total - REPORT_HEAD : 0;
    unsigned ntail = after_head < REPORT_TAIL ? after_head : REPORT_TAIL;
    *lost = after_head - ntail;
    return ntail;
}

unsigned report_box_shown(const struct report_box *b)
{
    unsigned lost;
    unsigned ntail = box_tail(b, &lost);
    unsigned nhead = b->total < REPORT_HEAD ? b->total : REPORT_HEAD;
    return nhead + (lost ? 1 : 0) + ntail;
}

void report_box_line(const struct report_box *b, unsigned i, char *out, size_t cap)
{
    unsigned lost;
    unsigned ntail = box_tail(b, &lost);
    unsigned nhead = b->total < REPORT_HEAD ? b->total : REPORT_HEAD;
    const char *s = "";
    if (i < nhead) {
        s = b->head[i];
    } else if (lost && i == nhead) {
        ksnprintf(out, cap, "(%u result line%s didn't fit: the lines between the first %u and "
                  "the last %u are not shown)", lost, lost == 1 ? "" : "s", nhead, ntail);
        return;
    } else if (i - nhead - (lost ? 1 : 0) < ntail) {
        unsigned n = b->total - ntail + (i - nhead - (lost ? 1 : 0));
        s = b->tail[(n - REPORT_HEAD) % REPORT_TAIL];
    }
    ksnprintf(out, cap, "%s", s);
}

struct report_box *report_box_new(void)
{
    return kzalloc(sizeof(struct report_box));
}

void report_box_free(struct report_box *b)
{
    kfree(b);
}

void report_box_keep(struct report_box *b, const char *line)
{
    box_keep(b, line);
}

/* Keep buf (one line) for the box. */
static void keep(const char *buf)
{
    uint64_t f = spin_lock_irqsave(&report_lock);
    box_keep(&box, buf);
    spin_unlock_irqrestore(&report_lock, f);
}

void report_refused(void)
{
    __atomic_add_fetch(&refused, 1, __ATOMIC_RELAXED);
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
    /* Read without the lock (kprintf can take long): the run is over by now, and a
     * straggler could at worst tear one line. */
    char line[REPORT_WIDTH + 80];
    unsigned n = report_box_shown(&box);
    for (unsigned i = 0; i < n; i++) {
        report_box_line(&box, i, line, sizeof(line));
        kprintf("  %s\n", line);
    }
    unsigned over = __atomic_load_n(&refused, __ATOMIC_RELAXED);
    if (over)
        kprintf("  (%u result lines from programs were over their cap: in the log only)\n",
                over);
    kprintf("=========================================================================\n");
}
