/* init's sessions: /svc/session (abi/idl/session.idl; docs/M13-PLAN.md,
 * "The terminal layer" and "X1 notes"). A session is a Jam OS shell on a
 * connection console of its own, for a remote terminal (tinyssh's): what
 * an SSH login runs in. Only init can give a shell a shell's authority, so
 * init starts them, the way it starts a terminal's (terms.c).
 *
 * The service. /svc/session is a shared channel init publishes, a channel
 * per opener (svc.connect, at most SESSION_OPENERS_MAX at once); an
 * opener's channel speaks `session`. An opener that leaves its answers
 * unread past the protocol's bound is cut off (its channel closed): it
 * broke the protocol, and init's loop never waits for a client.
 *
 * A session (session.open) is, in a job of its own under init's:
 *   console   bin/console "conn" "endidle" cols= rows= (process name
 *             "session-console"), in a job of its own: an ADMIN channel
 *             (init's, closed once the shell has its own), the master
 *             (CONN_MASTER_ROLE: the opener gets the other end), a
 *             `conn_ctl` channel (init keeps the client end) and the root
 *             with RIGHT_ROOT_KLOG alone (the log's lines its shell shows)
 *   shell     bin/shell "session" [TERM=<term>] [cmd=<command>] (process
 *             name "session-shell"), in a job of its own: a SHELL-level
 *             client of that console (SR_CONSOLE), the shell's powers as
 *             every terminal's shell has them (terms_shell_powers) and
 *             init's whole namespace, kept in step as the terminals'
 *             shells' is (sessions_tell)
 * The opener gets the master, the session's `session_ctl` channel (init
 * serves it: the size passed on to the console, the hang-up, the end) and
 * the shell's process with RIGHT_WAIT and RIGHT_INSPECT only (no
 * RIGHT_WRITE, which would let it into the shell: it waits on it and reads
 * its exit code).
 *
 * The end. The shell ends (`exit`, its command done, or its console gone
 * when the master closed or the session was hung up): `ended` with its
 * code goes to the opener, and after HUP_GRACE the shell's job is killed
 * (what it ran in the background; the POSIX programs on the terminal have
 * had the grace to act on their SIGHUP: their terminal hung up). The
 * console ends by itself ("endidle") once nobody is left on it and its
 * output has gone out on the master, so the opener reads everything the
 * shell printed before the master's peer closes; a hang-up kills it at
 * once. A session's slot is free once both have ended and the grace is
 * over. At most SESSION_SESSIONS_MAX sessions whose shell runs; as many
 * more may be ending (an opener that never reads its master keeps its
 * console waiting): a new one past that takes the oldest ending one's
 * place, killing what is left of it.
 *
 * Port keys: KEY_SESSION + K_SHARED (the shared channel), + K_OPENER + i
 * (opener i's channel), + K_SESS + 4 * s + kind (session s's shell end,
 * console end, session_ctl, conn_ctl's answers). A packet of a slot that
 * has been reused since (a ONCE binding fired before its handle closed) is
 * told apart by asking the process itself. */
#include <idl/conn_ctl.h>
#include <idl/console.h>
#include <idl/session.h>
#include <idl/svc.h>
#include <os.h>
#include <termconn.h>
#include "init.h"

#define SLOTS       (2 * SESSION_SESSIONS_MAX)   /* running sessions, and as many ending */
#define OPENERS     SESSION_OPENERS_MAX
#define HUP_GRACE   (2 * NS_PER_S)        /* after the shell's end: then its job is killed */
#define START_WAIT  (2 * NS_PER_S)        /* the new console's answer to console.new_client */
#define ROUND       16                    /* requests served on one channel per packet */
#define SHELL_LEVEL 1                     /* console.new_client's SHELL level */
/* What the opener may do with the shell's process: wait for it, read its
 * exit code, hand it on. Never RIGHT_WRITE (threads in it, a kill). */
#define PROC_RIGHTS (RIGHT_WAIT | RIGHT_INSPECT | RIGHT_TRANSFER)

enum { K_SHARED = 0x00, K_OPENER = 0x10, K_SESS = 0x40 };
enum { KIND_SHELL, KIND_CONSOLE, KIND_CTL, KIND_CONN, KINDS };
_Static_assert(K_OPENER + OPENERS <= K_SESS, "opener keys below the sessions'");
_Static_assert(K_SESS + KINDS * SLOTS <= KEY_SESSION_END - KEY_SESSION, "keys in range");

