/* utest: init's sessions, the hostile cases (sessrig.h; init's
 * sessions.c). init serves every session from its one loop, so nothing an
 * opener sends may stop it, and nothing an opener leaves may pile up.
 *
 * t_session_bad_args: a zero size, a TERM byte outside its set, a control
 * character in the command, a command past the shell's line: refused,
 * nothing started.
 *
 * t_session_garbage: junk, unfinished escapes and a handle typed into the
 * master; junk, a handle, an unknown method and another protocol's
 * message on the control channel and on an opener's channel: each
 * refused, and the session (and the opener) still answer.
 *
 * t_session_ctl_first: the control channel closed first: the session
 * carries on (no `ended` to send), and ends when the master closes.
 *
 * t_session_unread: an opener and a control channel that leave more
 * answers unread than the protocol's bound are cut off (their channels
 * closed), the session itself going on.
 *
 * t_session_opens: 1,000 connects closed at once, and 1,000 sessions
 * opened and closed at once (hung up before their shell is up): nothing
 * left behind, and SESSIONS_MAX sessions open after.
 *
 * t_session_no_grant: a program whose namespace lacks /svc/session can't
 * reach it (a child given /boot alone). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/session.h>
#include <os.h>
#include "conrig.h"
#include "sessrig.h"
#include "utest.h"

#define OPENS     1000
#define RAW_TXID  7u

/* A raw request (ordinal, extra zero bytes, maybe a handle) on ch: its
 * answer's status, waited for. */
static status_t raw_call(handle_t ch, uint32_t ordinal, uint32_t extra, bool with_handle)
{
    _Alignas(8) uint8_t b[64] = { 0 };
    ((uint32_t *)b)[0] = RAW_TXID;
    ((uint32_t *)b)[1] = ordinal;
    handle_t h = HANDLE_INVALID;
    if (with_handle && jam_event_create(&h) != OK)
        return ERR_INTERNAL;
    status_t st = jam_channel_write(ch, b, 8 + extra, with_handle ? &h : NULL, with_handle);
    if (st != OK)
        return st;
    signals_t seen;
    st = jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, now() + CON_WAIT, &seen);
    if (st != OK)
        return st;
    _Alignas(8) uint8_t rep[64];
    struct idl_msg m;
    st = idl_reply_read(ch, rep, sizeof(rep), &m);
    if (st != OK)
        return st;
    idl_msg_drop(&m);
    return m.n >= 8 && m.txid == RAW_TXID ? ((const struct idl_rep_hdr *)rep)->status
                                          : ERR_INTERNAL;
}

/* ---- arguments ------------------------------------------------------------------------ */

bool t_session_bad_args(void)
{
    if (sess_absent())
        return true;
    struct sess s;
    char longer[SESSION_COMMAND_RUN_MAX + 2];
    memset(longer, 'e', sizeof(longer) - 1);
    longer[sizeof(longer) - 1] = '\0';
    CHECK_ST(sess_open_st(0, 24, "", "", &s), ERR_INVALID_ARGS);
    CHECK_ST(sess_open_st(80, 0, "", "", &s), ERR_INVALID_ARGS);
    CHECK_ST(sess_open_st(80, 24, "xterm;rm", "", &s), ERR_INVALID_ARGS);
    CHECK_ST(sess_open_st(80, 24, "x y", "", &s), ERR_INVALID_ARGS);
    CHECK_ST(sess_open_st(80, 24, "", "echo a\necho b", &s), ERR_INVALID_ARGS);
    CHECK_ST(sess_open_st(80, 24, "", "echo \x7f", &s), ERR_INVALID_ARGS);
    CHECK_ST(sess_open_st(80, 24, "", longer, &s), ERR_OUT_OF_RANGE);
    unsigned n = 1;
    CHECK(sess_count(&n));
    CHECK_EQ(n, 0);
    return true;
}

/* ---- garbage ---------------------------------------------------------------------------- */

