/* console: POSIX clients, console.open_tty and the `tty` protocol
 * (console.h; abi/idl/console.idl; docs/M13-PLAN.md, "The terminal layer").
 *
 * A tty client is two channels: its `tty` channel (requests: the
 * settings, the size, drain and flush; events: the size, ISIG's signals)
 * and its data channel, a pipe both ways as a socketpair is: the console
 * writes what is typed into it, and reads and shows what the client
 * writes. One channel, so the client's end alone says readable, writable
 * and hung up (poll waits on one handle). The terminal has one line
 * discipline (<ldisc.h>, `ld`) for all of them: its settings outlast the
 * program that set them, and what was typed and not read stays with the
 * terminal.
 *
 * Input. While a tty client has the focus (keys.c: the newest open_keys or
 * open_tty), what is typed goes through `ld` as bytes: a window's keys
 * through <keybytes.h>, a connection's bytes as they came (connin.c). The
 * records `ld` makes ready go to the newest tty's data channel, one
 * message each. When a tty client goes, the messages it left unread are
 * read back (through the console's own duplicate of the client's end) and
 * put back at the front of `ld`'s queue, for the next reader; when no tty
 * client is left, what `ld` holds is typed into the keys focus as keys
 * (<termkeys.h>), so a line typed ahead under a POSIX program reaches the
 * shell after it.
 *
 * Output. The client's messages (at most TTY_OUT_MAX bytes) go through
 * `ld`'s output processing (OPOST) to the back end, as a client's
 * console.write would, in turns with a budget, and not while the back end
 * is congested. drain, and set_attr after a drain, are answered later:
 * once its output has been read empty.
 *
 * Each tty has a struct client of its own (its level is its opener's), so
 * the modes its output asks for (bracketed paste, application cursor
 * keys) are its own. */
#include <keybytes.h>
#include <ldisc.h>
#include <termkeys.h>
#include "console.h"

#define TTY_WAITS     4                    /* drain and set_attr answers waiting, per tty */
#define OUT_BUDGET    32                   /* output messages one turn reads, at most */
#define OUT_BUDGET_NS (10 * NS_PER_MS)     /* ... or this long */
#define REQ_BUDGET    32                   /* requests one turn serves */
#define PUSH_RETRY_NS (5 * NS_PER_MS)      /* a data channel was full: try again after this */
#define UNREAD_MSGS   256                  /* messages read back from a client, at most */
#define DATA_RIGHTS   (RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT | RIGHT_TRANSFER | RIGHT_DUPLICATE)

/* An answer waiting for the output to be shown. */
struct tty_wait {
    struct idl_txn    txn;
    bool              set;   /* a set_attr (else a drain) */
    uint8_t           when;  /* set_attr's TTY_WHEN_* */
    struct ld_termios t;     /* set_attr's settings */
};

struct ttyc {
    bool     used;
    handle_t ch;                    /* the tty channel, our end: also its focus handle */
    handle_t data;                  /* the data channel, our end: input written, output read */
    handle_t peer;                  /* a duplicate of the client's end: input left unread */
    struct client cl;               /* its modes and level */
    bool     app_cursor;            /* DECCKM (ESC [ ? 1 h): cursor keys as ESC O x */
    uint8_t  dec[8];                /* the DEC mode sequence being scanned in its output */
    unsigned ndec;
    struct tty_wait waits[TTY_WAITS];
    unsigned nwaits;
};

static struct ttyc ttys[MAX_TTYS];
static struct ldisc ld;
static struct ttyc *echo_to;        /* whose input is being typed: the echo goes as its output */
static uint32_t out_pending;        /* bit i: ttys[i]'s output had messages left after its turn */
static uint32_t out_deferred;       /* ... or the back end was congested */
static uint64_t push_retry_at;      /* a data channel was full (0: no retry due) */
static bool refocus;                /* the focus changed: deliver at the next turn */
static uint8_t msg[LD_REC_MAX];     /* one message, in or out */
static uint8_t shown[2 * LD_REC_MAX];
static struct termkeys to_keys;     /* what ld held, typed as keys when no tty is left */
_Static_assert(MAX_TTYS <= 32, "the pending sets are 32-bit masks");
_Static_assert(TTY_OUT_MAX <= LD_REC_MAX, "an output message fits msg");

