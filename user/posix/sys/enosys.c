/* The ENOSYS log: what a program asked of the layer that it does not
 * answer, once each, in the kernel log. A port that fails on an unknown
 * call, a prctl option or an ioctl it needs is easiest to fix when the
 * first run lists every gap; Chromium's runs are the reason it exists.
 *
 * It is off. JAMOS_ENOSYS_LOG=1 in the program's environment (anything but
 * empty or "0") turns it on, decided at the first gap the program meets.
 * A gap is one of:
 *   - a call number no handler answers (calls.c): "syscall 435 (clone3)";
 *   - an option of a call that is answered but not that option (prctl,
 *     futex, setpriority's target): "prctl option 53";
 *   - an ioctl request nothing understands (fd.c): "ioctl 0x5413";
 *   - a /proc path that is not there: "path /proc/cpuinfo".
 * Each is one line, "enosys: <program>: <what>", written with one
 * debug_write, so lines from several threads never mix. A gap is logged
 * the first time only, remembered in a table of SEEN_MAX entries (the
 * program's own); when it is full one line says so and no more are made,
 * so a program that tries ten thousand different numbers cannot fill the
 * log.
 *
 * Calls that are meant to fail are not gaps and have handlers
 * (membarrier, set_robust_list: -ENOSYS, as an old kernel answers), so
 * they are not logged.
 *
 * With the log off a gap costs one atomic load. The lock covers the table
 * and the line counters; the debug_write is made under it (a short call),
 * which keeps the lines in the order the table has them. */
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include "layer.h"

#define SEEN_MAX   256   /* distinct gaps remembered */
#define LINE_BYTES 200   /* a line, with its newline */
#define NAME_SHOWN 40    /* bytes of the program's name shown */
#define PATH_SHOWN 100   /* bytes of a path or a word shown (the whole is what is remembered) */

/* What kind of gap an entry of the table is. */
enum { GAP_CALL = 1, GAP_OPTION, GAP_IOCTL, GAP_PATH };

#define MODE_OFF 1   /* mode's values; 0: not decided yet */
#define MODE_ON  2

struct seen {
    uint8_t  kind;   /* GAP_* */
    uint64_t key;    /* the call number, the request, or a hash of the words */
};

static int mode;                /* MODE_*; atomic: read at every gap */
static lock_t log_lock;         /* guards what follows */
static struct seen seen[SEEN_MAX];
static unsigned nseen;
static bool full_said;          /* the "table full" line has been written */
static unsigned nlines;         /* lines written since the process began */
static char last_line[LINE_BYTES];

/* A line being made. */
struct line {
    char   b[LINE_BYTES];
    size_t n;
};

static void put(struct line *l, const char *s, size_t max)
{
    for (size_t i = 0; s[i] && i < max && l->n < sizeof(l->b) - 2; i++)
        l->b[l->n++] = s[i];
}

static void put_char(struct line *l, char c)
{
    if (l->n < sizeof(l->b) - 2)
        l->b[l->n++] = c;
}

