/* libos: a terminal's line discipline (<ldisc.h> says what it does). Pure:
 * no system calls, no allocation; one struct ldisc per terminal.
 *
 * The ready queue is a byte ring (ready[], rhead, rlen) with a ring of
 * record lengths beside it (recs[], chead, ncount): record i's bytes
 * follow record i - 1's. Only the last record can grow (open), and only
 * with raw input; ld_peek closes it, so a record a reader was sent never
 * changes afterwards. */
#include <ldisc.h>
#include <os.h>

#define ECHO_CHUNK 128u   /* echo bytes processed at a time */
#define TIME_NS    100000000ull   /* VTIME's unit: a tenth of a second */

void ld_default(struct ld_termios *t)
{
    memset(t, 0, sizeof(*t));
    t->iflag = LD_ICRNL | LD_IUTF8;
    t->oflag = LD_OPOST | LD_ONLCR;
    t->cflag = LD_CFLAG_DEFAULT;
    t->lflag = LD_ISIG | LD_ICANON | LD_ECHO | LD_ECHOE | LD_ECHOK | LD_ECHOCTL | LD_ECHOKE |
               LD_IEXTEN;
    t->cc[LD_VINTR] = 0x03;
    t->cc[LD_VQUIT] = 0x1c;
    t->cc[LD_VERASE] = 0x7f;
    t->cc[LD_VKILL] = 0x15;
    t->cc[LD_VEOF] = 0x04;
    t->cc[LD_VMIN] = 1;
    t->cc[LD_VSTART] = 0x11;
    t->cc[LD_VSTOP] = 0x13;
    t->cc[LD_VSUSP] = 0x1a;
    t->cc[LD_VREPRINT] = 0x12;
    t->cc[LD_VDISCARD] = 0x0f;
    t->cc[LD_VWERASE] = 0x17;
    t->cc[LD_VLNEXT] = 0x16;
    t->ispeed = t->ospeed = LD_CFLAG_DEFAULT & 017;   /* B38400 */
}

void ld_init(struct ldisc *l, ld_echo_fn *echo, void *ctx)
{
    memset(l, 0, sizeof(*l));
    ld_default(&l->t);
    l->echo = echo;
    l->echo_ctx = ctx;
}

/* ---- the ready queue --------------------------------------------------------------- */

/* A new record of n bytes at the back (closed, or open for raw input to
 * join): false if it doesn't fit. */
static bool rec_push(struct ldisc *l, const uint8_t *p, size_t n, bool open)
{
    if (l->ncount == LD_RECS_MAX || n > LD_READY_MAX - l->rlen)
        return false;
    for (size_t i = 0; i < n; i++)
        l->ready[(l->rhead + l->rlen + i) % LD_READY_MAX] = p[i];
    l->rlen += (uint32_t)n;
    l->recs[(l->chead + l->ncount++) % LD_RECS_MAX] = (uint16_t)n;
    l->open = open;
    return true;
}

/* A raw byte: onto the open record while it has room, else a new one. */
static void rec_raw(struct ldisc *l, uint8_t b)
{
    uint16_t *last = &l->recs[(l->chead + l->ncount - 1) % LD_RECS_MAX];
    if (!l->ncount || !l->open || *last >= LD_REC_MAX) {
        (void)rec_push(l, &b, 1, true);   /* full: dropped */
        return;
    }
    if (l->rlen == LD_READY_MAX)
        return;
    l->ready[(l->rhead + l->rlen++) % LD_READY_MAX] = b;
    (*last)++;
}

uint32_t ld_ready(const struct ldisc *l)
{
    return l->ncount;
}

long ld_peek(struct ldisc *l, uint8_t *buf, size_t cap)
{
    if (!l->ncount)
        return -1;
    if (l->ncount == 1)
        l->open = false;
    size_t n = l->recs[l->chead];
    for (size_t i = 0; i < n && i < cap; i++)
        buf[i] = l->ready[(l->rhead + i) % LD_READY_MAX];
    return (long)(n < cap ? n : cap);
}

