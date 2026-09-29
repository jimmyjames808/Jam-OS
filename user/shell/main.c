/* shell: the command line (M7 Track C).
 *
 * Talks to the console (abi/idl/console.idl): writes through
 * console.write, and reads keys from the channel console.open_keys gives
 * (struct input_key_event; keys from a serial terminal come with usage 0
 * and the character, or with the usage of the special key). It does its
 * own line editing: left/right/home/end, backspace/delete, Ctrl+A/E/U,
 * Ctrl+C (cancel the line, or kill what `run` started), Ctrl+L (clear),
 * up/down for the history. The line is redrawn with \r and ESC [ K, which
 * the console and serial terminals both understand.
 *
 * Startup handles (init, shell mode):
 *   SR_CONSOLE    a client end of the console's channel
 *   SR_RESOURCE   the root resource with RIGHT_READ (log) and RIGHT_MANAGE
 *                 (ktest, bench, stress, ps, mem through debug_command;
 *                 reboot); no RIGHT_MAP / RIGHT_SLICE
 *   SR_USER + 1   RES_PCI, RIGHTS_BASIC only (pci_enum for `devices`)
 *   SR_DEVMGR     a client end of devmgr's channel (`devices`)
 * The programs `run` starts get copies of SR_DEVMGR (as init's programs
 * do) and SR_CONSOLE.
 *
 * Exits 2 when the console goes away (init restarts the console, then the
 * shell with the new console's channel). */
#include <os.h>
#include <devmgr.h>
#include <idl/console.h>

#define LINE_MAX  240
#define HIST      32
#define MS        1000000ull
#define S         1000000000ull
#define PROMPT    "\033[93mjam>\033[0m "
#define PROMPT_W  5

static handle_t con, keys, root, pci, devmgr;

/* ---- output ------------------------------------------------------------------- */

static uint8_t obuf[2048];
static uint32_t on;

static void flush(void)
{
    if (!on)
        return;
    status_t st = console_write(con, (uint16_t)on, obuf);
    on = 0;
    if (st == ERR_PEER_CLOSED)
        jam_process_exit(2);
}

static void put(const char *s, size_t n)
{
    while (n) {
        uint32_t k = sizeof(obuf) - on < n ? sizeof(obuf) - on : (uint32_t)n;
        memcpy(obuf + on, s, k);
        on += k;
        s += k;
        n -= k;
        if (on == sizeof(obuf))
            flush();
    }
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    if (n > 0)
        put(buf, (size_t)n);
}

/* ---- keys ---------------------------------------------------------------------- */

enum {
    U_ENTER = 0x28, U_ESC = 0x29, U_BACKSPACE = 0x2a, U_TAB = 0x2b, U_RIGHT = 0x4f,
    U_LEFT = 0x50, U_DOWN = 0x51, U_UP = 0x52, U_HOME = 0x4a, U_END = 0x4d, U_DELETE = 0x4c,
    U_KP_ENTER = 0x58,
};

/* One key-down (or repeat) event; false on timeout. */
static bool get_key(struct input_key_event *ev, uint64_t deadline)
{
    for (;;) {
        uint32_t n = 0;
        struct channel_read_args a = {
            .h = keys, .bytes_cap = sizeof(*ev), .bytes = (uint64_t)(uintptr_t)ev,
            .actual_bytes = (uint64_t)(uintptr_t)&n,
        };
        status_t st = jam_channel_read(&a);
        if (st == OK) {
            if (n == sizeof(*ev) && ev->state != INPUT_KEY_UP)
                return true;
            continue;
        }
        if (st == ERR_PEER_CLOSED)
            jam_process_exit(2);
        if (st != ERR_SHOULD_WAIT)
            jam_process_exit(3);
        signals_t seen;
        st = jam_object_wait_one(keys, SIG_READABLE | SIG_PEER_CLOSED, deadline, &seen);
        if (st == ERR_TIMED_OUT)
            return false;
        if (st != OK)
            jam_process_exit(3);
    }
}

