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
 *   SR_DEVMGR     devmgr's query channel, a client end (`devices`, `usb`)
 *   SR_DEVMGR_CTL devmgr's control channel, a client end: only handed on to
 *                 the test programs of `utest` and `usbtest`
 *   SR_USER + 2   a channel from init: when devmgr dies, init starts it
 *                 again (with its drivers) and sends the new client end
 *                 here (INIT_SHELL_DEVMGR, <devmgr.h>); every command that
 *                 talks to devmgr takes the newest first
 * The programs `run` starts get a PROGRAM-level console channel of their
 * own (console.new_client: no input sources) and nothing of devmgr's; the
 * shell kills a program's job when it ends. Ctrl+C reaches the shell even
 * while the program holds the keys (the console sees to that).

 *
 * Tests as commands (M7 cleanup; the boot menu keeps only what must run
 * without a keyboard): `utest` and `usbtest` run those programs and show
 * their result lines; `crash <name> yes` runs one of the kernel's crash
 * tests (debug_command "crash"); `demo` runs bin/demo, which borrows the
 * screen from the console (console.lend_screen) until it ends or a key is
 * pressed; `pci` and `memmap` are the old Devices and memory map entries.
 *
 * Exits 2 when the console goes away (init restarts the console, then the
 * shell with the new console's channel). */
#include <os.h>
#include <devmgr.h>
#include <idl/console.h>
#include <idl/usbbus.h>
#include "sh.h"

#define LINE_MAX  240
#define HIST      32
#define MS        1000000ull
#define S         1000000000ull
#define PROMPT    "\033[93mjam>\033[0m "
#define PROMPT_W  5

static handle_t con, keys, root, pci, devmgr, devmgr_ctl, from_init;

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
    if (sh_capture(s, n))
        return;   /* into a pipe (sh_exec.c) */
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
/* The console's current line is one screen row and the redraw goes back
 * with \r: a line that wraps can't be redrawn (each redraw would commit
 * another copy of its first row). So a line fits the row (main). */
static unsigned line_max = LINE_MAX;

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
        } else if (u == U_TAB || (!u && cp == '\t')) {
            redo = sh_complete(line, &len, &pos, LINE_MAX);
        } else if (cp >= 0x20 && cp < 0x7f && !(ev.mods & (INPUT_MOD_CTRL | INPUT_MOD_ALT))) {
            if (len < line_max) {
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

/* ---- devmgr: the newest client end ------------------------------------------ */

static void drop(handle_t *h)
{
    if (*h)
        jam_handle_close(*h);
    *h = HANDLE_INVALID;
}

/* init restarts devmgr if it dies (with every driver it ran) and sends us
 * the new query and control client ends: take the newest, and forget dead
 * ones. Returns the query end, 0 while there is none (a restart in
 * progress, or no devmgr at all). */
static handle_t devmgr_now(void)
{
    while (from_init) {
        uint32_t kind = 0, n = 0, nh = 0;
        handle_t h[2] = { HANDLE_INVALID, HANDLE_INVALID };
        struct channel_read_args a = {
            .h = from_init, .bytes_cap = sizeof(kind), .bytes = (uint64_t)(uintptr_t)&kind,
            .actual_bytes = (uint64_t)(uintptr_t)&n, .handles = (uint64_t)(uintptr_t)h,
            .handles_cap = 2, .actual_handles = (uint64_t)(uintptr_t)&nh,
        };
        if (jam_channel_read(&a) != OK)
            break;   /* nothing new (or init's end is gone) */
        if (n == sizeof(kind) && kind == INIT_SHELL_DEVMGR && nh == 2) {
            drop(&devmgr);
            drop(&devmgr_ctl);
            devmgr = h[0];
            devmgr_ctl = h[1];
        } else {
            for (uint32_t i = 0; i < nh; i++)
                jam_handle_close(h[i]);
        }
    }
    handle_t *ends[2] = { &devmgr, &devmgr_ctl };
    for (unsigned i = 0; i < 2; i++) {
        signals_t seen = 0;
        if (*ends[i] && jam_object_wait_one(*ends[i], SIG_PEER_CLOSED, 0, &seen) == OK &&
            (seen & SIG_PEER_CLOSED))
            drop(ends[i]);
    }
    return devmgr;
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
        "  pci                  the kernel's PCI report (BARs, MSI/MSI-X)\n"
        "  usb                  USB devices (from usb-bus)\n"
        "  ps                   jobs and processes\n"
        "  run <prog> [args]    start bin/<prog> (or a bootfs path), wait, show how it ended\n"
        "  ktest [prefix]       kernel tests (as the boot menu's All tests)\n"
        "  utest                the user-space tests (bin/utest)\n"
        "  usbtest              the USB checks (bin/usbtest)\n"
        "  bench                kernel benchmark\n"
        "  stress <seconds>     stress test (1..600)\n"
        "  demo [seconds]       the visual demo on every CPU (any key stops it)\n"
        "  log [lines]          the last lines of the kernel log (default 20)\n"
        "  mem                  memory\n"
        "  memmap               the loader's memory map\n"
        "  kill <name>          kill the first process with that name (see ps)\n"
        "  clear                clear the screen\n"
        "  reboot               restart the machine\n"
        "  crash [name]         the kernel's crash tests (each panics the machine)\n"
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
        if (devmgr_now()) {
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
    if (!devmgr_now())
        say("devmgr: not running (restarting?)\n");
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

/* usb-bus: the bound driver that answers usbbus.status (as usbtest finds it). */
static handle_t find_usb_bus(void)
{
    for (uint32_t n = 0; devmgr_now() && n < 16; n++) {
        struct devmgr_rep r;
        handle_t hs[1];
        uint32_t nh = 0;
        status_t st = devmgr_call(devmgr, DEVMGR_GET_SERVICE, 0xffff, 0xffff, n, &r, hs, 1, &nh,
                                  (uint64_t)jam_clock_get() + 5 * S);
        if (st == ERR_NOT_FOUND)
            break;
        if (st != OK || nh != 1)
            continue;
        uint32_t d, h, i, hid, p, g;
        uint8_t settled;
        if (usbbus_status_until(hs[0], (uint64_t)jam_clock_get() + 2 * S, &d, &h, &i, &hid, &p, &g,
                                &settled) == OK)
            return hs[0];
        jam_handle_close(hs[0]);
    }
    return HANDLE_INVALID;
}

static const char *usb_speed(uint8_t s)
{
    static const char *const names[] = { "?", "FS", "LS", "HS", "SS", "SS+" };
    return s < 6 ? names[s] : "?";
}

static void cmd_usb(void)
{
    handle_t bus = find_usb_bus();
    if (!bus) {
        say("usb: no USB bus driver bound (devmgr has none that answers usbbus)\n");
        return;
    }
    uint32_t ndev, nhub, nif, nhid, nprob, gen;
    uint8_t settled;
    status_t st = usbbus_wait_settled_until(bus, (uint64_t)jam_clock_get() + 5 * S, 3000, &ndev,
                                            &nhub, &nif, &nhid, &nprob, &gen, &settled);
    if (st != OK) {
        say("usb: %s\n", status_str(st));
        jam_handle_close(bus);
        return;
    }
    say("usb: %u device(s), %u hub(s), %u interface(s) (%u HID), %u problem(s)%s\n", ndev, nhub,
        nif, nhid, nprob, settled ? "" : ", still settling");
    struct { uint32_t id; uint16_t vendor, product; } seen[64];
    uint32_t nseen = 0;
    for (uint32_t i = 0; i < ndev; i++) {
        uint32_t id, parent, route;
        uint16_t vendor, product, bcd, mp0;
        uint8_t speed, addr, slot, rport, port, level, tts, ttp, cls, sub, proto, ncfg, cfg, nifs,
            hubports, path[24], name[40], serial[24];
        if (usbbus_device_until(bus, (uint64_t)jam_clock_get() + 2 * S, i, &id, &parent, &vendor,
                                &product, &bcd, &speed, &addr, &slot, &rport, &port, &level,
                                &route, &tts, &ttp, &cls, &sub, &proto, &ncfg, &cfg, &nifs,
                                &mp0, &hubports, path, name, serial) != OK)
            break;
        path[23] = name[39] = '\0';
        if (nseen < 64)
            seen[nseen++] = (typeof(seen[0])){ id, vendor, product };
        say("  %-8s %04x:%04x %-3s", (char *)path, vendor, product, usb_speed(speed));
        if (cls == 9)
            say(" hub, %u ports", hubports);
        for (uint8_t k = 0; k < nifs && k < 8; k++) {
            uint8_t num, alt, nalt, icls, isub, iproto, nep, eps[8];
            if (usbbus_interface_until(bus, (uint64_t)jam_clock_get() + 2 * S, id, k, &num, &alt,
                                       &nalt, &icls, &isub, &iproto, &nep, eps) != OK)
                continue;
            const char *what = icls == 3 && isub == 1 && iproto == 1 ? " kbd"
                             : icls == 3 && isub == 1 && iproto == 2 ? " mouse"
                             : icls == 3 ? " hid" : icls == 8 ? " storage" : icls == 9 ? "" : "";
            if (icls != 9)
                say(" if%u %02x.%02x.%02x%s", num, icls, isub, iproto, what);
        }
        for (uint32_t j = 0; parent && j < nseen; j++)
            if (seen[j].id == parent)
                say(" behind hub %04x:%04x", seen[j].vendor, seen[j].product);
        if (name[0])
            say("  \"%s\"", (char *)name);
        say("\n");
    }
    jam_handle_close(bus);
}

/* (`run` itself is the command layer's: cmds_shell.c sh_run_program, where
 * the rules for what a program gets are.) */
static void cmd_run(int argc, char **argv)
{
    if (argc < 2) {
        say("usage: run <prog> [args]\n");
        return;
    }
    sh_set_status(sh_run_program(argc - 1, argv + 1));
}

/* The kernel log from position `from` on (up to 64 KiB); *got: bytes. */
static char *log_since(uint64_t from, size_t *got)
{
    handle_t r;
    *got = 0;
    if (jam_klog_open(root, &r) != OK)
        return NULL;
    size_t cap = 64 * 1024;
    char *buf = malloc(cap);
    uint64_t pos = from, first = 0;
    int64_t n;
    while (buf && *got < cap && (n = jam_klog_read(r, pos, buf + *got, cap - *got, &first)) > 0) {
        *got += (size_t)n;
        pos = first + (uint64_t)n;
    }
    jam_handle_close(r);
    return buf;
}

/* Where the kernel log ends now. */
static uint64_t log_end(void)
{
    handle_t r;
    uint64_t first = 0;
    char c;
    if (jam_klog_open(root, &r) != OK)
        return 0;
    jam_klog_read(r, UINT64_MAX, &c, 1, &first);   /* past the end: 0 bytes, first = the end */
    jam_handle_close(r);
    return first;
}

/* A test program (utest, usbtest): run it, then show its result lines
 * ("<name>: N passed ...", which it also puts in the RESULTS box) from what
 * it logged. */
static void cmd_test_prog(int argc, char **argv)
{
    const char *name = argv[0];
    uint64_t from = log_end();
    int code = sh_run_program_ex(argc, argv, true);
    sh_set_status(code);   /* `utest exit7; echo $?` as for any program */
    if (argc > 1)
        return;   /* a child mode (utest's own), not the suite: no result line */
    bool ok = code == 0;
    size_t got;
    char *log = log_since(from, &got);
    char pat[40];
    int pl = snprintf(pat, sizeof(pat), "%s: ", name);
    unsigned shown = 0;
    for (size_t i = 0; log && i < got;) {
        size_t e = i;
        while (e < got && log[e] != '\n')
            e++;
        for (size_t k = i; k + (size_t)pl < e; k++) {
            if (memcmp(log + k, pat, (size_t)pl) || log[k + pl] < '0' || log[k + pl] > '9')
                continue;
            bool passed_line = false;
            for (size_t j = k + (size_t)pl; j + 7 <= e && !passed_line; j++)
                passed_line = !memcmp(log + j, " passed", 7);
            if (!passed_line)
                break;
            say("\033[1m%s%.*s\033[0m\n", ok ? "" : "\033[91m", (int)(e - k), log + k);
            shown++;
            break;
        }
        i = e + 1;
    }
    free(log);
    if (!shown)
        say("%s: no result line in the log\n", name);
}

/* The kernel's crash tests. Each stops the machine with a panic screen
 * (bp excepted), so a name must be confirmed with "yes". */
static void cmd_crash(int argc, char **argv)
{
    if (argc == 1) {
        kcmd("crash");
        say("usage: crash <name> yes   (each one panics the kernel on purpose, bp excepted)\n");
        return;
    }
    if (argc != 3 || strcmp(argv[2], "yes")) {
        say("crash %s: this stops the machine on purpose; type \"crash %s yes\" to go ahead\n",
            argv[1], argv[1]);
        return;
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "crash %s", argv[1]);
    say("crash %s: here goes\n", argv[1]);
    int64_t r = kcmd(cmd);
    if (r == 0)
        say("crash %s: came back%s\n", argv[1],
            strcmp(argv[1], "bp") ? " (it should have panicked!)" : ", as a breakpoint must");
    else if (r == ERR_NOT_FOUND)
        say("crash: no test called %s (try crash alone)\n", argv[1]);
}

/* The visual demo: bin/demo with a thread per CPU; it borrows the screen
 * from the console and gives it back at the end. */
static void cmd_demo(int argc, char **argv)
{
    struct sys_info si;
    uint32_t cpus = jam_sys_info(root, &si) == OK && si.cpu_count ? si.cpu_count : 1;
    char c[24], s[24], d[16];
    snprintf(c, sizeof(c), "cpus=%u", cpus);
    snprintf(s, sizeof(s), "seconds=%s", argc > 1 ? argv[1] : "76");
    snprintf(d, sizeof(d), "demo");
    char *av[] = { d, c, s, NULL };
    say("demo: fractals on %u CPUs for %s s; any key stops it\n", cpus, argc > 1 ? argv[1] : "76");
    sh_set_status(sh_run_program(3, av));
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
    } else if (!strcmp(c, "pci")) {
        kcmd("devices");
    } else if (!strcmp(c, "memmap")) {
        kcmd("memmap");
    } else if (!strcmp(c, "utest") || !strcmp(c, "usbtest")) {
        cmd_test_prog(argc, argv);

    } else if (!strcmp(c, "crash")) {
        cmd_crash(argc, argv);
    } else if (!strcmp(c, "demo")) {
        cmd_demo(argc, argv);
    } else if (!strcmp(c, "usb")) {
        cmd_usb();
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
        } else if (!strcmp(argv[1], "init")) {
            /* The kernel's kill reaches the whole job tree. Nobody restarts
             * init: it supervises everything else (killed, the kernel prints
             * its RESULTS box while the rest runs on unsupervised). devmgr
             * may go: init starts it again, with its drivers. */
            say("kill: %s is not restarted by anyone: not killing it\n", argv[1]);

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
        sh_unknown(argc, argv);   /* a program in /boot/bin, or unknown */
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
    devmgr_ctl = startup_handle(SR_DEVMGR_CTL);
    from_init = startup_handle(SR_USER + 2);

    if (!con) {
        printf("shell: no console channel\n");
        return 1;
    }
    status_t st = console_open_keys(con, &keys);
    if (st != OK) {
        printf("shell: console.open_keys: %s\n", status_str(st));
        return st == ERR_PEER_CLOSED ? 2 : 1;
    }
    uint16_t cols = 0, rows = 0;
    if (console_size(con, &cols, &rows) == OK && cols > PROMPT_W + 1 &&
        cols - PROMPT_W - 1 < LINE_MAX)
        line_max = cols - PROMPT_W - 1;
    sh_init();
    say("\n\033[1mJam OS shell.\033[0m Type \033[1mhelp\033[0m for the commands.\n");
    for (;;) {
        char line[LINE_MAX + 1];
        read_line(line);
        remember(line);
        sh_line(line);   /* sh_exec.c: variables, aliases, ; && || and pipes */
        flush();
    }
}

/* ---- glue for the command layer (sh.h) ------------------------------------------ */

void sh_put_raw(const char *s, size_t n) { put(s, n); }
void sh_flush(void) { flush(); }
bool sh_get_key(struct input_key_event *ev, uint64_t deadline) { return get_key(ev, deadline); }
bool sh_is_ctrl(const struct input_key_event *ev, char letter) { return is_ctrl(ev, letter); }
void sh_main_command(char *line) { run_command(line); }
unsigned sh_history_count(void) { return nhist; }
const char *sh_history_at(unsigned i)
{
    return i < nhist && nhist - i <= HIST ? hist[i % HIST] : NULL;
}
handle_t sh_console(void) { return con; }
handle_t sh_root(void) { return root; }
handle_t sh_devmgr(void) { return devmgr_now(); }
handle_t sh_devmgr_ctl(void) { devmgr_now(); return devmgr_ctl; }