void ld_pop(struct ldisc *l)
{
    if (!l->ncount)
        return;
    uint32_t n = l->recs[l->chead];
    l->rhead = (l->rhead + n) % LD_READY_MAX;
    l->rlen -= n;
    l->chead = (l->chead + 1) % LD_RECS_MAX;
    if (!--l->ncount)
        l->open = false;
}

bool ld_unread(struct ldisc *l, const uint8_t *p, size_t n)
{
    if (l->ncount == LD_RECS_MAX || n > LD_READY_MAX - l->rlen || n > LD_REC_MAX)
        return false;
    if (!l->ncount)
        l->open = false;   /* the record put back is whole */
    l->rhead = (uint32_t)((l->rhead + LD_READY_MAX - n) % LD_READY_MAX);
    for (size_t i = 0; i < n; i++)
        l->ready[(l->rhead + i) % LD_READY_MAX] = p[i];
    l->rlen += (uint32_t)n;
    l->chead = (l->chead + LD_RECS_MAX - 1) % LD_RECS_MAX;
    l->recs[l->chead] = (uint16_t)n;
    l->ncount++;
    return true;
}

size_t ld_take_line(struct ldisc *l, uint8_t *buf, size_t cap)
{
    size_t n = l->nline < cap ? l->nline : cap;
    memcpy(buf, l->line, n);
    l->nline = 0;
    return n;
}

void ld_flush_input(struct ldisc *l)
{
    l->nline = 0;
    l->lnext = false;
    l->rhead = l->rlen = 0;
    l->chead = l->ncount = 0;
    l->open = false;
}

/* ---- output ------------------------------------------------------------------------- */

/* How byte c moves the column (c already output-processed). */
static void move_col(struct ldisc *l, uint8_t c)
{
    if (c == '\r')
        l->col = 0;
    else if (c == '\b')
        l->col -= l->col > 0;
    else if (c == '\t')
        l->col = (l->col | 7) + 1;
    else if (c >= 0x20 && c != 0x7f && (c & 0xc0) != 0x80)
        l->col++;   /* a character, or a UTF-8 sequence's first byte */
}

size_t ld_output(struct ldisc *l, const uint8_t *in, size_t n, uint8_t *out)
{
    bool post = l->t.oflag & LD_OPOST;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        if (post && c == '\n' && (l->t.oflag & LD_ONLCR)) {
            out[k++] = '\r';
            move_col(l, '\r');
        } else if (post && c == '\r' && (l->t.oflag & LD_OCRNL)) {
            c = '\n';
        }
        out[k++] = c;
        move_col(l, c);
    }
    return k;
}

/* Echo n bytes (through the output processing). */
static void echo(struct ldisc *l, const uint8_t *p, size_t n)
{
    uint8_t buf[2 * ECHO_CHUNK];
    for (size_t at = 0; at < n; at += ECHO_CHUNK) {
        size_t k = n - at < ECHO_CHUNK ? n - at : ECHO_CHUNK;
        size_t m = ld_output(l, p + at, k, buf);
        if (l->echo)
            l->echo(l->echo_ctx, buf, m);
    }
}

static void echo_str(struct ldisc *l, const char *s)
{
    echo(l, (const uint8_t *)s, strlen(s));
}

/* Is c a control character ECHOCTL shows as ^X? (Tab and NL show as
 * themselves, START and STOP as nothing special: Linux's rule.) */
static bool shown_ctl(const struct ldisc *l, uint8_t c)
{
    return (l->t.lflag & LD_ECHOCTL) && (c < 0x20 || c == 0x7f) && c != '\t' && c != '\n' &&
           c != l->t.cc[LD_VSTART] && c != l->t.cc[LD_VSTOP];
}

/* Echo one input byte as the terminal shows it (^C for a control one). */
static void echo_char(struct ldisc *l, uint8_t c)
{
    if (shown_ctl(l, c)) {
        uint8_t two[2] = { '^', (uint8_t)(c ^ 0x40) };
        echo(l, two, 2);
        return;
    }
    echo(l, &c, 1);
}

/* ---- line editing -------------------------------------------------------------------- */

static bool is_cc(const struct ldisc *l, uint8_t c, unsigned idx)
{
    return l->t.cc[idx] && c == l->t.cc[idx];   /* 0 is _POSIX_VDISABLE */
}