static bool is_ctrl(const struct input_key_event *ev, char letter)
{
    /* A terminal sends the control character; a keyboard, the letter with Ctrl. */
    if (ev->codepoint == (uint32_t)(letter - 'a' + 1))
        return true;
    return (ev->mods & INPUT_MOD_CTRL) &&
           (ev->codepoint == (uint32_t)letter || ev->usage == 4 + (letter - 'a'));
}

/* ---- the line editor --------------------------------------------------------- */

static char hist[HIST][LINE_MAX + 1];
static unsigned nhist;   /* entries ever added */

static void redraw(const char *line, unsigned len, unsigned pos)
{
    say("\r" PROMPT "%.*s\033[K", (int)len, line);
    if (pos < len)
        say("\033[%uD", len - pos);
}

/* Read one line into buf (NUL-terminated). */
static void read_line(char *buf)
{
    char line[LINE_MAX + 1];
    unsigned len = 0, pos = 0, back = 0;   /* back: 0 = the new line, n = n-th newest */
    char saved[LINE_MAX + 1];              /* the new line while browsing the history */
    say(PROMPT);
    flush();
    for (;;) {
        struct input_key_event ev;
        get_key(&ev, DEADLINE_NEVER);
        uint32_t cp = ev.codepoint;
        uint16_t u = ev.usage;
        bool redo = false;
        if (u == U_ENTER || u == U_KP_ENTER || (!u && (cp == '\n' || cp == '\r'))) {
            line[len] = '\0';
            memcpy(buf, line, len + 1);
            say("\r\n");
            flush();
            return;
        } else if (is_ctrl(&ev, 'c')) {
            say("^C\r\n");
            len = pos = back = 0;
            say(PROMPT);
        } else if (is_ctrl(&ev, 'l')) {
            console_clear(con);
            redo = true;
        } else if (is_ctrl(&ev, 'a') || u == U_HOME) {
            pos = 0;
            redo = true;
        } else if (is_ctrl(&ev, 'e') || u == U_END) {
            pos = len;
            redo = true;
        } else if (is_ctrl(&ev, 'u')) {
            memmove(line, line + pos, len - pos);
            len -= pos;
            pos = 0;
            redo = true;
        } else if (u == U_BACKSPACE || (!u && (cp == 8 || cp == 0x7f))) {
            if (pos) {
                memmove(line + pos - 1, line + pos, len - pos);
                pos--;
                len--;
                redo = true;
            }
        } else if (u == U_DELETE) {
            if (pos < len) {
                memmove(line + pos, line + pos + 1, len - pos - 1);
                len--;
                redo = true;
            }
        } else if (u == U_LEFT) {
            if (pos) {
                pos--;
                say("\033[D");
            }
        } else if (u == U_RIGHT) {
            if (pos < len) {
                pos++;
                say("\033[C");
            }
        } else if (u == U_UP || u == U_DOWN) {
            unsigned have = nhist < HIST ? nhist : HIST;
            unsigned nb = u == U_UP ? (back < have ? back + 1 : back) : (back ? back - 1 : 0);
            if (nb != back) {
                if (back == 0) {
                    memcpy(saved, line, len);
                    saved[len] = '\0';
                }
                back = nb;
                const char *src = back ? hist[(nhist - back) % HIST] : saved;
                len = pos = (unsigned)strlen(src);
                memcpy(line, src, len);
                redo = true;
            }
        } else if (cp >= 0x20 && cp < 0x7f && !(ev.mods & (INPUT_MOD_CTRL | INPUT_MOD_ALT))) {
            if (len < LINE_MAX) {
                memmove(line + pos + 1, line + pos, len - pos);
                line[pos++] = (char)cp;
                len++;
                if (pos == len) {
                    char c = (char)cp;
                    put(&c, 1);   /* typing at the end: just echo */
                } else {
                    redo = true;
                }
            }
        }
        if (redo)
            redraw(line, len, pos);
        flush();
    }
}