static unsigned slot_of(const struct ttyc *t)
{
    return (unsigned)(t - ttys);
}

/* ---- output ------------------------------------------------------------------------- */

/* ESC [ ? <params> h or l in t's output: the modes that are its own
 * (DECCKM; bracketed paste is the back ends' to read). */
static void scan_dec(struct ttyc *t, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b = p[i];
        if (b == 0x1b) {
            t->dec[0] = b;
            t->ndec = 1;
            continue;
        }
        if (!t->ndec)
            continue;
        bool fixed = (t->ndec == 1 && b == '[') || (t->ndec == 2 && b == '?');
        bool param = t->ndec >= 3 && ((b >= '0' && b <= '9') || b == ';');
        if ((fixed || param) && t->ndec < sizeof(t->dec)) {
            t->dec[t->ndec++] = b;
            continue;
        }
        if ((b == 'h' || b == 'l') && t->ndec == 4 && t->dec[3] == '1')
            t->app_cursor = b == 'h';
        t->ndec = 0;
    }
}

/* Bytes written to the terminal as t's (its output, or the echo of what
 * is typed into it), after the output processing. */
static void show(struct ttyc *t, const uint8_t *p, size_t n)
{
    scan_dec(t, p, n);
    back->write_raw(&t->cl, p, (uint32_t)n);
}

static void echo_cb(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx;
    if (echo_to)
        show(echo_to, p, n);
}

/* The waits of t, answered: the output so far has been shown. */
static void answer_waits(struct ttyc *t)
{
    for (unsigned i = 0; i < t->nwaits; i++) {
        struct tty_wait *w = &t->waits[i];
        if (!w->set) {
            (void)tty_reply_drain(w->txn, OK);   /* a client gone: nothing to do */
            continue;
        }
        if (w->when == TTY_WHEN_FLUSH)
            ttys_flush_in(slot_of(t));
        ld_set(&ld, &w->t);
        (void)tty_reply_set_attr(w->txn, OK);
    }
    t->nwaits = 0;
}

/* Read one message of t's output into msg: its length, -1 when none is
 * queued, -2 when it can't be read any more (closed, or a
 * message breaking its rules: the tty is hung up). */
static long out_take(struct ttyc *t)
{
    handle_t hs[CHANNEL_MAX_HANDLES];
    uint32_t n = 0, nh = 0;
    status_t st = drv_channel_read(t->data, msg, TTY_OUT_MAX, &n, hs, CHANNEL_MAX_HANDLES, &nh);
    for (uint32_t i = 0; st == OK && i < nh; i++)
        jam_handle_close(hs[i]);   /* bytes only */
    if (st == ERR_SHOULD_WAIT)
        return -1;
    if (st != OK) {
        if (st == ERR_BUFFER_TOO_SMALL)
            printf("console: a tty client wrote %u bytes in one message (most %u): hung up\n",
                   n, TTY_OUT_MAX);
        return -2;
    }
    return (long)n;
}

/* The output can't be read any more (a message past the limit): the tty
 * is hung up. */
static void out_gone(struct ttyc *t)
{
    answer_waits(t);
    ttys_hang_up(slot_of(t));
}

/* One turn of t's output: messages shown until none is left (the
 * waits are answered), the budget runs out (pending) or the back end is
 * congested (deferred). */