static bool master_garbage(void)
{
    static const char junk[] = "\033[9999;9999;9999H\033]0;\033P\x90\xff\xfe\033[?";
    CHECK(rig_typed(junk, sizeof(junk) - 1));
    uint8_t rnd[512];
    os_random(rnd, sizeof(rnd));
    for (size_t i = 0; i < sizeof(rnd); i++)
        if (rnd[i] < 0x20 || rnd[i] == 0x7f)
            rnd[i] = 'x';   /* no control keys (Enter, Ctrl+D, Tab) in it: printable junk */
    CHECK(rig_typed((const char *)rnd, sizeof(rnd)));
    handle_t ev;
    CHECK_ST(jam_event_create(&ev), OK);
    CHECK_ST(jam_channel_write(rig.master, "z", 1, &ev, 1), OK);   /* closed by the console */
    /* Ctrl+C drops the junk on the line: twice, in case an escape the junk
     * left open took the first. */
    rig_settle(300);
    CHECK(rig_typed("\x03", 1));
    rig_settle(300);
    CHECK(rig_typed("\x03", 1));
    CHECK(rig_expect(SESS_PROMPT, STEP_MS));
    rig_settle(300);
    return sess_runs("echo after-junk", "\r\nafter-junk\r\n");
}

static bool ctl_garbage(struct sess *s)
{
    uint32_t base = SESSION_CTL_PROTOCOL_ID << 16;
    CHECK_ST(raw_call(s->ctl, base | 99, 0, false), ERR_NOT_SUPPORTED);
    /* The other protocol's ordinal at this one's size (a bigger message is
     * ERR_INVALID_ARGS before its ordinal is looked at). */
    CHECK_ST(raw_call(s->ctl, SESSION_OPEN, 4, false), ERR_NOT_SUPPORTED);
    CHECK_ST(raw_call(s->ctl, SESSION_OPEN, 64 - 8, false), ERR_INVALID_ARGS);
    CHECK_ST(raw_call(s->ctl, base | 1, 4, true), ERR_INVALID_ARGS);        /* a handle */
    CHECK_ST(raw_call(s->ctl, base | 1, 1, false), ERR_INVALID_ARGS);       /* short */
    CHECK_ST(session_ctl_resize_within(s->ctl, CON_WAIT, 90, 25), OK);
    return true;
}

static bool opener_garbage(void)
{
    handle_t ch;
    CHECK(sess_connect(&ch));
    uint32_t base = SESSION_PROTOCOL_ID << 16;
    bool ok = raw_call(ch, base | 99, 0, false) == ERR_NOT_SUPPORTED &&
              raw_call(ch, base | 1, 2, false) == ERR_INVALID_ARGS &&
              raw_call(ch, base | 1, 8, true) == ERR_INVALID_ARGS &&
              raw_call(ch, (SESSION_CTL_PROTOCOL_ID << 16) | 1, 4, false) == ERR_NOT_SUPPORTED;
    struct sess s2;
    /* The channel still answers: a bad open is refused as ever. */
    ok = ok && session_open_within(ch, CON_WAIT, 0, 0, "", 0, "", 0, &s2.master, &s2.ctl,
                                   &s2.proc) == ERR_INVALID_ARGS;
    jam_handle_close(ch);
    if (!ok)
        printf("utest: %s: an opener's junk wasn't refused as it should be\n", utest_cur);
    return ok;
}

bool t_session_garbage(void)
{
    if (sess_absent())
        return true;
    struct sess s;
    CHECK(sess_open(80, 24, "", "", &s));
    bool ok = rig_expect(SESS_PROMPT, STEP_MS) && master_garbage() && ctl_garbage(&s) &&
              opener_garbage() && sess_runs("echo still-here", "\r\nstill-here\r\n");
    sess_close(&s);
    return sess_all_gone() && ok;
}

/* ---- the control channel closed first ------------------------------------------------- */

bool t_session_ctl_first(void)
{
    if (sess_absent())
        return true;
    struct sess s;
    CHECK(sess_open(80, 24, "", "", &s));
    jam_handle_close(s.ctl);
    s.ctl = HANDLE_INVALID;
    bool ok = rig_expect(SESS_PROMPT, STEP_MS) && sess_runs("echo no-ctl", "\r\nno-ctl\r\n");
    jam_handle_close(s.master);   /* the hang-up */
    s.master = rig.master = HANDLE_INVALID;
    ok = ok && rig_exit_code_is(s.proc, 2);
    sess_close(&s);
    return sess_all_gone() && ok;
}

/* ---- answers left unread ------------------------------------------------------------- */

/* Send n requests on ch and read none: true once ch's peer has closed
 * (init cut it off), within STEP_MS. */
