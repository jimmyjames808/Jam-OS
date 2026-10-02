/* The kernel log. Every line gets an uptime stamp and goes, under one
 * lock, to the ring (KLOG_SIZE bytes), COM1 and the framebuffer console, so
 * lines from different CPUs never interleave. The ring is what the panic
 * screen shows (klog_tail) and what the console process follows through the
 * klog read syscall (klog_read_at).
 *
 * What the log holds is text, whoever wrote it: printable ASCII, tabs,
 * newlines and well-formed UTF-8 (a song's "JAŸ-Z") go in as they are;
 * every other control character (escape sequences, C1's CSI) and each bad
 * piece of ill-formed UTF-8 (<jam/utf8.h>) goes in as one '?'. So a
 * process name or a program's line can't move a terminal's cursor, and
 * readers (the console, logd's files, dmesg) can trust what they read.
 *
 * Who wrote each line: a line a process starts (its debug_write and
 * debug_report lines) gets a mark with the process's koid in a second ring,
 * `marks`, under the same lock; the kernel's own lines get none, so a ktest
 * flooding the log doesn't push the processes' marks out. `known_from` is
 * where the marks are complete: a line at or past it with no mark is the
 * kernel's, an older one's writer is not known any more. A writer that
 * isn't the one whose line is unfinished starts a line of its own. A
 * line's text can say anything ("[init] ...", a kernel-looking "user:
 * process ..."): its mark is what a reader trusts (klog_lines). */
#include <stdint.h>
#include <jam/fbcon.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/serial.h>
#include <jam/spinlock.h>
#include <jam/time.h>
#include <jam/utf8.h>

/* KLOG_SIZE (klog.h): a power of two */

/* Page-aligned: the next kernel is handed exactly its pages (kexec). */
static char ring[KLOG_SIZE] __attribute__((aligned(4096)));
static uint64_t head;           /* total bytes ever written; ring_lock (klog_head reads it
                                   without, so it is stored atomically) */
static spinlock_t ring_lock = SPINLOCK_INIT("klog");
static bool at_line_start = true;
static uint64_t line_writer;    /* the writer of the unfinished line; ring_lock */

/* KLOG_MARKS (klog.h): a power of two. Mark i of all ever made is at
 * i % KLOG_MARKS; positions only grow, so the kept ones are sorted. */
static struct klog_line marks[KLOG_MARKS];
static uint64_t nmarks;         /* marks ever made; ring_lock */
static uint64_t known_from;     /* every process line from here on has its mark; ring_lock */

/* One lock across ring, serial and console so lines from different CPUs
 * never interleave. */
static void emit(const char *s, size_t len)
{
    uint64_t h = head;
    for (size_t i = 0; i < len; i++)
        ring[h++ & (KLOG_SIZE - 1)] = s[i];
    __atomic_store_n(&head, h, __ATOMIC_RELAXED);
    serial_write(s, len);
    fbcon_write(s, len);
}

/* s[0..len) as the log keeps it (the top of the file): the clean runs as
 * they are, a '?' for each control character or bad piece between them. */
static void emit_clean(const char *s, size_t len)
{
    const uint8_t *u = (const uint8_t *)s;
    size_t run = 0, i = 0;
    while (i < len) {
        if ((u[i] >= 0x20 && u[i] < 0x7f) || u[i] == '\n' || u[i] == '\t') {
            i++;
            continue;
        }
        uint32_t cp = 0;
        int k = u[i] < 0x80 ? -1 : utf8_seq(u + i, len - i, &cp);
        if (k > 0 && !utf8_is_control(cp)) {
            i += (size_t)k;
            continue;
        }
        emit(s + run, i - run);
        emit("?", 1);
        i += (size_t)(k > 0 ? k : -k);
        run = i;
    }
    emit(s + run, len - run);
}

/* ring_lock held: a line starts at the head, written by `writer`. */
static void mark_locked(uint64_t writer)
{
    line_writer = writer;
    if (writer == KLOG_WRITER_KERNEL)
        return;   /* no mark: the kernel's */
    struct klog_line *m = &marks[nmarks & (KLOG_MARKS - 1)];
    if (nmarks >= KLOG_MARKS)
        known_from = m->pos + 1;   /* that line's mark goes now */
    *m = (struct klog_line){ .pos = head, .writer = writer };
    nmarks++;
}

void klog_write(const char *s, size_t len)
{
    klog_write_from(KLOG_WRITER_KERNEL, s, len);
}

/* Every line starts with seconds since the TSC was calibrated. Interrupt
 * handlers may log: the lock is always taken with interrupts off. */