struct session {
    bool     used;
    bool     shell_gone, console_gone;   /* their ends have come */
    bool     reaped;           /* the shell's job killed (its grace is over) */
    bool     sizing;           /* a conn_ctl.resize is out, unanswered */
    uint16_t cols, rows;       /* the size the opener asked for last */
    uint16_t sent_cols, sent_rows;   /* the size the console was told last */
    uint64_t grace_end;        /* once the shell or the console has ended: when the shell's
                                * job is killed; 0 before */
    handle_t sjob, cjob;       /* the shell's job, the console's job */
    handle_t shell, console;   /* the processes */
    handle_t ctl;              /* session_ctl, our end (0: the opener closed it) */
    handle_t conn;             /* conn_ctl, the client end (0: the console has gone) */
    handle_t ns, back;         /* the shell's namespace channel, for ns_update (0: none) */
};

static handle_t port;
static handle_t shared_srv;             /* /svc/session's shared channel, our end */
static handle_t openers[OPENERS];       /* our ends of the openers' channels (0: free) */
static struct session sess[SLOTS];
static uint32_t conn_txid;              /* conn_ctl.resize's txids */

static uint64_t key_of(unsigned s, unsigned kind)
{
    return KEY_SESSION + K_SESS + KINDS * s + kind;
}

static void close_if(handle_t *h)
{
    if (*h)
        jam_handle_close(*h);
    *h = HANDLE_INVALID;
}

/* ---- a session's end ------------------------------------------------------------- */

/* Kill what is left in job, if anything is: the kernel logs every job
 * kill, and most of a session's jobs are empty by then. */
static void job_end(handle_t job)
{
    struct job_info ji;
    if (job && (jam_job_get_info(job, &ji) != OK || ji.used[JOB_LIMIT_THREADS]))
        jam_job_kill(job);
}

/* Slot s is over: everything closed, the slot free. Its processes have
 * ended, or are killed now. */
static void slot_free(struct session *s)
{
    unsigned i = (unsigned)(s - sess);
    if (s->ctl)
        (void)jam_port_unbind(port, s->ctl, key_of(i, KIND_CTL));
    if (s->conn)
        (void)jam_port_unbind(port, s->conn, key_of(i, KIND_CONN));
    job_end(s->sjob);
    job_end(s->cjob);
    handle_t *hs[] = { &s->sjob, &s->cjob, &s->shell, &s->console, &s->ctl, &s->conn,
                       &s->ns, &s->back };
    for (unsigned k = 0; k < sizeof(hs) / sizeof(hs[0]); k++)
        close_if(hs[k]);
    memset(s, 0, sizeof(*s));
}

/* The grace starts at the first end seen (the shell's or the console's). */
static void ending(struct session *s)
{
    if (!s->grace_end)
        s->grace_end = now() + HUP_GRACE;
}

/* Free s once nothing of it is left to wait for. */
static void maybe_free(struct session *s)
{
    if (s->shell_gone && s->console_gone && s->reaped)
        slot_free(s);
}

/* The shell's process ended: its code to the opener. */
static void shell_ended(struct session *s)
{
    struct process_info info;
    int32_t code = PROCESS_KILLED_CODE;
    if (jam_process_get_info(s->shell, &info) == OK && !info.killed)
        code = (int32_t)info.exit_code;
    s->shell_gone = true;
    if (s->ctl)
        (void)session_ctl_ended(s->ctl, code);   /* a full queue: the opener has its proc */
    close_if(&s->ns);      /* nothing to follow any more */
    close_if(&s->back);
    ending(s);
}

/* A process end's packet for slot s: true if p (the slot's handle) has
 * really ended (not an old packet of a slot used again since). */
static bool really_ended(handle_t p)
{
    signals_t seen = 0;
    return p && jam_object_wait_one(p, SIG_TERMINATED, 0, &seen) == OK;
}

/* Hang s up: its console killed, as if the master had closed. */
static void hang_up(struct session *s)
{
    if (!s->console_gone)
        (void)jam_process_kill(s->console);   /* its end comes to the loop */
    ending(s);
}