/* The column after the line's first n bytes, from where it started. */
static uint32_t line_col(const struct ldisc *l, uint32_t n)
{
    uint32_t col = l->canon_col;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = l->line[i];
        if (c == '\t')
            col = (col | 7) + 1;
        else if (c < 0x20 || c == 0x7f)
            col += shown_ctl(l, c) ? 2 : 0;
        else if ((c & 0xc0) != 0x80)
            col++;
    }
    return col;
}

/* Erase the line's last character (a whole UTF-8 one under IUTF8), and its
 * echo: false if the line was empty. */
static bool erase_one(struct ldisc *l)
{
    if (!l->nline)
        return false;
    uint32_t start = l->nline - 1;
    if (l->t.iflag & LD_IUTF8)
        while (start > 0 && (l->line[start] & 0xc0) == 0x80)
            start--;
    uint32_t before = line_col(l, start), after = line_col(l, l->nline);
    l->nline = start;
    if (!(l->t.lflag & LD_ECHO))
        return true;
    if (!(l->t.lflag & LD_ECHOE)) {
        echo_char(l, l->t.cc[LD_VERASE]);
        return true;
    }
    if (l->line[start] == '\t') {   /* the columns the tab took */
        for (uint32_t i = before; i < after; i++)
            echo_str(l, "\b");
        return true;
    }
    for (uint32_t i = before; i < after; i++)
        echo_str(l, "\b \b");
    return true;
}

static bool is_space(uint8_t c)
{
    return c == ' ' || c == '\t';
}

/* VWERASE: the spaces before the cursor, then the word before them. */
static void erase_word(struct ldisc *l)
{
    while (l->nline && is_space(l->line[l->nline - 1]))
        erase_one(l);
    while (l->nline && !is_space(l->line[l->nline - 1]))
        erase_one(l);
}

static void kill_line(struct ldisc *l)
{
    uint32_t lf = l->t.lflag;
    if ((lf & LD_ECHO) && (lf & LD_ECHOKE) && (lf & LD_ECHOE)) {
        while (erase_one(l))
            ;
        return;
    }
    l->nline = 0;
    if (!(lf & LD_ECHO))
        return;
    echo_char(l, l->t.cc[LD_VKILL]);
    if (lf & LD_ECHOK)
        echo_str(l, "\n");
}

static void reprint(struct ldisc *l)
{
    if (!(l->t.lflag & LD_ECHO))
        return;
    echo_char(l, l->t.cc[LD_VREPRINT]);
    echo_str(l, "\n");
    l->canon_col = l->col;
    for (uint32_t i = 0; i < l->nline; i++)
        echo_char(l, l->line[i]);
}

/* The line is done (a delimiter typed, or VEOF): a record for the reader. */
static void line_done(struct ldisc *l)
{
    (void)rec_push(l, l->line, l->nline, false);   /* full: the line is lost */
    l->nline = 0;
}

/* A character into the line being edited (room for one delimiter is kept
 * past the ordinary characters). */
static void line_add(struct ldisc *l, uint8_t c, bool delimiter)
{
    if (l->nline >= LD_MAX_CANON - (delimiter ? 0 : 1))
        return;   /* full: dropped, not echoed */
    if (!l->nline)
        l->canon_col = l->col;
    l->line[l->nline++] = c;
    bool nl_echo = c == '\n' && (l->t.lflag & LD_ECHONL);
    if ((l->t.lflag & LD_ECHO) || nl_echo)
        echo_char(l, c);
}

/* A byte in canonical mode, past the signals and VLNEXT. */
static void canonical(struct ldisc *l, uint8_t c)
{
    if (is_cc(l, c, LD_VERASE)) {
        erase_one(l);
    } else if (is_cc(l, c, LD_VKILL)) {
        kill_line(l);
    } else if ((l->t.lflag & LD_IEXTEN) && is_cc(l, c, LD_VWERASE)) {
        erase_word(l);
    } else if ((l->t.lflag & LD_IEXTEN) && is_cc(l, c, LD_VREPRINT)) {
        reprint(l);
    } else if (is_cc(l, c, LD_VEOF)) {
        line_done(l);   /* the line so far without a delimiter; empty: end of file */
    } else if (c == '\n' || is_cc(l, c, LD_VEOL) || is_cc(l, c, LD_VEOL2)) {
        line_add(l, c, true);
        line_done(l);
    } else {
        line_add(l, c, false);
    }
}

