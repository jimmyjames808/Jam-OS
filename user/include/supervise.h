/* What a supervisor shares (user/lib/supervise.c; docs/HISTORY.md, M11.6;
 * docs/history/M12-REVIEW-STORAGE.md "The shared supervision module"). A
 * supervisor is a process that starts other processes and starts them
 * again when they end: init (the mixer and the other services), devmgr
 * (the drivers) and, after the storage split, volumes (the filesystem
 * services). Four pieces, each used on its own:
 *
 *   - **the restart window** (struct sup_window): restarts in the last
 *     SUP_WINDOW, the wait before the next, and the death that gives up;
 *   - **the warm spare** (struct sup_spare): one process started with
 *     SR_STANDBY alone, waiting in libos before main, which becomes the
 *     next instance of any service of its program (standby_promote);
 *   - **what outlives an instance** (struct sup_kept): its keeper
 *     (<keep.h>) and its state VMO (<svcstate.h>);
 *   - **a restart measured**: the log line that says when the new
 *     instance ran, and its first answer, asked on a thread of its own.
 *
 * The supervisor stays the owner of what is its own: which service it
 * is, its port keys, what a restart does with the handles it makes
 * (adopting a promoted process into its tables) and its own counting
 * (init counts ends its way and never gives up the mixer).
 *
 * **Log lines** are "<prefix>: <text>" through the sink sup_log gave, one
 * per supervisor process, so the area scripts that grep "init: ..." or
 * "devmgr: ..." see the words they always did.
 *
 * **Threads:** everything here runs on the supervisor's loop thread but
 * the probe of sup_restarted, which touches only its own state (its busy
 * flag is released when it is done). The job helpers are one thread's too. */
#pragma once

#include <keep.h>
#include <os.h>
#include <svcstate.h>

/* ---- the log --------------------------------------------------------------------- */

/* Where a line goes (no newline): `report` lines are the ones the test
 * run reports as RESULTS (devmgr's say(true, ...)). */
typedef void (*sup_say_fn)(bool report, const char *line);
/* Say every line as "<prefix>: <text>" through say. Call it first. */
void sup_log(const char *prefix, sup_say_fn say);

/* ---- the restart window ---------------------------------------------------------- */

#define SUP_BACKOFF_FIRST (100 * NS_PER_MS)   /* the wait after the first death in the window */
#define SUP_BACKOFF_MAX   (5 * NS_PER_S)      /* ... doubling up to this */
#define SUP_WINDOW        (60 * NS_PER_S)
#define SUP_RESTART_LIMIT 5   /* restarts within SUP_WINDOW; the next death gives up */

/* One supervised thing's history. Zero is a fresh one. */
struct sup_window {
    uint64_t restarted[SUP_RESTART_LIMIT];   /* when the last counted restarts were (ring) */
    uint32_t restarts;                       /* since boot, counted or not */
    uint32_t backoff_ms;                     /* the last wait sup_window_end gave */
};

enum sup_end {
    SUP_END_COUNTED,      /* a crash, an error exit, a kill nobody meant to be an exception */
    SUP_END_ON_PURPOSE,   /* killed on purpose (devmgr's KILL, initctl's kill) */
};

/* Restarts counted within SUP_WINDOW before t. */
unsigned sup_window_recent(const struct sup_window *w, uint64_t t);
/* It ended at t. True: restart it at *at (t plus the wait; w->backoff_ms
 * says it in ms). False: give up (the death after SUP_RESTART_LIMIT
 * restarts in the window). The wait is SUP_BACKOFF_FIRST after the first
 * death in the window, doubling with every restart in it, at most
 * SUP_BACKOFF_MAX.
 *
 * `outlives`: a service that outlives its process (its clients wait for
 * the next instance): a death on purpose is never counted and never gives
 * up, and restarts at once; so does the first counted death in the
 * window, and the wait starts at SUP_BACKOFF_FIRST with the second. For
 * anything else every death counts and waits. */
bool sup_window_end(struct sup_window *w, uint64_t t, enum sup_end why, bool outlives,
                    uint64_t *at);
/* It was started again at t. `counted`: it goes into the window (a
 * restart after a death on purpose of a service that outlives its
 * process doesn't). */
void sup_window_started(struct sup_window *w, uint64_t t, bool counted);
/* Forget the window and the last wait (not the restarts since boot). */
void sup_window_reset(struct sup_window *w);

/* ---- the warm spare -------------------------------------------------------------- */

struct sup_limit {
    uint32_t kind;    /* JOB_LIMIT_* */
    uint64_t value;
};

/* What a spare is. Copied by sup_spare_init, but `limits` must stay. */
struct sup_spare_cfg {
    const char             *path;      /* its program ("bin/fat") */
    const char             *name;      /* its process name and argv[0] ("fat-spare");
                                        * NULL: the path's */
    const struct sup_limit *limits;    /* its job's: the limits of the instance it becomes;
                                        * none: the supervisor's job gives them */
    unsigned                nlimits;
    handle_t                port;      /* its SIG_TERMINATED is a packet there ... */
    uint64_t                key;       /* ... under this key (once per spare) */
};

struct sup_spare {
    struct sup_spare_cfg cfg;
    bool     on;         /* spares wanted at all (no boot word `nospare`) */
    bool     armed;      /* sup_spare_arm was called */
    bool     stopped;    /* sup_spare_stop was called */
    handle_t proc, job;  /* while one waits; 0: none */
    handle_t standby;    /* our end of its SR_STANDBY */
    uint64_t started;    /* uptime ns */
    uint64_t next_try;   /* the next may start then (uptime ns) */
    uint64_t backoff;    /* the last wait after a spare ended by itself, ns */
};

