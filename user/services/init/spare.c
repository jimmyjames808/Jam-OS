/* init's side of a service that outlives its process (docs/HISTORY.md, M11.6):
 * for the mixer, a keeper, a state VMO and a warm spare, on the supervision
 * module (<supervise.h>, user/lib/supervise.c), which holds how they work.
 * What is init's: which service it is (the mixer), its port keys
 * (KEY_SPARE, KEY_KEEP), the state VMO's size, the handles a start is
 * given, and adopting a promoted spare as the service (svc_adopt).
 *
 * When shell.c starts the mixer again after its end, the spare is promoted
 * (one message with the startup handles a new process would have been
 * given, and the time of the kill) and a new spare is started a little
 * later in the background (spare_due, after the loop's starts). Without a
 * spare (the boot word `nospare`, or none ready yet) a new process is
 * started as before. Either way the instance gets the state VMO and a new
 * keep channel, and only after it has its end does the keeper write its
 * restore into it.
 *
 * A mixer that reads neither SR_STATE nor SR_KEEP (one that doesn't
 * adopt state yet) starts fresh as before and costs nothing more: the
 * state's pages are committed only by svcstate_open, and the keeper's
 * restore is one done event (it holds nothing) on an end that closes with
 * the process.
 *
 * **Measured, every restart:** when the new instance runs (promoted, or a
 * process started), after the kill (initctl.kill) or after its end was
 * seen (a crash); and one cheap call on the shared audioctl channel init
 * keeps (audioctl.streams): kill to first answer, as a client feels it.
 * The spare's memory is its job's (the shell's `ps`).
 *
 * Everything here runs on init's loop thread (the module's probe thread
 * touches only its own state). */
#include <idl/audioctl.h>
#include <os.h>
#include <supervise.h>
#include "init.h"

/* The mixer's state VMO. The mixer's own layout (tens of KiB: two request
 * slots and its numbers) is the mixer's to set; svcstate_open refuses a
 * VMO smaller than the layout, so this is made with room to spare, which
 * costs nothing: pages are committed only as they are used. */
#define MIXER_STATE_SIZE (1ull << 20)

static struct sup_spare spare;   /* the mixer's warm spare */
static struct sup_kept mixer;    /* its keeper and state VMO */

static void sup_say(bool report, const char *line)
{
    if (report)
        init_say("%s", line);
    else
        printf("%s\n", line);
}

bool spare_kept(unsigned i)
{
    return i == MIXER;
}

void spare_init(handle_t loop_port, bool spares)
{
    sup_log("init", sup_say);
    const struct sup_spare_cfg cfg = { .path = svcs[MIXER].path, .port = loop_port,
                                       .key = KEY_SPARE };
    sup_spare_init(&spare, &cfg, spares);
    sup_kept_init(&mixer, loop_port, MIXER_STATE_SIZE, svcs[MIXER].path);
    if (!spares)
        printf("init: nospare: no warm spare; a restart of %s starts a process\n",
               svcs[MIXER].path);
}

/* ---- the spare ----------------------------------------------------------------------- */

uint64_t spare_due(uint64_t t)
{
    struct svc *s = &svcs[MIXER];
    if (s->given_up)
        sup_spare_dismiss(&spare);   /* a guard: every give-up calls kept_given_up too */
    if (s->given_up || !s->started)
        return DEADLINE_NEVER;   /* the mixer never started, or is given up on */
    sup_spare_arm(&spare, 0);        /* from its first start: a spare at once */
    return sup_spare_due(&spare, t);
}

void spare_event(void)
{
    sup_spare_event(&spare);
}

/* The instance's arguments into argv (2 entries): its path, and after an
 * end how the last instance ended, "killed" (a deliberate kill) or
 * "crashed": a service that adopts its state counts only crashes against
 * the request in progress (docs/HISTORY.md, M11.6, Q1's bad-request rule).
 * Returns argc. */
static int start_argv(const struct svc *s, const char *argv[2])
{
    argv[0] = s->path;
    if (!s->ended_at)
        return 1;
    argv[1] = s->kill_at ? "killed" : "crashed";
    return 2;
}

/* Promote the spare to be svc i, with x[0..nx) (each sent with r[k]).
 * ERR_NOT_FOUND: no spare waits (x untouched). Else x is consumed; OK:
 * svc i runs as the spare's process now. */
static status_t promote(unsigned i, struct spawn_handle *x, const rights_t *r, unsigned nx)
{
    if (!sup_spare_ready(&spare))
        return ERR_NOT_FOUND;
    struct svc *s = &svcs[i];
    const char *argv[2];
    struct standby_args a = { .hs = x, .rights = r, .n = nx, .argc = start_argv(s, argv),
                              .argv = argv, .envp = NULL,
                              .kill_ns = s->kill_at ? s->kill_at : s->ended_at };
    handle_t proc, job;
    status_t st = sup_spare_promote(&spare, &a, &proc, &job);
    if (st != OK)
        return st;
    return svc_adopt(i, proc, job);
}

/* ---- the keeper and the state ------------------------------------------------------ */

void kept_event(void)
{
    sup_kept_event(&mixer);
}

void kept_given_up(unsigned i)
{
    if (i != MIXER)
        return;
    sup_spare_dismiss(&spare);
    sup_kept_release(&mixer);
}

/* ---- the restart, measured ---------------------------------------------------------- */

/* One cheap call on the shared audioctl channel: audioctl.streams. */
static status_t probe_call(handle_t ch, uint64_t deadline)
{
    uint32_t len = 0;
    int32_t master = 0;
    uint8_t list[640];
    return audioctl_streams_until(ch, deadline, &master, list, &len);
}

/* Say how long svc i's restart took to get it running (at `up`), after
 * the deliberate kill at kill_at (0: it crashed), and measure its first
 * answer. */
static void said(unsigned i, bool promoted, uint64_t kill_at, uint64_t up)
{
    handle_t ch = HANDLE_INVALID;
    handle_t ctl = services_audioctl();
    if (!ctl || jam_handle_duplicate(ctl, RIGHT_SAME, &ch) != OK)
        ch = HANDLE_INVALID;
    sup_restarted(svcs[i].path, promoted, kill_at, svcs[i].ended_at, up, ch, probe_call);
}

status_t kept_start(unsigned i, struct spawn_handle *x, unsigned nx)
{
    rights_t r[STANDBY_MAX_HANDLES];
    if (i != MIXER || nx + KEPT_EXTRA > STANDBY_MAX_HANDLES) {
        for (unsigned j = 0; j < nx; j++)
            if (x[j].h)
                jam_handle_close(x[j].h);
        return ERR_INVALID_ARGS;
    }
    for (unsigned j = 0; j < nx; j++)
        r[j] = RIGHT_SAME;
    nx = sup_kept_handles(&mixer, x, r, nx);
    struct svc *s = &svcs[i];
    uint64_t kill_at = s->kill_at;   /* the start clears it */
    status_t st = promote(i, x, r, nx);
    bool promoted = st == OK;
    if (st == ERR_NOT_FOUND) {
        const char *argv[2];
        struct svc_args a = { .argc = start_argv(s, argv), .argv = argv, .x = x, .rights = r,
                              .nx = nx };
        st = svc_start_args(i, &a);
    }
    if (st != OK)
        return st;
    uint64_t up = now();
    sup_kept_hand_over(&mixer, KEY_KEEP);
    if (s->ended_at)   /* a restart */
        said(i, promoted, kill_at, up);
    return OK;
}