static void out_turn(struct ttyc *t)
{
    uint32_t bit = 1u << slot_of(t);
    out_pending &= ~bit;
    out_deferred &= ~bit;
    if (!t->used)
        return;
    uint64_t t0 = now();
    klog_event();   /* kernel lines logged before this output go above it */
    for (unsigned k = 0; k < OUT_BUDGET && now() - t0 < OUT_BUDGET_NS; k++) {
        if (back->congested && back->congested()) {
            out_deferred |= bit;
            return;
        }
        long n = out_take(t);
        if (n == -1) {
            answer_waits(t);
            return;
        }
        if (n == -2) {
            out_gone(t);
            return;
        }
        size_t m = ld_output(&ld, msg, (size_t)n, shown);
        show(t, shown, m);
    }
    out_pending |= bit;
}

/* Throw away the output t wrote and we haven't shown. */
static void out_discard(struct ttyc *t)
{
    for (long n = 0; t->used && n != -1;) {
        n = out_take(t);
        if (n == -2)
            out_gone(t);
    }
}

/* ---- input --------------------------------------------------------------------------- */

/* Throw away the input queued for t and not read (through our duplicate
 * of its end); with keep, put it back into ld's queue for the next reader
 * instead. */
static void in_drain(struct ttyc *t, bool keep)
{
    static uint16_t lens[UNREAD_MSGS];
    static uint8_t held[LD_READY_MAX];
    unsigned n = 0;
    size_t used = 0;
    for (unsigned k = 0; k < 1024 + 1; k++) {   /* a channel's queue holds 1024 */
        handle_t hs[CHANNEL_MAX_HANDLES];
        uint32_t got = 0, nh = 0;
        if (drv_channel_read(t->peer, msg, sizeof(msg), &got, hs, CHANNEL_MAX_HANDLES, &nh) != OK)
            break;   /* empty: we only ever write messages that fit msg */
        for (uint32_t i = 0; i < nh; i++)
            jam_handle_close(hs[i]);
        if (!keep || n == UNREAD_MSGS || got > sizeof(held) - used)
            continue;   /* past what ld could take back anyway: dropped */
        memcpy(held + used, msg, got);
        lens[n++] = (uint16_t)got;
        used += got;
    }
    while (n--) {   /* the last first, so the first ends up at the front */
        used -= lens[n];
        (void)ld_unread(&ld, held + used, lens[n]);   /* no room: dropped */
    }
}

void ttys_flush_in(unsigned i)
{
    ld_flush_input(&ld);
    if (i < MAX_TTYS && ttys[i].used)
        in_drain(&ttys[i], false);
}

/* Is any focus a tty? */
static bool any_tty(void)
{
    for (unsigned i = 0; i < nfocus; i++)
        if (focus_tty[i] >= 0)
            return true;
    return false;
}

/* What ld holds, typed into the keys focus as keys: no POSIX reader is
 * left to take it. The end-of-file records carry nothing. */
static void hand_to_keys(void)
{
    uint8_t line[LD_MAX_CANON];
    for (long n; (n = ld_peek(&ld, msg, sizeof(msg))) >= 0;) {
        ld_pop(&ld);
        for (long i = 0; i < n; i++) {
            struct termkey k[2];
            unsigned got = termkeys_byte(&to_keys, msg[i], k);
            for (unsigned j = 0; j < got; j++) {
                struct input_key_event ev = { k[j].usage, INPUT_KEY_DOWN, 0, k[j].cp };
                focus_key(&ev);
            }
        }
    }
    size_t n = ld_take_line(&ld, line, sizeof(line));
    for (size_t i = 0; i < n; i++) {
        struct termkey k[2];
        unsigned got = termkeys_byte(&to_keys, line[i], k);
        for (unsigned j = 0; j < got; j++) {
            struct input_key_event ev = { k[j].usage, INPUT_KEY_DOWN, 0, k[j].cp };
            focus_key(&ev);
        }
    }
}

/* ld's ready records to the newest tty's data channel, while it takes them;
 * or, with no tty left, to the keys focus. */