void klog_write_from(uint64_t writer, const char *s, size_t len)
{
    uint64_t f = spin_lock_irqsave(&ring_lock);
    if (len && !at_line_start && writer != line_writer) {
        emit("\n", 1);   /* someone else's unfinished line: ours is a line of its own */
        at_line_start = true;
    }
    while (len) {
        if (at_line_start) {
            mark_locked(writer);
            char stamp[24];
            uint64_t ns = tsc_hz ? uptime_ns() : 0;
            int n = ksnprintf(stamp, sizeof(stamp), "[%5lu.%06lu] ", ns / 1000000000,
                              (ns / 1000) % 1000000);
            emit(stamp, (size_t)n);
            at_line_start = false;
        }
        size_t chunk = 0;
        while (chunk < len && s[chunk] != '\n')
            chunk++;
        if (chunk < len) {
            chunk++;   /* include the newline */
            at_line_start = true;
        }
        emit_clean(s, chunk);
        s += chunk;
        len -= chunk;
    }
    spin_unlock_irqrestore(&ring_lock, f);
}

void klog_write_raw(const char *s, size_t len)
{
    uint64_t f = spin_lock_irqsave(&ring_lock);
    /* A copy of lines whose writers aren't known any more: each its mark. */
    for (size_t i = 0; i < len;) {
        if (at_line_start)
            mark_locked(KLOG_WRITER_UNKNOWN);
        size_t chunk = 0;
        while (i + chunk < len && s[i + chunk] != '\n')
            chunk++;
        at_line_start = i + chunk < len;
        if (at_line_start)
            chunk++;
        emit_clean(s + i, chunk);
        i += chunk;
    }
    spin_unlock_irqrestore(&ring_lock, f);
}

void klog_force_unlock(void)
{
    spin_force_unlock(&ring_lock);
}

size_t klog_tail(char *buf, size_t size)
{
    uint64_t f = spin_lock_irqsave(&ring_lock);
    uint64_t avail = head < KLOG_SIZE ? head : KLOG_SIZE;
    uint64_t n = avail < size ? avail : size;
    for (uint64_t i = 0; i < n; i++)
        buf[i] = ring[(head - n + i) & (KLOG_SIZE - 1)];
    spin_unlock_irqrestore(&ring_lock, f);
    return (size_t)n;
}

/* ---- readers ------------------------------------------------------------------ */

uint64_t klog_head(void)
{
    return __atomic_load_n(&head, __ATOMIC_RELAXED);
}

const char *klog_ring(void)
{
    return ring;
}

size_t klog_ring_copy(const char *r, uint64_t size, uint64_t h, uint64_t pos, char *buf,
                      size_t cap, uint64_t *first)
{
    uint64_t oldest = h > size ? h - size : 0;
    if (pos < oldest)
        pos = oldest;
    if (pos > h)
        pos = h;
    uint64_t n = h - pos < cap ? h - pos : cap;
    for (uint64_t i = 0; i < n; i++)
        buf[i] = r[(pos + i) & (size - 1)];
    *first = pos;
    return (size_t)n;
}

size_t klog_read_kept(uint64_t pos, uint64_t keep, char *buf, size_t cap, uint64_t *first)
{
    uint64_t f = spin_lock_irqsave(&ring_lock);
    if (keep < KLOG_SIZE && head > keep && pos < head - keep)
        pos = head - keep;
    size_t n = klog_ring_copy(ring, KLOG_SIZE, head, pos, buf, cap, first);
    spin_unlock_irqrestore(&ring_lock, f);
    return n;
}

size_t klog_read_at(uint64_t pos, char *buf, size_t cap, uint64_t *first)
{
    return klog_read_kept(pos, KLOG_SIZE, buf, cap, first);
}

size_t klog_lines(uint64_t pos, struct klog_line *out, size_t cap, uint64_t *known)
{
    uint64_t f = spin_lock_irqsave(&ring_lock);
    /* The first kept mark at or past pos: a binary search over [lo, nmarks). */
    uint64_t lo = nmarks > KLOG_MARKS ? nmarks - KLOG_MARKS : 0, hi = nmarks;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (marks[mid & (KLOG_MARKS - 1)].pos < pos)
            lo = mid + 1;
        else
            hi = mid;
    }
    size_t n = 0;
    for (; lo < nmarks && n < cap; lo++)
        out[n++] = marks[lo & (KLOG_MARKS - 1)];
    *known = known_from;
    spin_unlock_irqrestore(&ring_lock, f);
    return n;
}
