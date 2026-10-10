/* The signal thread (docs/M13-PLAN.md section 6): one per POSIX process,
 * started before the program's constructors, waiting on one port for
 * everything that sends the process a signal:
 *   - the starter's signal channel (SR_SIGNALS, the `signal` protocol):
 *     kill and sigqueue from the parent, the native shell's Ctrl+C;
 *   - the children's ends (proc.c binds each child to this port; their
 *     packets go to lx_child_packet, which calls sig_child_ended: SIGCHLD);
 *   - the process's terminal's events (tty.h): a resize is SIGWINCH, ISIG's
 *     keys with no shell below are SIGINT and SIGQUIT;
 *   - the timers (sigtimer.c): SIGALRM and the POSIX timers' signals.
 * Each signal is sent to the process (signal.c's sig_send_process: its
 * default action at once, or routed to a thread). While execve waits for
 * the program that replaced us, every signal goes on to it instead.
 *
 * The thread is libjam's (thread_spawn): no thread-local storage, so it
 * never makes a call through musl, never runs a handler, and the call
 * boundary leaves it alone (sig_on_signal_thread: its stack is ours,
 * static). Nothing interrupts it. */
#include <errno.h>
#include <idl/signal.h>
#include <signal.h>
#include <sys/wait.h>
#include "proc.h"
#include "sig.h"
#include "tty.h"

#define STACK_SIZE   (64u << 10)
#define KEY_CHANNEL  (SIG_KEY_OURS | 1)          /* the starter's signal channel */
#define KEY_TTY      (SIG_KEY_OURS | 2)          /* the terminal's events */
#define KEY_WAKE     (SIG_KEY_OURS | 3)          /* look for a terminal again */
#define KEY_TIMER    (SIG_KEY_OURS | (1ull << 32))   /* | the timer's index */
#define SEND_WAIT_NS NS_PER_S                    /* how long a send waits for room */

static uint8_t stack[STACK_SIZE] __attribute__((aligned(16)));
static handle_t port;                    /* made at the start, never closed */
static handle_t channel;                 /* SR_SIGNALS, or 0 */
static struct fd_desc *tty;              /* the terminal listened to (a reference kept) */

bool sig_on_signal_thread(void)
{
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    return sp - (uintptr_t)stack < sizeof(stack);
}

handle_t sig_port(void)
{
    return port;
}

/* ---- sending on a child's channel -------------------------------------------------- */

long sig_channel_send(handle_t ch, int signo, int code, int32_t value)
{
    uint8_t c = code == SI_USER ? SIGNAL_CODE_USER : SIGNAL_CODE_QUEUE;
    uint64_t deadline = now() + SEND_WAIT_NS;
    for (;;) {
        status_t st = signal_send(ch, (uint8_t)signo, c, value);
        if (st == OK || st == ERR_PEER_CLOSED)
            return 0;   /* sent, or the program has ended: nothing to signal */
        if (st != ERR_SHOULD_WAIT)
            return lx_err(st);
        signals_t seen;
        st = jam_object_wait_one(ch, SIG_WRITABLE | SIG_PEER_CLOSED, deadline, &seen);
        if (st == ERR_TIMED_OUT)
            return -EAGAIN;   /* the program is stuck: its queue stays full */
        if (st != OK && st != ERR_INTERRUPTED)
            return lx_err(st);
    }
}

/* ---- a signal comes in ------------------------------------------------------------- */

/* Signal si to the process, or on to the program execve waits for. */
static void arrived(const struct sig_info *si)
{
    int32_t child = lx_exec_child();
    if (child)
        (void)lx_child_signal(child, 0, si->signo, si->code, (int32_t)si->value);
    else
        sig_send_process(si);
}

static void on_send(void *ctx, uint8_t signo, uint8_t code, int32_t value)
{
    if (!signo || signo > SIG_MAX || signo == SIGKILL || code > SIGNAL_CODE_QUEUE)
        return;   /* nothing a well-behaved starter sends */
    struct sig_info si = { .signo = signo, .code = code == SIGNAL_CODE_QUEUE ? SI_QUEUE : SI_USER,
                           .pid = (int32_t)lx_getppid(0, 0, 0, 0, 0, 0), .value = value };
    arrived(&si);
}