/* CR and NL as the input flags map them; false: the byte is dropped. */
static bool map_input(const struct ldisc *l, uint8_t *c)
{
    uint32_t in = l->t.iflag;
    if (in & LD_ISTRIP)
        *c &= 0x7f;
    if (*c == '\r') {
        if (in & LD_IGNCR)
            return false;
        if (in & LD_ICRNL)
            *c = '\n';
    } else if (*c == '\n' && (in & LD_INLCR)) {
        *c = '\r';
    }
    return true;
}

/* ISIG's keys: which, if c is one. */
static enum ld_sig signal_of(const struct ldisc *l, uint8_t c)
{
    if (!(l->t.lflag & LD_ISIG))
        return LD_SIG_NONE;
    if (is_cc(l, c, LD_VINTR))
        return LD_SIG_INT;
    if (is_cc(l, c, LD_VQUIT))
        return LD_SIG_QUIT;
    if (is_cc(l, c, LD_VSUSP))
        return LD_SIG_SUSP;
    return LD_SIG_NONE;
}

/* A byte that is input as it is (raw mode, or after VLNEXT). */
static void literal(struct ldisc *l, uint8_t c)
{
    if (l->t.lflag & LD_ICANON) {
        line_add(l, c, false);
        return;
    }
    rec_raw(l, c);
    if (l->t.lflag & LD_ECHO)
        echo_char(l, c);
}

enum ld_sig ld_input(struct ldisc *l, uint8_t b)
{
    uint8_t c = b;
    if (l->lnext) {
        l->lnext = false;
        literal(l, c);   /* its echo goes over the "^" shown for VLNEXT */
        return LD_SIG_NONE;
    }
    if (!map_input(l, &c))
        return LD_SIG_NONE;
    enum ld_sig sig = signal_of(l, c);
    if (sig == LD_SIG_SUSP)
        return sig;   /* job control is not built: the key does nothing */
    if (sig != LD_SIG_NONE) {
        if (!(l->t.lflag & LD_NOFLSH))
            ld_flush_input(l);
        if (l->t.lflag & LD_ECHO)
            echo_char(l, c);
        return sig;
    }
    if ((l->t.lflag & LD_IEXTEN) && is_cc(l, c, LD_VLNEXT)) {
        l->lnext = true;
        if ((l->t.lflag & LD_ECHO) && (l->t.lflag & LD_ECHOCTL))
            echo_str(l, "^\b");
        return LD_SIG_NONE;
    }
    if (l->t.lflag & LD_ICANON)
        canonical(l, c);
    else
        literal(l, c);
    return LD_SIG_NONE;
}

void ld_set(struct ldisc *l, const struct ld_termios *t)
{
    bool was_canon = l->t.lflag & LD_ICANON;
    l->t = *t;
    if (was_canon && !(t->lflag & LD_ICANON) && l->nline) {
        for (uint32_t i = 0; i < l->nline; i++)
            rec_raw(l, l->line[i]);
        l->nline = 0;
    }
}

/* ---- reads --------------------------------------------------------------------------- */

uint64_t ld_read_deadline(const struct ld_termios *t, size_t got, size_t want, uint64_t start,
                          uint64_t last)
{
    if (t->lflag & LD_ICANON)
        return got ? 0 : LD_NEVER;
    size_t min = t->cc[LD_VMIN] < want ? t->cc[LD_VMIN] : want;
    uint64_t time = t->cc[LD_VTIME] * TIME_NS;
    if (!t->cc[LD_VMIN] && !time)
        return 0;                                  /* a poll: what there is */
    if (!t->cc[LD_VMIN])
        return got ? 0 : start + time;             /* a timed read of at least 1 */
    if (got >= min)
        return 0;
    if (!time || !got)
        return LD_NEVER;                           /* MIN bytes; TIME starts at the first */
    return last + time;                            /* the gap after a byte */
}