static void remember(const char *line)
{
    if (!line[0])
        return;
    if (nhist && !strcmp(hist[(nhist - 1) % HIST], line))
        return;
    size_t n = strnlen(line, LINE_MAX);
    memcpy(hist[nhist % HIST], line, n);
    hist[nhist % HIST][n] = '\0';
    nhist++;
}

/* ---- commands ------------------------------------------------------------------ */

#define MAX_ARGS 16

static int split(char *s, char **argv)
{
    int n = 0;
    while (*s && n < MAX_ARGS) {
        while (*s == ' ')
            *s++ = '\0';
        if (!*s)
            break;
        argv[n++] = s;
        while (*s && *s != ' ')
            s++;
    }
    return n;
}

static void cmd_help(void)
{
    say("Commands:\n"
        "  help                 this list\n"
        "  devices              PCI functions and the drivers devmgr bound\n"
        "  usb                  USB devices\n"
        "  ps                   jobs and processes\n"
        "  run <prog> [args]    start bin/<prog> (or a bootfs path), wait, show how it ended\n"
        "  ktest [prefix]       kernel tests (as the boot menu's All tests)\n"
        "  bench                kernel benchmark\n"
        "  stress <seconds>     stress test (1..600)\n"
        "  log [lines]          the last lines of the kernel log (default 20)\n"
        "  mem                  memory\n"
        "  kill <name>          kill the first process with that name (see ps)\n"
        "  clear                clear the screen\n"
        "  reboot               restart the machine\n"
        "  panic                test: panic the kernel (its screen must show)\n"
        "Keys: left/right/home/end, backspace/delete, up/down history, Ctrl+C cancel,\n"
        "Ctrl+L clear, Shift+PageUp/PageDown scroll back.\n");
}

/* A kernel command; its output arrives as kernel log lines (above). */
static int64_t kcmd(const char *cmd)
{
    flush();
    int64_t r = jam_debug_command(root, cmd, strlen(cmd));
    if (r < 0)
        say("%s: %s\n", cmd, status_str((status_t)r));
    return r;
}

static void cmd_devices(void)
{
    if (!pci) {
        say("devices: no PCI resource\n");
        return;
    }
    struct pci_dev_info info;
    uint32_t i;
    for (i = 0; jam_pci_enum(pci, i, &info) == OK; i++) {
        /* devmgr names a device by ids and instance: the n-th with these ids. */
        uint32_t inst = 0;
        struct pci_dev_info o;
        for (uint32_t j = 0; j < i; j++)
            if (jam_pci_enum(pci, j, &o) == OK && o.vendor == info.vendor &&
                o.device == info.device)
                inst++;
        const char *drv = "";
        if (devmgr) {
            struct devmgr_rep rep;
            handle_t hs[DEVMGR_MAX_HANDLES];
            uint32_t nh = 0;
            status_t st = devmgr_call(devmgr, DEVMGR_GET_DRIVER, info.vendor, info.device, inst,
                                      &rep, hs, DEVMGR_MAX_HANDLES, &nh,
                                      (uint64_t)jam_clock_get() + 5 * S);
            for (uint32_t k = 0; k < nh; k++)
                jam_handle_close(hs[k]);
            drv = st == OK ? "  driver running" : st == ERR_BAD_STATE ? "  driver gone" : "";
        }
        say("  %02x:%02x.%x %04x:%04x class %02x.%02x.%02x%s%s%s\n", info.bus, info.dev, info.fn,
            info.vendor, info.device, info.class_code, info.subclass, info.prog_if,
            info.flags & PCI_INFO_BRIDGE ? " bridge" : "",
            info.flags & PCI_INFO_DISPLAY ? " display" : "", drv);
    }
    say("%u PCI function%s\n", i, i == 1 ? "" : "s");
    if (devmgr) {
        struct devmgr_rep rep;
        status_t st = devmgr_call(devmgr, DEVMGR_STATUS, 0, 0, 0, &rep, NULL, 0, NULL,
                                  (uint64_t)jam_clock_get() + 5 * S);
        if (st == OK)
            say("devmgr: %u bound, %u failed, %u skipped\n", rep.a, rep.b, rep.c);
        else
            say("devmgr: %s\n", status_str(st));
    }
}