/* Everything queued on the starter's channel. */
static void take_channel(void)
{
    static const struct signal_ops ops = { .send = on_send };
    for (unsigned guard = 0; guard < CHANNEL_MAX_QUEUED; guard++)
        if (signal_serve_one(channel, &ops, NULL) != OK)
            break;   /* ERR_SHOULD_WAIT (drained), or the starter has gone */
}

/* The terminal's queued events, as signals. */
static void take_tty(void)
{
    uint64_t sigs = tty_take_signals(tty);
    for (int s = 1; s < SIG_MAX; s++)
        if (sigs & (1ull << s)) {   /* tty.h's bits are 1 << signo (SIGWINCH, SIGINT, SIGQUIT) */
            struct sig_info si = { .signo = s, .code = SI_KERNEL };
            arrived(&si);
        }
}

/* Listen to the process's terminal, once it has one. */
static void bind_tty(void)
{
    if (tty)
        return;
    struct fd_desc *d = tty_controlling();
    if (!d)
        return;
    handle_t h = tty_event_handle(d);
    if (h && jam_port_bind(port, h, KEY_TTY, SIG_READABLE, PORT_BIND_PERSISTENT) == OK) {
        tty = d;   /* kept for good: the process's terminal is held for its life anyway */
        take_tty();   /* what came before the binding */
        return;
    }
    fd_put(d);
}

void sig_tty_attached(void)
{
    struct port_packet p = { .key = KEY_WAKE };
    if (port)
        (void)jam_port_queue(port, &p);   /* a full port: the next wake will do */
}

bool sig_child_ended(int32_t pid, int wstatus)
{
    struct sig_info si = {
        .signo = SIGCHLD, .pid = pid,
        .code = WIFEXITED(wstatus) ? CLD_EXITED : CLD_KILLED,
        .status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : WTERMSIG(wstatus),
    };
    bool reap = sig_reaps_children();
    sig_send_process(&si);   /* dropped at once if SIGCHLD is ignored */
    return reap;
}

status_t sig_timer_bind(handle_t h, unsigned index)
{
    return jam_port_bind(port, h, KEY_TIMER | index, SIG_SIGNALED, PORT_BIND_PERSISTENT);
}

/* ---- the thread -------------------------------------------------------------------- */

static void on_packet(const struct port_packet *p)
{
    if (!(p->key & SIG_KEY_OURS))
        lx_child_packet(p);
    else if (p->key == KEY_CHANNEL)
        take_channel();
    else if (p->key == KEY_TTY)
        take_tty();
    else if (p->key == KEY_WAKE)
        bind_tty();
    else if ((p->key & KEY_TIMER) == KEY_TIMER)
        sig_timer_fired((unsigned)(p->key & 0xffffffffu));
}

static void loop(void *arg)
{
    (void)arg;
    for (;;) {
        struct port_packet p;
        if (jam_port_wait(port, DEADLINE_NEVER, &p) == OK)
            on_packet(&p);
    }
}

/* "posix: signals: <what> (<status>)" in the log and the end: a process
 * that can't receive signals or see its children end can't run. */
_Noreturn static void fail(const char *what, status_t st)
{
    const char *parts[] = { "posix: signals: ", what, " (", status_str(st), ")\n" };
    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
        (void)jam_debug_write(parts[i], strlen(parts[i]));   /* the log: nothing to do if not */
    jam_process_exit(127);
}

/* After the descriptor table (fd.c's start, 101), before the program's
 * constructors: the main thread listed with the hand-over's mask, the
 * ignored signals, the port and the thread. */
__attribute__((constructor(102))) static void sig_start(void)
{
    const char *cwd;
    const struct posixfds_state *ho = lx_handover(&cwd);
    sig_start_state(ho ? ho->sigignore : 0);
    sig_thread_begin(startup_handle(SR_SELF_THREAD), (uint32_t)lx_gettid(0, 0, 0, 0, 0, 0),
                     ho ? ho->sigmask : 0);
    status_t st = jam_port_create(&port);
    if (st != OK)
        fail("no port", st);
    channel = startup_handle(SR_SIGNALS);
    if (channel) {
        st = jam_port_bind(port, channel, KEY_CHANNEL, SIG_READABLE, PORT_BIND_PERSISTENT);
        if (st != OK)
            fail("the signal channel can't be bound", st);
    }
    bind_tty();
    handle_t t;
    st = thread_spawn("signals", loop, NULL, stack, sizeof(stack), &t);
    if (st != OK)
        fail("no signal thread", st);
    jam_handle_close(t);
    if (channel)
        take_channel();   /* sent before the binding */
}
