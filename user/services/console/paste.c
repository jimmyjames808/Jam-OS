/* console: pasted text typed into the focus (console.h, "paste.c").
 *
 * A paste (Super+V in a terminal window: the clipboard's text, window.c)
 * becomes key presses on the newest focus's key channel, each with usage
 * 0 and its character, as a serial terminal's keys come (paste_key says
 * which characters go). If the client that opened that focus asked for
 * bracketed paste (it wrote ESC [ ? 2004 h, term.c), the keys come
 * between ESC [ 200 ~ and ESC [ 201 ~, each byte a key (ESC is the Escape
 * key: usage 0x29 and 0x1b), so the program knows the text was pasted,
 * not typed: the shell then puts it on its line and runs nothing
 * (user/services/shell/sh_paste.c). Pasted text carries no ESC of its own,
 * so it can't end the bracket early.
 *
 * A channel holds at most 1024 messages, a long paste more keys than
 * that: the keys go as fast as the program reads them, what doesn't fit
 * now waiting for paste_deadline (PASTE_RETRY_NS). Only one paste at a
 * time; it is dropped when its focus is no longer the newest (a program
 * started, or ended), and its text dropped (the bracket still closed) by
 * a key typed (paste_stop). */
#include "console.h"

#define PASTE_RETRY_NS (5 * NS_PER_MS)   /* a full channel: try again after this */
#define PASTE_BUDGET   512u              /* keys a loop turn writes, at most */
#define U_ESC          0x29              /* HID usage: Escape */

enum { P_OPEN, P_TEXT, P_CLOSE };
static const char open_seq[] = "\033[200~", close_seq[] = "\033[201~";

static struct {
    char    *text;                 /* the text (NULL: no paste), len bytes */
    size_t   len, at;              /* ... and how far it has been typed */
    handle_t to;                   /* the focus channel it goes to */
    bool     bracketed;            /* between the markers */
    uint8_t  phase;                /* P_*: the opening marker, the text, the closing one */
    unsigned mark;                 /* bytes of the marker of this phase sent */
    uint64_t retry_at;             /* a full channel: the next try (0: none) */
} paste;

static void paste_free(void)
{
    free(paste.text);
    paste.text = NULL;
}

bool paste_start(const char *text, size_t n)
{
    if (paste.text || !nfocus)
        return false;
    paste.text = malloc(n ? n : 1);
    if (!paste.text)
        return false;
    memcpy(paste.text, text, n);
    struct client *c = focus_client[nfocus - 1];
    paste.len = n;
    paste.at = 0;
    paste.to = focus[nfocus - 1];
    paste.bracketed = c && c->bracketed;
    paste.phase = paste.bracketed ? P_OPEN : P_TEXT;
    paste.mark = 0;
    paste.retry_at = 0;
    if (view_back) {   /* as typing does: back to the bottom */
        view_back = 0;
        dirty = true;
    }
    return true;
}

/* The next key of the paste, and how it advances (bytes of text, or one
 * byte of a marker): false at its end. */
static bool next_key(struct input_key_event *ev, size_t *text_at)
{
    *text_at = paste.at;
    if (paste.phase == P_TEXT) {
        if (paste_key(paste.text, paste.len, text_at, ev))
            return true;
        if (!paste.bracketed)
            return false;
        paste.phase = P_CLOSE;
        paste.mark = 0;
    }
    const char *seq = paste.phase == P_OPEN ? open_seq : close_seq;
    if (paste.mark == sizeof(open_seq) - 1) {
        if (paste.phase == P_CLOSE)
            return false;
        paste.phase = P_TEXT;   /* the opener is out: the text */
        return next_key(ev, text_at);
    }
    uint8_t b = (uint8_t)seq[paste.mark];
    *ev = (struct input_key_event){ b == 0x1b ? U_ESC : 0, INPUT_KEY_DOWN, 0, b };
    return true;
}

void paste_pump(void)
{
    if (!paste.text || (paste.retry_at && now() < paste.retry_at))
        return;
    paste.retry_at = 0;
    if (!nfocus || focus[nfocus - 1] != paste.to) {
        paste_free();   /* its program went, or another took the keys */
        return;
    }
    for (unsigned n = 0; n < PASTE_BUDGET; n++) {
        struct input_key_event ev;
        size_t text_at;
        if (!next_key(&ev, &text_at)) {
            paste_free();
            return;
        }
        status_t st = jam_channel_write(paste.to, &ev, sizeof(ev), NULL, 0);
        if (st == ERR_SHOULD_WAIT) {
            paste.retry_at = now() + PASTE_RETRY_NS;
            return;
        }
        if (st != OK) {
            paste_free();   /* the focus's client is gone: keys.c drops it */
            return;
        }
        if (paste.phase == P_TEXT)
            paste.at = text_at;
        else
            paste.mark++;
    }
}

uint64_t paste_deadline(void)
{
    if (!paste.text)
        return DEADLINE_NEVER;
    return paste.retry_at;   /* 0: more to write now */
}

void paste_stop(void)
{
    if (!paste.text)
        return;
    if (!paste.bracketed || (paste.phase == P_OPEN && !paste.mark)) {
        paste_free();   /* nothing of a bracket is out: nothing to close */
        return;
    }
    paste.at = paste.len;   /* no more text: the opener finished, then the closer */
}