static void cmd_run(int argc, char **argv)
{
    if (argc < 2) {
        say("usage: run <prog> [args]\n");
        return;
    }
    char path[128];
    if (strchr(argv[1], '/'))
        snprintf(path, sizeof(path), "%s", argv[1]);
    else
        snprintf(path, sizeof(path), "bin/%s", argv[1]);
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    if (bootfs_default(&fs) != OK || bootfs_lookup(fs, path, &data, &size) != OK) {
        say("run: no %s in bootfs\n", path);
        return;
    }
    handle_t job, proc;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    if (st != OK) {
        say("run: no job (%s)\n", status_str(st));
        return;
    }
    struct spawn_handle x[2];
    unsigned nx = 0;
    handle_t h;
    if (devmgr && jam_handle_duplicate(devmgr, RIGHT_SAME, &h) == OK)
        x[nx++] = (struct spawn_handle){ SR_DEVMGR, h };
    if (jam_handle_duplicate(con, RIGHT_SAME, &h) == OK)
        x[nx++] = (struct spawn_handle){ SR_CONSOLE, h };
    argv[1] = path;
    struct spawn_args a = {
        .path = path, .argc = argc - 1, .argv = (const char *const *)argv + 1, .job = job,
        .extra = x, .nextra = nx,
    };
    uint64_t t0 = (uint64_t)jam_clock_get();
    st = spawn(&a, &proc);
    if (st != OK) {
        say("run: can't start %s (%s)\n", path, status_str(st));
        jam_handle_close(job);
        return;
    }
    say("run: %s started (Ctrl+C kills it)\n", path);
    flush();
    struct process_info info;
    bool killed = false;
    while ((st = spawn_wait(proc, 50 * MS, &info)) == ERR_TIMED_OUT) {
        struct input_key_event ev;
        while (get_key(&ev, 0))
            if (is_ctrl(&ev, 'c') && !killed) {
                say("^C: killing %s\n", path);
                flush();
                jam_job_kill(job);
                killed = true;
            }
    }
    uint64_t ms = ((uint64_t)jam_clock_get() - t0) / MS;
    if (st != OK)
        say("run: lost track of %s (%s)\n", path, status_str(st));
    else if (info.killed)
        say("run: %s was killed after %lu ms\n", path, (unsigned long)ms);
    else
        say("run: %s exited with code %ld after %lu ms\n", path, (long)info.exit_code,
            (unsigned long)ms);
    struct job_info ji;
    if (jam_job_get_info(job, &ji) == OK) {
        bool clean = true;
        for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
            clean &= ji.used[k] == 0;
        if (!clean)
            say("run: its job still holds %lu pages, %lu handles, %lu threads\n",
                (unsigned long)ji.used[JOB_LIMIT_PAGES], (unsigned long)ji.used[JOB_LIMIT_HANDLES],
                (unsigned long)ji.used[JOB_LIMIT_THREADS]);
    }
    jam_handle_close(proc);
    jam_handle_close(job);
}

static void cmd_log(int argc, char **argv)
{
    unsigned want = 20;
    if (argc > 1) {
        want = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
            want = want * 10 + (unsigned)(*p - '0');
        if (!want || want > 1000)
            want = 20;
    }
    handle_t r;
    status_t st = jam_klog_open(root, &r);
    if (st != OK) {
        say("log: %s\n", status_str(st));
        return;
    }
    size_t cap = 64 * 1024;
    char *buf = malloc(cap);
    uint64_t first = 0, pos = 0, got = 0;
    int64_t n;
    while (buf && got < cap && (n = jam_klog_read(r, pos, buf + got, cap - got, &first)) > 0) {
        if (got == 0)
            pos = first;
        got += (uint64_t)n;
        pos = first + (uint64_t)n;
    }
    jam_handle_close(r);
    if (!buf) {
        say("log: out of memory\n");
        return;
    }
    /* The last `want` complete lines. */
    size_t start = got;
    unsigned lines = 0;
    while (start > 0) {
        if (buf[start - 1] == '\n' && start != got && ++lines == want)
            break;
        start--;
    }
    say("\033[90m");
    put(buf + start, got - start);
    say("\033[0m");
    free(buf);
}