void ttys_deliver(void)
{
    int top = focus_top_tty();
    if (top < 0) {
        if (nfocus && !any_tty() && (ld_ready(&ld) || ld.nline))
            hand_to_keys();
        return;
    }
    if (push_retry_at && now() < push_retry_at)
        return;
    push_retry_at = 0;
    struct ttyc *t = &ttys[top];
    for (long n; (n = ld_peek(&ld, msg, sizeof(msg))) >= 0;) {
        status_t st = jam_channel_write(t->data, msg, (uint32_t)n, NULL, 0);
        if (st == ERR_SHOULD_WAIT || st == ERR_NO_MEMORY) {
            push_retry_at = now() + PUSH_RETRY_NS;   /* the reader is behind */
            return;
        }
        ld_pop(&ld);   /* sent, or its reader is gone (its tty goes soon) */
    }
}

/* ISIG's key typed into tty t: the shell's keys below it get the key (it
 * signals the program it runs), or else t itself is told to raise it. */
static void interrupt(struct ttyc *t, enum ld_sig sig)
{
    if (!(ld.t.lflag & LD_NOFLSH))
        in_drain(t, false);   /* ld flushed its own */
    struct input_key_event ev = { 0x06, INPUT_KEY_DOWN, INPUT_MOD_LCTRL, 0x03 };   /* Ctrl+C */
    if (sig == LD_SIG_QUIT)
        ev = (struct input_key_event){ 0x31, INPUT_KEY_DOWN, INPUT_MOD_LCTRL, 0x1c };   /* Ctrl+\ */
    if (focus_to_shell(&ev, nfocus - 1))   /* t has the focus: it is the newest */
        return;
    (void)tty_signal(t->ch, sig == LD_SIG_QUIT ? TTY_SIG_QUIT : TTY_SIG_INT);   /* full: lost */
}

void ttys_type(unsigned i, const uint8_t *p, size_t n)
{
    if (i >= MAX_TTYS || !ttys[i].used)
        return;
    struct ttyc *t = &ttys[i];
    echo_to = t;
    for (size_t k = 0; k < n; k++) {
        enum ld_sig sig = ld_input(&ld, p[k]);
        if (sig == LD_SIG_INT || sig == LD_SIG_QUIT)
            interrupt(t, sig);
    }
    echo_to = NULL;
    ttys_deliver();
}

void ttys_key(unsigned i, const struct input_key_event *ev)
{
    if (i >= MAX_TTYS || !ttys[i].used)
        return;
    uint8_t b[KEYBYTES_MAX];
    unsigned n = keybytes(ev->usage, ev->state, ev->mods, ev->codepoint, ttys[i].app_cursor, b);
    if (n)
        ttys_type(i, b, n);
}

bool ttys_bracketed(unsigned i)
{
    return i < MAX_TTYS && ttys[i].used && ttys[i].cl.bracketed;
}

/* ---- the tty protocol ------------------------------------------------------------------ */

static void termios_of(struct ld_termios *t, uint32_t iflag, uint32_t oflag, uint32_t cflag,
                       uint32_t lflag, const uint8_t cc[32], uint32_t ispeed, uint32_t ospeed)
{
    t->iflag = iflag;
    t->oflag = oflag;
    t->cflag = cflag;
    t->lflag = lflag;
    memcpy(t->cc, cc, LD_NCCS);
    t->ispeed = ispeed;
    t->ospeed = ospeed;
}

static status_t op_get_attr(void *ctx, uint32_t *iflag, uint32_t *oflag, uint32_t *cflag,
                            uint32_t *lflag, uint8_t cc[32], uint32_t *ispeed, uint32_t *ospeed)
{
    (void)ctx;
    *iflag = ld.t.iflag;
    *oflag = ld.t.oflag;
    *cflag = ld.t.cflag;
    *lflag = ld.t.lflag;
    memcpy(cc, ld.t.cc, LD_NCCS);
    *ispeed = ld.t.ispeed;
    *ospeed = ld.t.ospeed;
    return OK;
}

