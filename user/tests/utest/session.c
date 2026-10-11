/* utest: init's sessions, a session's life (sessrig.h; init's sessions.c).
 *
 * t_session_shell: an interactive session: the prompt, `echo`, TERM from
 * open's `term` (a POSIX program on it prints it), a resize through the
 * control channel seen by that program (TIOCGWINSZ), a zero size refused,
 * signal 0 answered; `exit 7` ends it: `ended` says 7, the shell's process
 * ends with 7, the master gives the rest and closes, and nothing of the
 * session is left.
 *
 * t_session_command: open's `command`: the line alone (no banner, no
 * prompt), its output, and the session's end with its status (0, 3 from
 * `exit 3`, 1 from `false`).
 *
 * t_session_hangup: session_ctl.hangup, signal 15 and closing the master
 * each end a session: `ended` 2 (the shell's console went), the processes
 * gone; a POSIX program on the terminal gets SIGHUP (its handler writes a
 * file) before what is left is killed.
 *
 * t_session_limit: SESSIONS_MAX sessions run at once, the next is
 * ERR_NO_RESOURCES; one hung up makes room for a new one.
 *
 * t_session_posix: posixtest's session section (jamos_session_open: a
 * session as a POSIX program has it, its master a descriptor and its
 * shell a child for waitpid and kill), given /svc/session, which its own
 * list doesn't ask for. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <idl/session.h>
#include <os.h>
#include "conrig.h"
#include "sessrig.h"
#include "utest.h"

#define HUP_FILE "/data/x1-hup.txt"   /* posixtest --child-hup writes it on SIGHUP */

/* ---- the rig ---------------------------------------------------------------------- */

bool sess_absent(void)
{
    handle_t ch;
    if (svc_open(SVC_SESSION, &ch) == OK) {
        jam_handle_close(ch);
        return false;
    }
    printf("utest: %s: no /svc/session here (not a shell-mode init): skipped\n", utest_cur);
    return true;
}

bool sess_connect(handle_t *ch)
{
    CHECK_ST(svc_open(SVC_SESSION, ch), OK);
    return true;
}

status_t sess_open_st(uint16_t cols, uint16_t rows, const char *term, const char *cmd,
                      struct sess *s)
{
    handle_t ch;
    memset(s, 0, sizeof(*s));
    status_t st = svc_open(SVC_SESSION, &ch);
    if (st != OK)
        return st;
    st = session_open_within(ch, CON_WAIT, cols, rows, term, (uint32_t)strlen(term), cmd,
                             (uint32_t)strlen(cmd), &s->master, &s->ctl, &s->proc);
    jam_handle_close(ch);
    return st;
}

bool sess_open(uint16_t cols, uint16_t rows, const char *term, const char *cmd, struct sess *s)
{
    CHECK_ST(sess_open_st(cols, rows, term, cmd, s), OK);
    memset(&rig, 0, sizeof(rig));
    rig.master = s->master;
    return true;
}

void sess_close(struct sess *s)
{
    handle_t *hs[] = { &s->master, &s->ctl, &s->proc };
    for (unsigned k = 0; k < 3; k++)
        if (*hs[k])
            jam_handle_close(*hs[k]);
    if (rig.master == s->master)
        rig.master = HANDLE_INVALID;
    memset(s, 0, sizeof(*s));
}

static void on_ended(void *ctx, int32_t code)
{
    *(int64_t *)ctx = code;
}

bool sess_ended(struct sess *s, int32_t *code)
{
    static const struct session_ctl_events ev = { .ended = on_ended };
    int64_t got = INT64_MIN;
    uint64_t until = now() + (uint64_t)STEP_MS * NS_PER_MS;
    while (got == INT64_MIN) {
        signals_t seen;
        CHECK_ST(jam_object_wait_one(s->ctl, SIG_READABLE, until, &seen), OK);
        CHECK_ST(session_ctl_read_event(s->ctl, &ev, &got), OK);
    }
    *code = (int32_t)got;
    return true;
}

bool sess_master_ends(struct sess *s)
{
    uint64_t until = now() + (uint64_t)STEP_MS * NS_PER_MS;
    while (rig_take(until))
        ;
    signals_t seen = 0;
    CHECK_ST(jam_object_wait_one(s->master, SIG_PEER_CLOSED, 0, &seen), OK);
    return true;
}