/* `on` false: no spare is ever started and sup_spare_ready says no. */
void     sup_spare_init(struct sup_spare *sp, const struct sup_spare_cfg *cfg, bool on);
/* Spares are wanted from now: the first starts at first_at (uptime ns).
 * The first call counts; the later ones do nothing. */
void     sup_spare_arm(struct sup_spare *sp, uint64_t first_at);
/* Start one if none waits and it is time (a failed start backs off,
 * 100 ms doubling to 5 s, and is said). Returns when the supervisor must
 * come back, or DEADLINE_NEVER. */
uint64_t sup_spare_due(struct sup_spare *sp, uint64_t t);
/* The same deadline without starting anything. */
uint64_t sup_spare_next(const struct sup_spare *sp);
/* Its SIG_TERMINATED packet: if it really ended, it is dropped and
 * replaced after a wait that grows while spares end soon after their
 * start. A packet of an earlier spare (promoted or dismissed since) is
 * ignored. */
void     sup_spare_event(struct sup_spare *sp);
/* Does one wait now? */
bool     sup_spare_waits(const struct sup_spare *sp);
/* Does a live one wait? (A dead one found here is said and dropped.) Ask
 * before making the handles a promotion needs. */
bool     sup_spare_ready(struct sup_spare *sp);
/* Promote it with a (consumed whatever happens). OK: *proc and *job are
 * the caller's, the process and its job, no longer on the port; the next
 * spare starts 50 ms later, not at once (a start takes CPU time the
 * promoted instance needs while it sets itself up). Else it couldn't be
 * promoted (said; the spare is gone). ERR_NOT_FOUND: none waits (a is
 * untouched): call sup_spare_ready first. */
status_t sup_spare_promote(struct sup_spare *sp, const struct standby_args *a, handle_t *proc,
                           handle_t *job);
/* The spare goes: its standby channel closed, its job killed. */
void     sup_spare_dismiss(struct sup_spare *sp);
/* The supervisor is stopping: dismiss it, and start no other. */
void     sup_spare_stop(struct sup_spare *sp);

/* ---- what outlives an instance --------------------------------------------------- */

/* A keeper and a state VMO for one supervised thing. */
struct sup_kept {
    struct keeper keeper;       /* its keep channel and what it kept */
    handle_t      state;        /* the state VMO, every right, never mapped; 0: not made */
    uint64_t      state_size;   /* its bytes */
    handle_t      port;         /* where the keeper's channel is bound */
    uint64_t      key;          /* ... under this key, while bound */
    bool          bound;
    uint16_t      gen;          /* bumped with every new keep channel */
    char          name[32];     /* what the log calls it */
};

void sup_kept_init(struct sup_kept *k, handle_t port, uint64_t state_size, const char *name);
/* Add the state VMO (made the first time) and a new keep channel to
 * x[0..n) (r: their rights; room for two more): the new count. Either
 * one that can't be made is said and left out: the instance then starts
 * fresh, as one without them. The dead instance's last puts are taken
 * first. */
unsigned sup_kept_handles(struct sup_kept *k, struct spawn_handle *x, rights_t *r, unsigned n);
/* The generation of the keep channel the last sup_kept_handles made, for
 * the port key a supervisor with many of them puts in sup_kept_hand_over. */
uint16_t sup_kept_gen(const struct sup_kept *k);
/* The instance has its keep channel's end: hand it what was kept, then
 * listen to it on the port under `key`. */
void sup_kept_hand_over(struct sup_kept *k, uint64_t key);
/* Is k listening under the generation gen (a key's)? False: a stale
 * packet. */
bool sup_kept_listening(const struct sup_kept *k, uint16_t gen);
/* The keeper's channel has something to read: take it. */
void sup_kept_event(struct sup_kept *k);
/* Given up on, retired or stopped in order: the keeper closes what it
 * kept (its clients see ERR_PEER_CLOSED) and the state VMO goes. */
void sup_kept_release(struct sup_kept *k);

/* ---- a restart, measured --------------------------------------------------------- */

/* One cheap call on the new instance's channel: OK when it answered. */
typedef status_t (*sup_probe_call)(handle_t ch, uint64_t deadline);
/* Say how long a restart took to get the instance running (at `up`): from
 * the deliberate kill at kill_at (0: there was none), else from the end
 * seen at ended_at; promoted: by the spare. Then call() on ch, on a thread
 * of its own that serves nothing, and say when it answered (5 s at most).
 * One at a time: a restart while a call waits isn't measured. ch is
 * consumed; HANDLE_INVALID measures nothing but the line. */
void sup_restarted(const char *name, bool promoted, uint64_t kill_at, uint64_t ended_at,
                   uint64_t up, handle_t ch, sup_probe_call call);

/* ---- jobs that have ended -------------------------------------------------------- */

/* Is anything still charged to the job of a process that has ended?
 * Said, with who's name, if it is not empty in the end: a job not empty
 * yet is looked at again for 2 s (sup_job_run_due), and only then called
 * not clean. False only if it can't be looked at again (no room). */
bool     sup_job_empty(handle_t job, const char *who);
/* Decide the jobs being looked at again that are due. */
void     sup_job_run_due(void);
uint64_t sup_job_next_deadline(void);   /* DEADLINE_NEVER: nothing to look at again */
/* Wait (bounded) until every job being looked at again is decided; false
 * if any was not empty in the end since the last call. */
bool     sup_job_settle(void);
/* Jobs that were not empty in the end, since the start. */
unsigned sup_job_unclean(void);