/* A wait for the output to drain: answered by out_turn once it is
 * empty (it runs next turn: out_pending). */
static status_t add_wait(struct ttyc *t, const struct tty_wait *w)
{
    if (t->nwaits == TTY_WAITS)
        return ERR_NO_RESOURCES;
    t->waits[t->nwaits++] = *w;
    out_pending |= 1u << slot_of(t);
    return IDL_LATER;
}

static status_t op_set_attr(void *ctx, struct idl_txn txn, uint8_t when, uint32_t iflag,
                            uint32_t oflag, uint32_t cflag, uint32_t lflag, const uint8_t cc[32],
                            uint32_t ispeed, uint32_t ospeed)
{
    struct ttyc *t = ctx;
    if (when > TTY_WHEN_FLUSH)
        return ERR_INVALID_ARGS;
    struct tty_wait w = { .txn = txn, .set = true, .when = when };
    termios_of(&w.t, iflag, oflag, cflag, lflag, cc, ispeed, ospeed);
    if (when == TTY_WHEN_NOW) {
        ld_set(&ld, &w.t);
        return OK;
    }
    return add_wait(t, &w);
}

static status_t op_drain(void *ctx, struct idl_txn txn)
{
    struct tty_wait w = { .txn = txn };
    return add_wait(ctx, &w);
}

static status_t op_flush(void *ctx, uint8_t queue)
{
    struct ttyc *t = ctx;
    if (queue > TTY_QUEUE_BOTH)
        return ERR_INVALID_ARGS;
    if (queue != TTY_QUEUE_OUT)
        ttys_flush_in(slot_of(t));
    if (queue != TTY_QUEUE_IN)
        out_discard(t);
    return OK;
}

static status_t op_get_size(void *ctx, uint16_t *c, uint16_t *r)
{
    (void)ctx;
    back->size(c, r);
    return OK;
}

static const struct tty_ops tty_ops = {
    .get_attr = op_get_attr, .set_attr = op_set_attr, .drain = op_drain, .flush = op_flush,
    .get_size = op_get_size,
};

/* ---- opening and closing ---------------------------------------------------------------- */

void ttys_init(void)
{
    ld_init(&ld, echo_cb, NULL);
}

/* The data channel: our end kept, with a duplicate of the client's end
 * (reading it takes back the input the client left unread); the client's
 * end given with the rights of a socketpair's end. */
static status_t make_data(struct ttyc *t, handle_t *give)
{
    handle_t theirs;
    status_t st = jam_channel_create(&t->data, &theirs);
    if (st != OK)
        return st;
    st = jam_handle_duplicate(theirs, RIGHT_READ | RIGHT_WAIT, &t->peer);
    if (st != OK)
        jam_handle_close(theirs);
    else
        st = jam_handle_replace(theirs, DATA_RIGHTS, give);   /* it closes theirs either way */
    if (st != OK) {
        if (t->peer)
            jam_handle_close(t->peer);
        jam_handle_close(t->data);
        t->peer = t->data = HANDLE_INVALID;
    }
    return st;
}

/* Close t's handles and free its slot (its focus entry is gone already). */
static void slot_free(struct ttyc *t)
{
    unsigned i = slot_of(t);
    jam_port_unbind(port, t->ch, KEY(K_TTY, i));
    jam_port_unbind(port, t->data, KEY(K_TTY_OUT, i));
    for (unsigned k = 0; k < t->nwaits; k++)   /* the client is gone: nobody to tell */
        (void)tty_reply_drain(t->waits[k].txn, ERR_PEER_CLOSED);
    jam_handle_close(t->ch);
    jam_handle_close(t->data);
    jam_handle_close(t->peer);
    out_pending &= ~(1u << i);
    out_deferred &= ~(1u << i);
    memset(t, 0, sizeof(*t));
}