static void cmd_mem(void)
{
    int64_t free_mib = kcmd("mem");
    struct job_info ji;
    if (jam_job_get_info(startup_handle(SR_JOB), &ji) == OK)
        say("shell's job: %lu pages, %lu handles, %lu threads, %lu message bytes\n",
            (unsigned long)ji.used[JOB_LIMIT_PAGES], (unsigned long)ji.used[JOB_LIMIT_HANDLES],
            (unsigned long)ji.used[JOB_LIMIT_THREADS], (unsigned long)ji.used[JOB_LIMIT_MSG_BYTES]);
    if (free_mib >= 0)
        say("mem: %ld MiB free\n", (long)free_mib);
}

static void run_command(char *line)
{
    char *argv[MAX_ARGS + 1];
    int argc = split(line, argv);
    if (!argc)
        return;
    argv[argc] = NULL;
    const char *c = argv[0];
    if (!strcmp(c, "help") || !strcmp(c, "?")) {
        cmd_help();
    } else if (!strcmp(c, "devices")) {
        cmd_devices();
    } else if (!strcmp(c, "usb")) {
        say("usb: no USB bus driver yet (M7 Track A); `devices` lists the controller\n");
    } else if (!strcmp(c, "ps")) {
        kcmd("ps");
    } else if (!strcmp(c, "run")) {
        cmd_run(argc, argv);
    } else if (!strcmp(c, "ktest")) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "ktest%s%s", argc > 1 ? " " : "", argc > 1 ? argv[1] : "");
        int64_t r = kcmd(cmd);
        if (r >= 0)
            say("shell: %s: %ld passed\n", cmd, (long)r);
    } else if (!strcmp(c, "bench")) {
        if (kcmd("bench") >= 0)
            say("shell: bench: done (results in the log above)\n");
    } else if (!strcmp(c, "stress")) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "stress %s", argc > 1 ? argv[1] : "");
        int64_t r = kcmd(cmd);
        if (r >= 0)
            say("shell: %s: %s\n", cmd, r == 0 ? "PASSED" : "FAILED");
    } else if (!strcmp(c, "log")) {
        cmd_log(argc, argv);
    } else if (!strcmp(c, "kill")) {
        if (argc != 2) {
            say("usage: kill <name>\n");
        } else {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "kill %s", argv[1]);
            int64_t r = kcmd(cmd);
            if (r >= 0)
                say("shell: killed process %ld (%s)\n", (long)r, argv[1]);
        }
    } else if (!strcmp(c, "mem")) {
        cmd_mem();
    } else if (!strcmp(c, "clear")) {
        flush();
        console_clear(con);
    } else if (!strcmp(c, "panic")) {
        kcmd("panic");
    } else if (!strcmp(c, "reboot")) {
        say("rebooting...\n");
        flush();
        status_t st = jam_reboot(root);
        say("reboot: %s\n", status_str(st));
    } else {
        say("%s: unknown command (try help)\n", c);
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    con = startup_handle(SR_CONSOLE);
    root = startup_handle(SR_RESOURCE);
    pci = startup_handle(SR_USER + 1);
    devmgr = startup_handle(SR_DEVMGR);
    if (!con) {
        printf("shell: no console channel\n");
        return 1;
    }
    status_t st = console_open_keys(con, &keys);
    if (st != OK) {
        printf("shell: console.open_keys: %s\n", status_str(st));
        return st == ERR_PEER_CLOSED ? 2 : 1;
    }
    say("\n\033[1mJam OS shell.\033[0m Type \033[1mhelp\033[0m for the commands.\n");
    for (;;) {
        char line[LINE_MAX + 1];
        read_line(line);
        remember(line);
        run_command(line);
        flush();
    }
}
