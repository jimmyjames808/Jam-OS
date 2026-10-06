/* init: the boot before this one, if it panicked (<crashlog.h>).
 *
 * After a panic the kernel starts a fresh copy of itself (kernel/kexec/),
 * whose boot looks like any other, with one difference: the kernel hands
 * init the panicked kernel's log (SR_CRASHLOG, a read-only VMO). init
 * passes it to logd, which saves it as /data/logs/<name>-crash.txt before
 * it opens this boot's own log, and answers on a channel of init's. The
 * boot's first shell waits for that answer (behind the splash, which takes
 * longer anyway) and prints one line:
 *     the last boot panicked: <message> (saved as /data/logs/boot-0024-crash.txt).
 *     Code JAM-PF-0008: crashlog shows the details
 * or why it was not saved; the code is the one the kernel's panic screen
 * showed (<crashinfo.h> finds it in the log). On the desktop, once that
 * shell is up, a notice says so too: "Jam OS restarted after a problem" /
 * "Everything is back. Code ...", with Details (a new terminal running the
 * shell's `crashlog`, which shows the saved report) and Close; Close only,
 * if the log wasn't saved. No logd given the log within DATA_WAIT of
 * init's start (no /data: the stick is not there), or no answer within
 * SAVE_WAIT once one was: the shell starts, the line says so, and the log
 * is let go (a later /data doesn't get it).
 * A logd that dies before it answers is started again by shell.c and
 * gets the log again. */
#include <crashinfo.h>
#include <crashlog.h>
#include <os.h>
#include "init.h"

#define DATA_WAIT  (20 * NS_PER_S)   /* /data, and logd with the log, from init's start */
#define SAVE_WAIT  (60 * NS_PER_S)   /* logd's answer, from init's start */
#define BANNER_MAX 256
#define CODE_SCAN  (16u << 10)       /* the log searched for the code, from the panic on */

enum { NONE, WAITING, DONE };

static int      state = NONE;
static handle_t log;            /* SR_CRASHLOG, until DONE */
static handle_t answer;         /* our end of the newest logd's result channel (0: none) */
static bool     given;          /* a logd was given the log: SAVE_WAIT from then on */
static handle_t port;
static uint64_t key;
static uint64_t started;        /* uptime ns */
static struct crashlog_header head;
static char     banner[BANNER_MAX];
static bool     banner_given;
static char     code[CRASH_CODE_MAX];   /* the panic's code, "" if its log has none */
static bool     saved;          /* logd saved the log (crashlog has it) */
static bool     noticed;        /* the desktop's notice was posted */

static void drop(handle_t *h)
{
    if (*h)
        jam_handle_close(*h);
    *h = HANDLE_INVALID;
}

/* The result is known: the banner, logged too; the log let go. */
static void done(const char *saved_or_why)
{
    int n = snprintf(banner, sizeof(banner), "the last boot panicked: %s (%s)",
                     head.message[0] ? head.message : "no message", saved_or_why);
    if (code[0] && n > 0 && (size_t)n < sizeof(banner))
        snprintf(banner + n, sizeof(banner) - (size_t)n, ". Code %s%s", code,
                 saved ? ": crashlog shows the details" : "");
    printf("init: %s\n", banner);
    state = DONE;
    drop(&log);
    drop(&answer);
}

/* The panic's code, from its lines in the log (they start at panic_at). */
static void find_code(uint64_t vmo_size)
{
    static char text[CODE_SCAN];
    uint64_t from = sizeof(head) + head.panic_at, n = head.text_len - head.panic_at;
    if (n > sizeof(text))
        n = sizeof(text);
    if (from > vmo_size || jam_vmo_read(log, from, text, (size_t)n) != OK)
        return;
    crash_code(text, (size_t)n, code);
}