bool sess_count(unsigned *n)
{
    static struct proc_stat ps[512];
    int64_t got = jam_proc_list(startup_handle(SR_RESOURCE), ps, 512);
    if (got < 0)
        FAIL("proc_list: %s", status_str((status_t)got));
    *n = 0;
    for (int64_t i = 0; i < got; i++)
        if (ps[i].state != PROCESS_DEAD && (!strcmp(ps[i].name, "session-shell") ||
                                            !strcmp(ps[i].name, "session-console")))
            (*n)++;
    return true;
}

bool sess_all_gone(void)
{
    uint64_t until = now() + (uint64_t)STEP_MS * NS_PER_MS;
    unsigned n = 1;
    while (sess_count(&n) && n && now() < until)
        (void)jam_sleep_until(now() + 20 * NS_PER_MS);
    if (n)
        FAIL("%u session processes left", n);
    return true;
}

bool sess_runs(const char *line, const char *want)
{
    CHECK(rig_typed(line, strlen(line)));
    CHECK(rig_typed("\r", 1));
    CHECK(rig_expect(want, STEP_MS));
    return rig_expect(SESS_PROMPT, STEP_MS);
}

/* ---- 1. an interactive session -------------------------------------------------------- */

static bool shell_flow(struct sess *s)
{
    CHECK(rig_expect("Jam OS shell", STEP_MS));
    CHECK(rig_expect(SESS_PROMPT, STEP_MS));
    CHECK(sess_runs("echo hi-session", "\r\nhi-session\r\n"));
    CHECK(sess_runs("echo $TERM", "\r\nxterm-256color\r\n"));
    CHECK_ST(session_ctl_resize_within(s->ctl, CON_WAIT, 0, 30), ERR_INVALID_ARGS);
    CHECK_ST(session_ctl_resize_within(s->ctl, CON_WAIT, 120, 40), OK);
    CHECK_ST(session_ctl_signal_within(s->ctl, CON_WAIT, 0), OK);
    CHECK_ST(session_ctl_signal_within(s->ctl, CON_WAIT, 2), ERR_NOT_SUPPORTED);
    /* A POSIX program on the terminal sees the size and the TERM. */
    CHECK(sess_runs("posixtest --child-winsize", "winsize 120x40 TERM=xterm-256color"));
    CHECK(rig_typed("exit 7\r", 7));
    int32_t code = 0;
    CHECK(sess_ended(s, &code));
    CHECK_EQ(code, 7);
    CHECK(rig_exit_code_is(s->proc, 7));
    CHECK(sess_master_ends(s));
    CHECK_ST(session_ctl_signal_within(s->ctl, CON_WAIT, 0), ERR_BAD_STATE);
    return true;
}

bool t_session_shell(void)
{
    if (sess_absent())
        return true;
    struct sess s;
    CHECK(sess_open(100, 30, "xterm-256color", "", &s));
    bool ok = shell_flow(&s);
    sess_close(&s);
    return ok && sess_all_gone();
}

/* ---- 2. a command --------------------------------------------------------------------- */

/* cmd's session: want in its output, no banner, then its end with code. */
static bool command_is(const char *cmd, const char *want, int32_t code)
{
    struct sess s;
    CHECK(sess_open(80, 24, "", cmd, &s));
    bool ok = !want[0] || rig_expect(want, STEP_MS);
    int32_t got = -100;
    ok = ok && sess_ended(&s, &got) && sess_master_ends(&s);
    if (ok && (got != code || rig_saw("Jam OS shell") || strstr(rig.out, "Jam OS shell"))) {
        printf("utest: %s: \"%s\" ended with %d (want %d), or showed the banner\n", utest_cur,
               cmd, (int)got, (int)code);
        ok = false;
    }
    sess_close(&s);
    return ok;
}

bool t_session_command(void)
{
    if (sess_absent())
        return true;
    CHECK(command_is("echo cmd-out", "cmd-out\r\n", 0));
    CHECK(command_is("exit 3", "", 3));
    CHECK(command_is("false", "", 1));
    CHECK(command_is("echo a; echo b", "a\r\nb\r\n", 0));
    return sess_all_gone();
}

/* ---- 3. hang-ups ---------------------------------------------------------------------- */