static void put_dec(struct line *l, long v)
{
    char tmp[24];
    unsigned k = 0;
    unsigned long u = v < 0 ? 0ul - (unsigned long)v : (unsigned long)v;
    do {
        tmp[k++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0)
        put_char(l, '-');
    while (k)
        put_char(l, tmp[--k]);
}

static void put_hex(struct line *l, unsigned long v)
{
    char tmp[20];
    unsigned k = 0;
    do {
        tmp[k++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    put(l, "0x", 2);
    while (k)
        put_char(l, tmp[--k]);
}

/* FNV-1a, 64 bits: a path or a word as a table key. */
static uint64_t hash(const char *s)
{
    uint64_t h = 14695981039346656037ull;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 1099511628211ull;
    return h;
}

/* Is the log on? Decided once, from the environment (or by the test hook). */
static bool enabled(void)
{
    int m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
    if (!m) {
        const char *v = getenv("JAMOS_ENOSYS_LOG");
        int want = v && v[0] && strcmp(v, "0") ? MODE_ON : MODE_OFF;
        int none = 0;
        m = __atomic_compare_exchange_n(&mode, &none, want, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE) ? want : none;
    }
    return m == MODE_ON;
}

/* "enosys: <program>: " begun. */
static void begin(struct line *l)
{
    l->n = 0;
    put(l, "enosys: ", 8);
    put(l, program_invocation_short_name ? program_invocation_short_name : "?", NAME_SHOWN);
    put(l, ": ", 2);
}

/* The line ended and written; log_lock held. */
static void emit_locked(struct line *l)
{
    l->b[l->n++] = '\n';   /* put leaves room for it and the NUL */
    l->b[l->n] = '\0';
    (void)jam_debug_write(l->b, l->n);   /* the log: nothing to do if it can't */
    nlines++;
    memcpy(last_line, l->b, l->n + 1);
}

/* l, if (kind, key) has not been logged: remembered, and written. With
 * the table full: the one line that says so, then nothing. */
static void log_once(unsigned kind, uint64_t key, struct line *l)
{
    lock_take(&log_lock);
    bool known = false;
    for (unsigned i = 0; i < nseen && !known; i++)
        known = seen[i].kind == kind && seen[i].key == key;
    if (!known && nseen < SEEN_MAX) {
        seen[nseen++] = (struct seen){ (uint8_t)kind, key };
        emit_locked(l);
    } else if (!known && !full_said) {
        full_said = true;
        struct line f;
        begin(&f);
        put(&f, "more than ", 10);
        put_dec(&f, SEEN_MAX);
        put(&f, " different gaps: the rest are not logged", 40);
        emit_locked(&f);
    }
    lock_give(&log_lock);
}

void lx_enosys_call(long n)
{
    if (!enabled())
        return;
    struct line l;
    begin(&l);
    put(&l, "syscall ", 8);
    put_dec(&l, n);
    const char *name = lx_sysname(n);
    if (name) {
        put(&l, " (", 2);
        put(&l, name, 40);
        put_char(&l, ')');
    }
    log_once(GAP_CALL, (uint64_t)n, &l);
}

void lx_enosys_option(const char *what, long value)
{
    if (!enabled())
        return;
    struct line l;
    begin(&l);
    put(&l, what, PATH_SHOWN);
    put_char(&l, ' ');
    put_dec(&l, value);
    log_once(GAP_OPTION, hash(what) ^ ((uint64_t)value * 0x9e3779b97f4a7c15ull), &l);
}

/* ioctl requests a program makes on any descriptor to find out whether it
 * is a terminal: ENOTTY is their answer, and no gap. */
static bool ioctl_is_probe(unsigned long req)
{
    switch (req) {
    case 0x5401:   /* TCGETS */
    case 0x5402:   /* TCSETS */
    case 0x5403:   /* TCSETSW */
    case 0x5404:   /* TCSETSF */
    case 0x540f:   /* TIOCGPGRP */
    case 0x5413:   /* TIOCGWINSZ */
    case 0x5429:   /* TIOCGSID */
        return true;
    }
    return false;
}

void lx_enosys_ioctl(unsigned long req)
{
    if (ioctl_is_probe(req) || !enabled())
        return;
    struct line l;
    begin(&l);
    put(&l, "ioctl ", 6);
    put_hex(&l, req);
    log_once(GAP_IOCTL, req, &l);
}

void lx_enosys_path(const char *path)
{
    if (!enabled())
        return;
    struct line l;
    begin(&l);
    put(&l, "path ", 5);
    put(&l, path, PATH_SHOWN);
    log_once(GAP_PATH, hash(path), &l);
}

void lx_enosys_set(int on)
{
    lock_take(&log_lock);
    nseen = 0;
    full_said = false;
    lock_give(&log_lock);
    __atomic_store_n(&mode, on ? MODE_ON : MODE_OFF, __ATOMIC_RELEASE);
}

unsigned lx_enosys_lines(void)
{
    lock_take(&log_lock);
    unsigned n = nlines;
    lock_give(&log_lock);
    return n;
}

size_t lx_enosys_last(char *buf, size_t cap)
{
    lock_take(&log_lock);
    size_t n = strnlen(last_line, sizeof(last_line));
    if (cap) {
        size_t k = n < cap - 1 ? n : cap - 1;
        memcpy(buf, last_line, k);
        buf[k] = '\0';
    }
    lock_give(&log_lock);
    return n;
}
