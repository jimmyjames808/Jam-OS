/* utest: init's sessions (/svc/session, abi/idl/session.idl): what
 * session.c (a session's life) and sessbad.c (the hostile cases) share.
 * Only a shell-mode init serves /svc/session: in the regression run (init's
 * test mode) every session test says it is skipped and passes. The master
 * is read and typed through conrig.c's remote terminal (rig.master). */
#pragma once

#include <os.h>

#define SESS_PROMPT "\033[0m> "   /* the end of the shell's prompt (shell/main.c) */

/* A session as the opener holds it. */
struct sess {
    handle_t master, ctl, proc;
};

/* /svc/session isn't in our namespace: said once per test, true (skip). */
bool sess_absent(void);
/* A channel of our own to /svc/session (svc.connect). */
bool sess_connect(handle_t *ch);
/* session.open on a new channel of our own: st is what it answered. */
status_t sess_open_st(uint16_t cols, uint16_t rows, const char *term, const char *cmd,
                      struct sess *s);
/* The same, OK wanted; rig.master is its master from now on. */
bool sess_open(uint16_t cols, uint16_t rows, const char *term, const char *cmd, struct sess *s);
/* Close what is left of s (a hang-up, if the master is still open). */
void sess_close(struct sess *s);
/* session_ctl.ended's code, waited for (up to STEP_MS) on s's ctl. */
bool sess_ended(struct sess *s, int32_t *code);
/* The master gives what is left, then its peer closes (the console has
 * ended), within STEP_MS. */
bool sess_master_ends(struct sess *s);
/* The session processes init runs ("session-shell", "session-console")
 * still alive: *n; false if the list can't be read (no sysinfo power). */
bool sess_count(unsigned *n);
/* Wait (up to STEP_MS) until none is alive. */
bool sess_all_gone(void);
/* The shell runs line, and want comes before its next prompt. */
bool sess_runs(const char *line, const char *want);