/* ---- the size --------------------------------------------------------------------- */

/* The newest size to the console, unless one is out or it has it. */
static void push_size(struct session *s)
{
    if (!s->conn || s->sizing || (s->cols == s->sent_cols && s->rows == s->sent_rows))
        return;
    status_t st = conn_ctl_resize_send(s->conn, idl_txid_next(&conn_txid), s->cols, s->rows);
    if (st != OK)
        return;   /* full or gone: the next resize, or the console's end, settles it */
    s->sizing = true;
    s->sent_cols = s->cols;
    s->sent_rows = s->rows;
}

/* conn_ctl's answers (or the console gone). */
static void conn_answers(struct session *s)
{
    for (unsigned n = 0; s->conn && n < ROUND; n++) {
        _Alignas(8) uint8_t b[CONN_CTL_REP_MAX];
        struct idl_msg m;
        status_t st = idl_reply_read(s->conn, b, sizeof(b), &m);
        if (st == ERR_SHOULD_WAIT)
            break;
        if (st != OK) {   /* the console has gone */
            (void)jam_port_unbind(port, s->conn, key_of((unsigned)(s - sess), KIND_CONN));
            close_if(&s->conn);
            return;
        }
        idl_msg_drop(&m);   /* an answer: its status says nothing we act on */
        s->sizing = false;
    }
    push_size(s);
}

/* ---- session_ctl ------------------------------------------------------------------ */

static status_t op_resize(void *ctx, uint16_t cols, uint16_t rows)
{
    struct session *s = ctx;
    if (!cols || !rows)
        return ERR_INVALID_ARGS;
    s->cols = cols < CONN_COLS_MAX ? cols : CONN_COLS_MAX;
    s->rows = rows < CONN_ROWS_MAX ? rows : CONN_ROWS_MAX;
    push_size(s);
    return OK;
}

static status_t op_hangup(void *ctx)
{
    hang_up(ctx);
    return OK;
}

/* kill(2) of the session (libc's): SIGHUP, SIGKILL and SIGTERM hang up. */
static status_t op_signal(void *ctx, uint8_t signo)
{
    struct session *s = ctx;
    if (!signo)
        return s->shell_gone ? ERR_BAD_STATE : OK;
    if (signo != 1 && signo != 9 && signo != 15)
        return ERR_NOT_SUPPORTED;
    hang_up(s);
    return OK;
}

static const struct session_ctl_ops ctl_ops = {
    .resize = op_resize, .hangup = op_hangup, .signal = op_signal,
};

/* What the opener sent on the session's control channel. */
static void ctl_requests(struct session *s)
{
    unsigned i = (unsigned)(s - sess);
    status_t st = OK;
    unsigned n = 0;
    for (; st == OK && s->ctl && n < ROUND; n++)
        st = session_ctl_unread_full(s->ctl) ? ERR_BAD_STATE
                                             : session_ctl_serve_one(s->ctl, &ctl_ops, s);
    if (st == OK && s->ctl) {   /* more may be queued: the binding fires on an edge */
        struct port_packet pkt = { .key = key_of(i, KIND_CTL), .type = PORT_PACKET_USER };
        (void)jam_port_queue(port, &pkt);   /* a full port: served with its next request */
    } else if (st != ERR_SHOULD_WAIT && s->ctl) {
        /* The opener closed it, or left its answers unread: the session
         * goes on without it (the size stays, no `ended`). */
        (void)jam_port_unbind(port, s->ctl, key_of(i, KIND_CTL));
        close_if(&s->ctl);
    }
}

/* ---- starting one ----------------------------------------------------------------- */

/* The handles a start makes, ours until they are handed on. */
struct start {
    handle_t admin, admin_c;     /* the console's ADMIN channel: ours, its */
    handle_t master, master_c;   /* the master: the opener's, the console's */
    handle_t conn, conn_c;       /* conn_ctl: ours, the console's */
    handle_t ctl, ctl_c;         /* session_ctl: ours, the opener's */
};

static void start_drop(struct start *a)
{
    handle_t *hs[] = { &a->admin, &a->admin_c, &a->master, &a->master_c, &a->conn, &a->conn_c,
                       &a->ctl, &a->ctl_c };
    for (unsigned k = 0; k < sizeof(hs) / sizeof(hs[0]); k++)
        close_if(hs[k]);
}