status_t op_open_tty(void *ctx, handle_t *out_tty, handle_t *out_data)
{
    const struct client *opener = ctx;
    unsigned i;
    for (i = 0; i < MAX_TTYS && ttys[i].used; i++)
        ;
    if (i == MAX_TTYS)
        return ERR_NO_RESOURCES;
    struct ttyc *t = &ttys[i];
    handle_t theirs, data_give;
    status_t st = jam_channel_create(&t->ch, &theirs);
    if (st != OK)
        return st;
    if ((st = make_data(t, &data_give)) != OK) {
        jam_handle_close(t->ch);
        jam_handle_close(theirs);
        t->ch = HANDLE_INVALID;
        return st;
    }
    t->used = true;
    t->cl = (struct client){ .level = opener->level };
    st = jam_port_bind(port, t->ch, KEY(K_TTY, i), SIG_READABLE | SIG_PEER_CLOSED,
                       PORT_BIND_PERSISTENT);
    if (st == OK)
        st = jam_port_bind(port, t->data, KEY(K_TTY_OUT, i), SIG_READABLE, PORT_BIND_PERSISTENT);
    if (st == OK)
        st = focus_push(t->ch, &t->cl, (int)i);
    if (st != OK) {
        slot_free(t);
        jam_handle_close(theirs);
        jam_handle_close(data_give);
        return st;
    }
    *out_tty = theirs;
    *out_data = data_give;
    ttys_deliver();   /* what the terminal held for a reader */
    return OK;
}

unsigned ttys_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < MAX_TTYS; i++)
        n += ttys[i].used;
    return n;
}

void ttys_release(unsigned i)
{
    if (i >= MAX_TTYS || !ttys[i].used)
        return;
    struct ttyc *t = &ttys[i];
    in_drain(t, true);   /* what it left unread is the next reader's */
    slot_free(t);
}

void ttys_hang_up(unsigned i)
{
    for (unsigned f = 0; f < nfocus; f++)
        if (focus_tty[f] == (int)i) {
            focus_drop(f);   /* ttys_release, through the focus */
            return;
        }
}

/* ---- the loop ----------------------------------------------------------------------------- */

void tty_event(uint32_t kind, uint32_t i)
{
    if (i >= MAX_TTYS || !ttys[i].used)
        return;
    struct ttyc *t = &ttys[i];
    if (kind == K_TTY_OUT) {
        out_turn(t);
        return;
    }
    for (unsigned k = 0; k < REQ_BUDGET; k++) {
        status_t st = tty_serve_one(t->ch, &tty_ops, t);
        if (st == ERR_SHOULD_WAIT)
            return;
        if (st != OK) {   /* the client let it go: the terminal goes from it */
            ttys_hang_up(i);
            return;
        }
    }
    out_pending |= 1u << i;   /* more requests: another turn (ttys_pump) */
}

void ttys_refocus(void)
{
    refocus = true;
}

uint64_t ttys_deadline(void)
{
    if (out_pending || refocus)
        return 0;
    /* What ld holds waits for a reader: the newest tty's channel to take
     * more (push_retry_at), or a focus to change. */
    if (!ld_ready(&ld) || focus_top_tty() < 0)
        return DEADLINE_NEVER;
    return push_retry_at ? push_retry_at : 0;
}

void ttys_pump(void)
{
    uint32_t p = out_pending;
    for (unsigned i = 0; i < MAX_TTYS; i++) {
        if (!(p & 1u << i))
            continue;
        tty_event(K_TTY, i);   /* requests left over, then the output */
        if (ttys[i].used)
            out_turn(&ttys[i]);
    }
    refocus = false;
    ttys_deliver();
}

void ttys_resume(void)
{
    out_pending |= out_deferred;
    out_deferred = 0;
}

void ttys_resized(void)
{
    uint16_t c, r;
    back->size(&c, &r);
    for (unsigned i = 0; i < MAX_TTYS; i++)
        if (ttys[i].used)
            (void)tty_size(ttys[i].ch, c, r);   /* a full channel: that client misses it */
}
