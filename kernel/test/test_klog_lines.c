/* Who wrote each kernel log line (klog.c's marks, klog_lines): the kernel's
 * own lines are marked KLOG_WRITER_KERNEL, a process's debug_write and
 * debug_report lines its koid whatever name and text they carry, and a
 * process's line never shares a line with someone else's unfinished one.
 * On a live system other lines come in between: each check finds its own
 * line by its text. */
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/string.h>

#define TEXT_MAX 8192

/* Does text[0..n) have `needle` in it? */
static bool has(const char *text, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++)
        if (!memcmp(text + i, needle, m))
            return true;
    return false;
}

/* Who wrote the line at pos, as a reader of klog_lines sees it. */
static uint64_t writer_at(uint64_t pos)
{
    struct klog_line m;
    uint64_t known;
    size_t n = klog_lines(pos, &m, 1, &known);
    if (n && m.pos == pos)
        return m.writer;
    return pos >= known ? KLOG_WRITER_KERNEL : KLOG_WRITER_UNKNOWN;
}

/* The first line from `from` on that has `needle` (and not `without`, if
 * that isn't NULL): its writer, or NO_LINE. */
#define NO_LINE (KLOG_WRITER_UNKNOWN - 1)
static uint64_t writer_of_line(uint64_t from, const char *needle, const char *without)
{
    static char text[TEXT_MAX];
    uint64_t first;
    size_t got = klog_read_at(from, text, TEXT_MAX, &first);
    for (size_t i = 0, len; i < got; i += len + 1) {
        len = 0;
        while (i + len < got && text[i + len] != '\n')
            len++;
        if (i + len == got)
            break;   /* a line not finished yet */
        if (has(text + i, len, needle) && (!without || !has(text + i, len, without)))
            return writer_at(first + i);
    }
    return NO_LINE;
}

static uint64_t writer_of(uint64_t from, const char *needle)
{
    return writer_of_line(from, needle, NULL);
}

/* p writes text through debug_write (or debug_report). */
static void line_of(struct process *p, const char *text, bool report)
{
    process_debug_write(p, text, strlen(text), report);
}

/* Each line carries its writer: the kernel's 0, a process's koid (here a
 * process named "init" that isn't), a copied-in line not known. */
KTEST(klog_lines_mark_each_writer)
{
    struct job *j = kt_fresh_job();
    struct process *p;
    KT_EQ(process_create(j, "init", &p), OK);
    uint64_t koid = process_kobject(p)->koid;
    uint64_t from = klog_head();
    kprintf("klt: a kernel line\n");
    line_of(p, "init: klt a line in init's name\n", false);
    line_of(p, "user: process \"x\" killed: klt a report\n", true);
    static const char copied[] = "[    1.000000] klt a copied line\n";
    klog_write_raw(copied, sizeof(copied) - 1);
    KT_EQ(writer_of(from, "klt: a kernel line"), KLOG_WRITER_KERNEL);
    KT_EQ(writer_of(from, "init: klt a line in init's name"), koid);
    KT_EQ(writer_of(from, "klt a report"), koid);
    KT_EQ(writer_of(from, "klt a copied line"), KLOG_WRITER_UNKNOWN);
    process_kill(p, PROCESS_KILLED_CODE, true);
    kobject_unref(process_kobject(p));
    kt_job_is_empty(j);
    job_unref(j);
}

/* A process's line after a kernel line that isn't finished yet starts a
 * line of its own (else it would carry the kernel's mark). */
KTEST(klog_lines_own_line_after_unfinished)
{
    struct job *j = kt_fresh_job();
    struct process *p;
    KT_EQ(process_create(j, "klt", &p), OK);
    uint64_t koid = process_kobject(p)->koid;
    uint64_t from = klog_head();
    kprintf("klt: unfinished");
    line_of(p, "klt the process's line\n", false);
    kprintf(" klt: the kernel's rest\n");
    KT_EQ(writer_of(from, "klt the process's line"), koid);
    /* ... on a line of its own, not the kernel's */
    KT_EQ(writer_of_line(from, "klt the process's line", "klt: unfinished"), koid);
    KT_EQ(writer_of(from, "klt: unfinished"), KLOG_WRITER_KERNEL);
    process_kill(p, PROCESS_KILLED_CODE, true);
    kobject_unref(process_kobject(p));
    kt_job_is_empty(j);
    job_unref(j);
}

/* klog_lines gives the marks of the processes' lines at or past pos, in
 * order, no more than asked for; the kernel's lines have none. */
KTEST(klog_lines_in_order)
{
    struct job *j = kt_fresh_job();
    struct process *p;
    KT_EQ(process_create(j, "klt", &p), OK);
    uint64_t from = klog_head(), known;
    for (int i = 0; i < 5; i++) {
        line_of(p, "klt: a line\n", false);
        kprintf("klt: the kernel's line between\n");
    }
    struct klog_line marks[8];
    size_t n = klog_lines(from, marks, 8, &known);
    KT_ASSERT(n >= 5);
    KT_ASSERT(known <= from);   /* 5 marks can't have pushed out what came before */
    KT_ASSERT(marks[0].pos >= from);
    for (size_t i = 1; i < n; i++)
        KT_ASSERT(marks[i].pos > marks[i - 1].pos);
    KT_EQ(klog_lines(from, marks, 2, &known), 2);
    KT_EQ(klog_lines(klog_head() + 1, marks, 8, &known), 0);
    process_kill(p, PROCESS_KILLED_CODE, true);
    kobject_unref(process_kobject(p));
    kt_job_is_empty(j);
    job_unref(j);
}