static status_t start_channels(struct start *a)
{
    status_t st = jam_channel_create(&a->admin, &a->admin_c);
    if (st == OK)
        st = jam_channel_create(&a->master_c, &a->master);
    if (st == OK)
        st = jam_channel_create(&a->conn, &a->conn_c);
    if (st == OK)
        st = jam_channel_create(&a->ctl, &a->ctl_c);
    return st;
}

/* A job of its own for one of the session's processes. */
static status_t new_job(handle_t *out)
{
    return jam_job_create(startup_handle(SR_JOB), 0, out);
}

/* The console, with the channels' console ends (moved). */
static status_t start_console(struct session *s, struct start *a)
{
    char cols[12], rows[12];
    snprintf(cols, sizeof(cols), "cols=%u", s->cols);
    snprintf(rows, sizeof(rows), "rows=%u", s->rows);
    const char *argv[] = { "console", "conn", "endidle", cols, rows };
    struct spawn_handle x[4] = { { SR_USER + 0, a->admin_c }, { CONN_MASTER_ROLE, a->master_c },
                                 { CONN_CTL_ROLE, a->conn_c } };
    unsigned nx = 3;
    x[nx] = (struct spawn_handle){ SR_RESOURCE,
                                   services_root_with(RIGHTS_BASIC | RIGHT_ROOT_KLOG) };
    nx += x[nx].h != HANDLE_INVALID;   /* without it: no log lines for the shell */
    a->admin_c = a->master_c = a->conn_c = HANDLE_INVALID;   /* spawn moves them */
    status_t st = new_job(&s->cjob);
    struct spawn_args sa = { .path = "bin/console", .name = "session-console", .argc = 5,
                             .argv = argv, .job = s->cjob, .extra = x, .nextra = nx };
    if (st == OK)
        return spawn(&sa, &s->console);
    for (unsigned k = 0; k < nx; k++)
        jam_handle_close(x[k].h);
    return st;
}

/* The shell, on a SHELL-level client of the console. */
static status_t start_shell(struct session *s, struct start *a, const char *term, uint32_t tn,
                            const char *cmd, uint32_t cn)
{
    struct spawn_handle x[3] = { { SR_CONSOLE, HANDLE_INVALID } };
    status_t st = console_new_client_until(a->admin, now() + START_WAIT, SHELL_LEVEL, &x[0].h);
    close_if(&a->admin);   /* the shell's client is enough: "endidle" counts it */
    if (st == OK)
        st = new_job(&s->sjob);
    if (st != OK) {
        close_if(&x[0].h);
        return st;
    }
    unsigned nx = 1 + terms_shell_powers(&x[1]);
    char targ[5 + SESSION_TERM_MAX + 1], carg[4 + SESSION_COMMAND_RUN_MAX + 1];
    snprintf(targ, sizeof(targ), "TERM=%.*s", (int)tn, term);
    snprintf(carg, sizeof(carg), "cmd=%.*s", (int)cn, cmd);
    const char *argv[5] = { "bin/shell", "session" };
    int argc = 2;
    if (tn)
        argv[argc++] = targ;
    if (cn)
        argv[argc++] = carg;
    if (init_runlines)
        argv[argc++] = "runlines";
    struct spawn_args sa = { .path = "bin/shell", .name = "session-shell", .argc = argc,
                             .argv = argv, .job = s->sjob, .extra = x, .nextra = nx,
                             .ns = NS_ALL, .ns_out = &s->ns, .ns_back_out = &s->back };
    return spawn(&sa, &s->shell);
}

/* Its processes' ends and its two channels on the loop's port. */
static status_t watch(struct session *s, struct start *a)
{
    unsigned i = (unsigned)(s - sess);
    status_t st = jam_port_bind(port, s->shell, key_of(i, KIND_SHELL), SIG_TERMINATED,
                                PORT_BIND_ONCE);
    if (st == OK)
        st = jam_port_bind(port, s->console, key_of(i, KIND_CONSOLE), SIG_TERMINATED,
                           PORT_BIND_ONCE);
    s->ctl = a->ctl;
    s->conn = a->conn;
    a->ctl = a->conn = HANDLE_INVALID;
    if (st == OK)
        st = jam_port_bind(port, s->ctl, key_of(i, KIND_CTL), SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st == OK)
        st = jam_port_bind(port, s->conn, key_of(i, KIND_CONN), SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    return st;
}

/* ---- session.open ----------------------------------------------------------------- */

static bool term_ok(const char *t, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        char c = t[i];
        bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum && c != '.' && c != '_' && c != '+' && c != '-')
            return false;
    }
    return true;
}