/* s at its prompt is hung up by how: 0 hangup, 1 signal 15, 2 the master closed. */
static bool hung_up_by(int how)
{
    struct sess s;
    CHECK(sess_open(80, 24, "", "", &s));
    bool ok = rig_expect(SESS_PROMPT, STEP_MS);
    if (ok && how == 0)
        ok = session_ctl_hangup_within(s.ctl, CON_WAIT) == OK;
    if (ok && how == 1)
        ok = session_ctl_signal_within(s.ctl, CON_WAIT, 15) == OK;
    if (ok && how == 2) {
        jam_handle_close(s.master);
        s.master = rig.master = HANDLE_INVALID;
    }
    int32_t code = 0;
    ok = ok && sess_ended(&s, &code);
    if (ok && code != 2) {
        printf("utest: %s: hung up (%d): ended with %d, want 2\n", utest_cur, how, (int)code);
        ok = false;
    }
    sess_close(&s);
    return ok;
}

/* A POSIX program on the terminal when it hangs up: its SIGHUP handler
 * runs (it writes HUP_FILE) before the session's leftovers are killed. */
static bool sighup(void)
{
    (void)fs_unlink(HUP_FILE);
    struct sess s;
    CHECK(sess_open(80, 24, "", "", &s));
    bool ok = rig_expect(SESS_PROMPT, STEP_MS) &&
              rig_typed("posixtest --child-hup\r", 22) && rig_expect("hup-ready", STEP_MS);
    if (ok)
        ok = session_ctl_hangup_within(s.ctl, CON_WAIT) == OK;
    sess_close(&s);
    uint64_t until = now() + (uint64_t)STEP_MS * NS_PER_MS;
    while (ok && fs_stat(HUP_FILE, NULL, NULL, NULL) != OK && now() < until)
        (void)jam_sleep_until(now() + 20 * NS_PER_MS);
    if (ok && fs_stat(HUP_FILE, NULL, NULL, NULL) != OK) {
        printf("utest: %s: no %s: the program got no SIGHUP\n", utest_cur, HUP_FILE);
        ok = false;
    }
    (void)fs_unlink(HUP_FILE);
    return ok;
}

bool t_session_hangup(void)
{
    if (sess_absent())
        return true;
    for (int how = 0; how < 3; how++)
        CHECK(hung_up_by(how));
    CHECK(sess_all_gone());
    if (fs_stat("/data", NULL, NULL, NULL) == OK)   /* the SIGHUP check writes a file there */
        CHECK(sighup());
    return sess_all_gone();
}

/* ---- 4. from a POSIX program ---------------------------------------------------------- */

bool t_session_posix(void)
{
    if (sess_absent())
        return true;
    static const char *const grants[] = { "/svc/" SVC_SESSION, "/data", "/boot", NULL };
    const char *argv[] = { "posixtest", "session" };
    handle_t proc;
    struct spawn_args a = { .path = "bin/posixtest", .argc = 2, .argv = argv,
                            .job = startup_handle(SR_JOB), .ns = grants };
    CHECK_ST(spawn(&a, &proc), OK);
    struct process_info info;
    CHECK_ST(spawn_wait(proc, 60 * NS_PER_S, &info), OK);
    jam_handle_close(proc);
    if (info.killed || info.exit_code)
        FAIL("posixtest session ended with %ld (its lines are in the log)", (long)info.exit_code);
    return sess_all_gone();
}

/* ---- 5. the limit ---------------------------------------------------------------------- */

bool t_session_limit(void)
{
    if (sess_absent())
        return true;
    struct sess s[SESSION_SESSIONS_MAX + 1];
    memset(s, 0, sizeof(s));
    bool ok = true;
    for (unsigned i = 0; ok && i < SESSION_SESSIONS_MAX; i++)
        ok = sess_open_st(80, 24, "", "", &s[i]) == OK;
    if (ok)
        ok = sess_open_st(80, 24, "", "", &s[SESSION_SESSIONS_MAX]) == ERR_NO_RESOURCES;
    if (ok) {   /* one hangs up: room for another */
        ok = session_ctl_hangup_within(s[0].ctl, CON_WAIT) == OK;
        sess_close(&s[0]);
        ok = ok && sess_open_st(80, 24, "", "", &s[0]) == OK;
    }
    if (!ok)
        printf("utest: %s: the %u sessions' limit didn't hold\n", utest_cur,
               SESSION_SESSIONS_MAX);
    for (unsigned i = 0; i <= SESSION_SESSIONS_MAX; i++)
        sess_close(&s[i]);
    return sess_all_gone() && ok;
}