void lastboot_init(handle_t to_port, uint64_t to_key)
{
    log = startup_handle(SR_CRASHLOG);
    if (!log)
        return;
    port = to_port;
    key = to_key;
    started = now();
    uint64_t size = 0;
    if (jam_vmo_get_size(log, &size) != OK || size < sizeof(head) ||
        jam_vmo_read(log, 0, &head, sizeof(head)) != OK || !crashlog_header_ok(&head, size)) {
        memset(&head, 0, sizeof(head));
        done("its log was not saved: the kernel's copy of it can't be read");
        return;
    }
    state = WAITING;
    find_code(size);
    printf("init: the last boot panicked%s%s: logd saves its log once /data is there\n",
           code[0] ? ", code " : "", code);
}

unsigned lastboot_logd_handles(struct spawn_handle *x)
{
    if (state != WAITING)
        return 0;
    handle_t dup = HANDLE_INVALID, mine = HANDLE_INVALID, theirs = HANDLE_INVALID;
    status_t st = jam_handle_duplicate(log, RIGHTS_BASIC | RIGHT_READ, &dup);
    if (st == OK)
        st = jam_channel_create(&mine, &theirs);
    if (st == OK)
        st = jam_port_bind(port, mine, key, SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_ONCE);
    if (st != OK) {
        printf("init: logd can't be given the last boot's log (%s)\n", status_str(st));
        drop(&dup);
        drop(&mine);
        drop(&theirs);
        return 0;
    }
    drop(&answer);   /* an earlier logd's, which died before it answered */
    answer = mine;
    given = true;
    x[0] = (struct spawn_handle){ SR_CRASHLOG, dup };
    x[1] = (struct spawn_handle){ LOGD_SR_CRASH_RESULT, theirs };
    return 2;
}

void lastboot_event(void)
{
    if (state != WAITING || !answer)
        return;
    struct crashlog_result r;
    uint32_t n = 0, nh = 0;
    struct channel_read_args a = {
        .h = answer, .bytes_cap = sizeof(r), .bytes = (uint64_t)(uintptr_t)&r,
        .actual_bytes = (uint64_t)(uintptr_t)&n, .actual_handles = (uint64_t)(uintptr_t)&nh,
    };
    status_t st = jam_channel_read(&a);
    if (st == ERR_SHOULD_WAIT) {
        (void)jam_port_bind(port, answer, key, SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_ONCE);
        return;
    }
    if (st != OK || n != sizeof(r)) {   /* logd ended without a word: the next one tries */
        drop(&answer);
        return;
    }
    char why[160];
    r.path[sizeof(r.path) - 1] = '\0';
    saved = r.status == OK;
    if (r.status == OK)
        snprintf(why, sizeof(why), "saved as %s", r.path);
    else
        snprintf(why, sizeof(why), "not saved: %s%s%s", r.path[0] ? r.path : "",
                 r.path[0] ? ": " : "", status_str(r.status));
    done(why);
}

uint64_t lastboot_wait_until(uint64_t t)
{
    if (state != WAITING)
        return DEADLINE_NEVER;
    uint64_t until = started + (given ? SAVE_WAIT : DATA_WAIT);
    if (t < until)
        return until;
    done(given ? "not saved: logd didn't finish within 60 s"
               : "not saved: no /data within 20 s; is the Jam OS stick in?");
    return DEADLINE_NEVER;
}

void lastboot_shell_ready(void)
{
    if (state != DONE || noticed || !comp_up())
        return;   /* no panic before this boot, or nowhere to show it */
    noticed = true;
    char body[96];
    snprintf(body, sizeof(body), "Everything is back.%s%s", code[0] ? " Code " : "", code);
    static const char *const details[] = { "Details", "Close", NULL };
    static const char *const close[] = { "Close", NULL };
    if (comp_notice_run("Jam OS restarted after a problem", body, 'J', 1, saved ? details : close,
                        saved ? "crashlog" : NULL))
        printf("init: notice \"Jam OS restarted after a problem\" (%s)%s\n", body,
               saved ? ": Details runs crashlog" : "");
}

const char *lastboot_banner(void)
{
    if (state != DONE || banner_given)
        return "";
    banner_given = true;
    return banner;
}