static bool command_ok(const char *c, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if ((uint8_t)c[i] < 0x20 || c[i] == 0x7f)
            return false;
    return true;
}

/* Sessions whose shell runs and that nobody has hung up. */
static unsigned sessions_running(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < SLOTS; i++)
        n += sess[i].used && !sess[i].grace_end;
    return n;
}

/* A free slot: one not in use, or the oldest ending session's, whatever
 * is left of it killed (there is one: at most SESSIONS_MAX run, and
 * SLOTS is twice that). NULL if SESSIONS_MAX run. */
static struct session *slot_new(void)
{
    if (sessions_running() >= SESSION_SESSIONS_MAX)
        return NULL;
    struct session *oldest = NULL;
    for (unsigned i = 0; i < SLOTS; i++) {
        struct session *s = &sess[i];
        if (!s->used)
            return s;
        if (s->grace_end && (!oldest || s->grace_end < oldest->grace_end))
            oldest = s;
    }
    if (oldest)
        slot_free(oldest);
    return oldest;
}

static status_t op_open(void *ctx, uint16_t cols, uint16_t rows, const char *term, uint32_t tn,
                        const char *cmd, uint32_t cn, handle_t *out_master, handle_t *out_ctl,
                        handle_t *out_proc)
{
    (void)ctx;
    if (!cols || !rows || !term_ok(term, tn) || !command_ok(cmd, cn))
        return ERR_INVALID_ARGS;
    if (cn > SESSION_COMMAND_RUN_MAX)
        return ERR_OUT_OF_RANGE;
    struct session *s = slot_new();
    if (!s)
        return ERR_NO_RESOURCES;
    s->used = true;
    s->cols = s->sent_cols = cols < CONN_COLS_MAX ? cols : CONN_COLS_MAX;
    s->rows = s->sent_rows = rows < CONN_ROWS_MAX ? rows : CONN_ROWS_MAX;
    struct start a = { 0 };
    handle_t proc = HANDLE_INVALID;
    status_t st = start_channels(&a);
    if (st == OK)
        st = start_console(s, &a);
    if (st == OK)
        st = start_shell(s, &a, term, tn, cmd, cn);
    if (st == OK)
        st = watch(s, &a);
    if (st == OK)
        st = jam_handle_duplicate(s->shell, PROC_RIGHTS, &proc);
    if (st != OK) {
        printf("init: a session didn't start (%s)\n", status_str(st));
        start_drop(&a);
        slot_free(s);   /* its jobs killed */
        return st == ERR_NO_MEMORY ? st : ERR_NO_RESOURCES;
    }
    *out_master = a.master;
    *out_ctl = a.ctl_c;
    *out_proc = proc;
    a.master = a.ctl_c = HANDLE_INVALID;
    start_drop(&a);
    return OK;
}

static const struct session_ops open_ops = { .open = op_open };

/* ---- the shared channel and the openers ----------------------------------------- */

static void opener_close(unsigned i)
{
    (void)jam_port_unbind(port, openers[i], KEY_SESSION + K_OPENER + i);
    close_if(&openers[i]);
}

