/* Output, pipes, input and Ctrl+C for the commands.
 *
 * Pipes: every stage runs in turn in this process (sh_exec.c). While a
 * stage runs, everything it prints through sh_put / sh_say is appended to
 * a memory buffer instead of the screen; the next stage gets that buffer
 * as its input (sh_stdin), and the last stage prints to wherever the whole
 * pipeline prints. So `dmesg | grep usb | tail -3` works with any builtin,
 * at the cost of each stage finishing before the next starts (fine for
 * text this size; a pipe holds at most 4 MiB). A program started with
 * `run` inside a pipe gets an SR_STDOUT channel, which libos's printf
 * writes to; the shell copies what arrives into the pipe (sh_program.c).
 * Kernel commands (ktest, bench, stress, kill) print into the kernel log,
 * so their text can't be piped. */
#include "sh_core.h"

#define TYPEAHEAD 128   /* keys kept for the next line; more are dropped */

static struct sh_stdio io;   /* out NULL: the screen */
static bool interrupted;
/* Keys typed while a command ran, oldest first: a ring. */
static struct input_key_event ahead[TYPEAHEAD];
static unsigned ahead_first, ahead_n;

/* ---- buffers and where output goes --------------------------------------------------- */

void sh_buf_add(struct sh_buf *b, const char *s, size_t n)
{
    if (b->n + n > SH_PIPE_MAX) {
        b->full = true;
        n = SH_PIPE_MAX - b->n;
    }
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n)
            cap *= 2;
        char *p = malloc(cap);
        if (!p) {
            b->full = true;
            return;
        }
        if (b->n)
            memcpy(p, b->p, b->n);
        free(b->p);
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

struct sh_stdio sh_stdio_get(void)
{
    return io;
}

void sh_stdio_set(struct sh_stdio to)
{
    io = to;
}

bool sh_piped(void)
{
    return io.out != NULL;
}

void sh_put(const char *s, size_t n)
{
    if (io.out)
        sh_buf_add(io.out, s, n);
    else
        sh_console_write(s, n);
}

static void vsay(const char *fmt, va_list ap)
{
    char buf[1024];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n >= (int)sizeof(buf)) {
        char *big = malloc((size_t)n + 1);
        if (big) {
            vsnprintf(big, (size_t)n + 1, fmt, ap2);
            sh_put(big, (size_t)n);
            free(big);
            n = 0;
        } else {
            n = sizeof(buf) - 1;
        }
    }
    va_end(ap2);
    if (n > 0)
        sh_put(buf, (size_t)n);
}

void sh_say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsay(fmt, ap);
    va_end(ap);
}

void sh_tty(const char *fmt, ...)
{
    struct sh_buf *saved = io.out;
    io.out = NULL;
    va_list ap;
    va_start(ap, fmt);
    vsay(fmt, ap);
    va_end(ap);
    io.out = saved;
}

/* ---- input ---------------------------------------------------------------------------- */

bool sh_stdin(const char **data, size_t *len)
{
    if (!io.have_in)
        return false;
    *data = io.in ? io.in : "";
    *len = io.in_len;
    return true;
}

bool sh_input(const char *who, int argc, char **argv, int i, const char **data, size_t *len)
{
    if (i < argc) {
        char abs[SH_PATH_MAX];
        const void *d;
        uint64_t n;
        bool dir = false;
        status_t st = sh_resolve(argv[i], abs, sizeof(abs)) ? sh_stat(abs, &dir, &n)
                                                            : ERR_NOT_FOUND;
        if (st == ERR_NOT_FOUND) {
            sh_tty("%s: %s: no such file\n", who, argv[i]);
            return false;
        }
        if (st == OK && dir) {
            sh_tty("%s: %s: is a directory\n", who, argv[i]);
            return false;
        }
        if (st == OK)
            st = sh_read(abs, &d, &n);
        if (st != OK) {
            sh_tty("%s: %s: can't read it (%s)\n", who, argv[i], sh_why(st));
            return false;
        }
        *data = d;
        *len = (size_t)n;
        return true;
    }
    if (sh_stdin(data, len))
        return true;
    sh_tty("%s: no input (give a file, or pipe into it: dmesg | %s)\n", who, who);
    return false;
}

/* ---- keys while a command runs: Ctrl+C ---------------------------------------------- */

void sh_io_new_line(void)
{
    interrupted = false;
}

bool sh_cancelled(void)
{
    return interrupted;
}

/* Ctrl+C was pressed: the line stops, and what was typed before it goes. */
static void interrupt(void)
{
    if (!interrupted)
        sh_tty("^C\n");
    interrupted = true;
    ahead_n = 0;
}

int sh_poll_key(uint64_t deadline)
{
    sh_flush();
    struct input_key_event ev;
    while (sh_get_key(&ev, deadline)) {
        if (sh_is_ctrl(&ev, 'c')) {
            interrupt();
            return 3;
        }
        if (ev.codepoint)
            return (int)ev.codepoint;
    }
    return -1;
}

/* Wait until deadline for one key that isn't the running command's:
 * Ctrl+C stops the line, any other key waits for the line editor, so a
 * line typed ahead is not lost. false if no key came. */
static bool take_key(uint64_t deadline)
{
    struct input_key_event ev;
    if (!sh_get_key(&ev, deadline))
        return false;
    if (sh_is_ctrl(&ev, 'c'))
        interrupt();
    else if (ahead_n < TYPEAHEAD)
        ahead[(ahead_first + ahead_n++) % TYPEAHEAD] = ev;
    return true;
}

bool sh_interrupted(void)
{
    sh_flush();
    while (!interrupted && take_key(0))
        ;
    return interrupted;
}

bool sh_sleep(uint64_t ns)
{
    uint64_t deadline = now() + ns;
    sh_flush();
    for (uint64_t t = now(); !interrupted && t < deadline; t = now()) {
        /* Background programs' output meanwhile (sh_jobs.c). */
        bool bg = sh_jobs_running();
        take_key(bg && deadline - t > SH_JOBS_POLL_NS ? t + SH_JOBS_POLL_NS : deadline);
        if (bg && sh_jobs_poll(false))
            sh_flush();
    }
    return !interrupted;
}

void sh_drop_typeahead(void)
{
    ahead_first = ahead_n = 0;
}

bool sh_typeahead(struct input_key_event *ev)
{
    if (!ahead_n)
        return false;
    *ev = ahead[ahead_first];
    ahead_first = (ahead_first + 1) % TYPEAHEAD;
    ahead_n--;
    return true;
}
