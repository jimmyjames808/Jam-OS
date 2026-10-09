/* A service's notices on the desktop (<notice.h>): notify.idl's post. */
#include <idl/notify.h>
#include <idl/svc.h>
#include <notice.h>

_Static_assert(NOTICE_DISMISSED == NOTIFY_DISMISSED, "notice.h follows notify.idl");

void notice_init(struct notice_box *b, handle_t svc)
{
    *b = (struct notice_box){ .svc = svc };
}

static void say_once(struct notice_box *b, const char *title, status_t st)
{
    if (!b->said)
        printf("notice: \"%s\" not shown on the desktop (%s)\n", title, status_str(st));
    b->said = true;
}

/* title or body cut to n bytes, at a character's start (the server
 * refuses a str that isn't whole UTF-8). */
static uint32_t cut(const char *s, uint32_t n)
{
    uint32_t len = (uint32_t)strlen(s);
    if (len <= n)
        return len;
    while (n && ((unsigned char)s[n] & 0xc0) == 0x80)
        n--;
    return n;
}

/* A channel of our own on /svc/notify (b->ch; HANDLE_INVALID if not). */
static status_t connect(struct notice_box *b)
{
    handle_t ch = HANDLE_INVALID;
    status_t st = svc_connect_within(b->svc, NOTICE_CONNECT_WAIT, &ch);
    b->ch = st == OK ? ch : HANDLE_INVALID;
    return st;
}

static status_t post(struct notice_box *b, const char *title, const char *body, char icon,
                     uint8_t tint)
{
    return notify_post(b->ch, title, cut(title, 63), body, cut(body, 95), (uint8_t)icon, tint);
}

void notice_post(struct notice_box *b, const char *title, const char *body, char icon,
                 uint8_t tint)
{
    if (!b->svc)
        return;   /* no compositor: the log has it */
    if (!body)
        body = "";
    status_t st = OK;
    if (!b->ch)
        st = connect(b);
    if (st == OK)
        st = post(b, title, body, icon, tint);
    if (st == ERR_PEER_CLOSED) {   /* the compositor restarted: once more on a new channel */
        jam_handle_close(b->ch);
        b->ch = HANDLE_INVALID;
        st = connect(b);
        if (st == OK)
            st = post(b, title, body, icon, tint);
    }
    if (st != OK && st != ERR_SHOULD_WAIT && b->ch) {
        jam_handle_close(b->ch);
        b->ch = HANDLE_INVALID;
    }
    if (st != OK)
        say_once(b, title, st);   /* ERR_SHOULD_WAIT: the compositor's queue is full */
    else
        b->said = false;
}