static status_t on_connect(void *ctx, handle_t *out)
{
    (void)ctx;
    unsigned i = 0;
    while (i < OPENERS && openers[i])
        i++;
    if (i == OPENERS)
        return ERR_NO_RESOURCES;
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    st = jam_port_bind(port, mine, KEY_SESSION + K_OPENER + i, SIG_READABLE | SIG_PEER_CLOSED,
                       PORT_BIND_PERSISTENT);
    if (st != OK) {
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    openers[i] = mine;
    *out = theirs;
    return OK;
}

/* The shared channel speaks only svc.connect. */
static uint32_t svc_only(void *ctx, const void *req, uint32_t n, void *rep, handle_t *rhs,
                         uint32_t *rhn)
{
    static const struct svc_ops none;
    return svc_dispatch(&none, ctx, req, n, rep, rhs, rhn);
}

/* A round of the shared channel's requests (svc.connect). */
static void shared_requests(void)
{
    status_t st = OK;
    for (unsigned n = 0; st == OK && n < ROUND; n++)
        st = svc_serve_request(shared_srv, svc_only, on_connect, NULL);
    if (st == OK) {
        struct port_packet pkt = { .key = KEY_SESSION + K_SHARED, .type = PORT_PACKET_USER };
        (void)jam_port_queue(port, &pkt);   /* a full port: served with its next request */
    }
}

static void opener_requests(unsigned i)
{
    status_t st = OK;
    for (unsigned n = 0; st == OK && openers[i] && n < ROUND; n++)
        st = session_unread_full(openers[i]) ? ERR_BAD_STATE
                                             : session_serve_one(openers[i], &open_ops, NULL);
    if (st == OK && openers[i]) {
        struct port_packet pkt = { .key = KEY_SESSION + K_OPENER + i, .type = PORT_PACKET_USER };
        (void)jam_port_queue(port, &pkt);
    } else if (st != ERR_SHOULD_WAIT && openers[i]) {
        opener_close(i);   /* it closed its end, or left its answers unread */
    }
}

/* ---- the loop's side ---------------------------------------------------------------- */

static void session_event(unsigned i, unsigned kind)
{
    struct session *s = &sess[i];
    if (!s->used)
        return;   /* an old packet of a slot freed since */
    if (kind == KIND_SHELL && !s->shell_gone && really_ended(s->shell))
        shell_ended(s);
    else if (kind == KIND_CONSOLE && !s->console_gone && really_ended(s->console)) {
        s->console_gone = true;
        ending(s);
    } else if (kind == KIND_CTL) {
        ctl_requests(s);
    } else if (kind == KIND_CONN) {
        conn_answers(s);
    }
    maybe_free(s);
}

void sessions_event(unsigned k)
{
    if (k == K_SHARED && shared_srv)
        shared_requests();
    else if (k >= K_OPENER && k < K_OPENER + OPENERS && openers[k - K_OPENER])
        opener_requests(k - K_OPENER);
    else if (k >= K_SESS && k < K_SESS + KINDS * SLOTS)
        session_event((k - K_SESS) / KINDS, (k - K_SESS) % KINDS);
}

uint64_t sessions_due(uint64_t t)
{
    uint64_t next = DEADLINE_NEVER;
    for (unsigned i = 0; i < SLOTS; i++) {
        struct session *s = &sess[i];
        if (!s->used || !s->grace_end || s->reaped)
            continue;
        if (t < s->grace_end) {
            next = s->grace_end < next ? s->grace_end : next;
            continue;
        }
        job_end(s->sjob);   /* what the shell left running */
        s->reaped = true;
        if (!s->shell_gone) {    /* a shell stuck past its console's end: it ends now */
            s->shell_gone = true;
            if (s->ctl)
                (void)session_ctl_ended(s->ctl, PROCESS_KILLED_CODE);
        }
        maybe_free(s);
    }
    return next;
}

void sessions_tell(void)
{
    for (unsigned i = 0; i < SLOTS; i++) {
        struct session *s = &sess[i];
        if (!s->used || !s->ns)
            continue;
        status_t st = ns_update(s->ns, s->back, NS_ALL);
        if (st != OK && st != ERR_PEER_CLOSED)   /* the next change sends it all again */
            printf("init: a session's shell didn't get the new mounts (%s)\n", status_str(st));
    }
}

void sessions_init(handle_t loop_port)
{
    port = loop_port;
    handle_t cli;
    status_t st = jam_channel_create(&cli, &shared_srv);
    if (st == OK)
        st = jam_port_bind(port, shared_srv, KEY_SESSION + K_SHARED, SIG_READABLE,
                           PORT_BIND_PERSISTENT);
    if (st != OK) {
        printf("init: no /svc/session: no sessions this boot (%s)\n", status_str(st));
        if (shared_srv) {
            jam_handle_close(cli);
            close_if(&shared_srv);
        }
        return;
    }
    services_publish(SVC_SESSION, cli, true);   /* a channel per opener (svc.connect) */
    shared_watch(SVC_SESSION, cli);   /* answers a holder never reads: dropped (kept for good) */
}