static bool cut_off_after(handle_t ch, uint32_t ordinal, uint32_t extra, unsigned n)
{
    _Alignas(8) uint8_t b[16] = { 0 };
    ((uint32_t *)b)[1] = ordinal;
    for (unsigned i = 0; i < n; i++) {
        ((uint32_t *)b)[0] = 100 + i;   /* a txid of our own, never read */
        if (jam_channel_write(ch, b, 8 + extra, NULL, 0) != OK)
            break;   /* closed already */
    }
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(ch, SIG_PEER_CLOSED, now() + (uint64_t)STEP_MS * NS_PER_MS,
                                 &seen), OK);
    return true;
}

bool t_session_unread(void)
{
    if (sess_absent())
        return true;
    handle_t ch;
    CHECK(sess_connect(&ch));
    bool ok = cut_off_after(ch, (SESSION_PROTOCOL_ID << 16) | 99, 0, 40);
    jam_handle_close(ch);
    struct sess s;
    ok = ok && sess_open(80, 24, "", "", &s);   /* a new opener is served */
    if (ok) {
        uint32_t resize = (SESSION_CTL_PROTOCOL_ID << 16) | 1;
        ok = rig_expect(SESS_PROMPT, STEP_MS) && cut_off_after(s.ctl, resize, 4, 40) &&
             sess_runs("echo ctl-cut", "\r\nctl-cut\r\n");   /* the session goes on */
        sess_close(&s);
    }
    return sess_all_gone() && ok;
}

/* ---- 1,000 opens --------------------------------------------------------------------- */

bool t_session_opens(void)
{
    if (sess_absent())
        return true;
    uint64_t t0 = now();
    for (unsigned i = 0; i < OPENS; i++) {
        handle_t ch;
        CHECK(sess_connect(&ch));
        jam_handle_close(ch);
    }
    uint64_t t1 = now();
    for (unsigned i = 0; i < OPENS; i++) {
        struct sess s;
        /* SESSIONS_MAX may still run while init sees the last ones end. */
        uint64_t until = now() + (uint64_t)STEP_MS * NS_PER_MS;
        status_t st;
        while ((st = sess_open_st(80, 24, "", "", &s)) == ERR_NO_RESOURCES && now() < until)
            (void)jam_sleep_until(now() + 2 * NS_PER_MS);
        if (st != OK)
            FAIL("open %u of %u: %s", i, OPENS, status_str(st));
        sess_close(&s);   /* hung up at once */
    }
    printf("utest: %s: %u connects in %lu ms, %u sessions opened and closed in %lu ms\n",
           utest_cur, OPENS, (unsigned long)((t1 - t0) / NS_PER_MS), OPENS,
           (unsigned long)((now() - t1) / NS_PER_MS));
    CHECK(sess_all_gone());
    struct sess s[SESSION_SESSIONS_MAX];
    memset(s, 0, sizeof(s));
    bool ok = true;
    for (unsigned i = 0; ok && i < SESSION_SESSIONS_MAX; i++)
        ok = sess_open_st(80, 24, "", "", &s[i]) == OK;
    for (unsigned i = 0; i < SESSION_SESSIONS_MAX; i++)
        sess_close(&s[i]);
    if (!ok)
        printf("utest: %s: %u sessions didn't open after the loop\n", utest_cur,
               SESSION_SESSIONS_MAX);
    return sess_all_gone() && ok;
}

/* ---- no grant ------------------------------------------------------------------------- */

/* "utest session-probe": /svc/session is not ours (exit 0), or it is (1). */
int session_child(int argc, char **argv)
{
    (void)argc;
    if (strcmp(argv[1], "session-probe"))
        return 127;
    handle_t ch;
    status_t st = svc_open(SVC_SESSION, &ch);
    if (st == OK)
        jam_handle_close(ch);
    return st == ERR_NOT_FOUND ? 0 : 1;
}

bool t_session_no_grant(void)
{
    if (sess_absent())
        return true;
    static const char *const only_boot[] = { "/boot", NULL };
    const char *argv[] = { "utest", "session-probe" };
    handle_t proc;
    struct spawn_args a = { .path = "bin/utest", .argc = 2, .argv = argv,
                            .job = startup_handle(SR_JOB), .ns = only_boot };
    CHECK_ST(spawn(&a, &proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, CON_WAIT, &info), OK);
    jam_handle_close(proc);
    CHECK_EQ(info.killed, 0);
    CHECK_EQ(info.exit_code, 0);
    return true;
}
