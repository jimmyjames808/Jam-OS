/* console: the notices (console.h). With the kernel log off the screen (a
 * plain boot: the argument "nolog"), a few things the log says are worth
 * a line on the screen anyway. Each is one line, in yellow, above the line
 * being typed, and goes into the log too ("console: notice: ..."), so the
 * boot's file on /data has it and the serial port shows it:
 *
 *   a stick plugged in and mounted ("a stick is at /usb0 ...") or pulled
 *     out; the Jam OS stick itself pulled out (/data and /esp gone) and
 *     back. From init's "/x mounted" and "/x is gone" lines, announced
 *     once the mounts have stayed the same for SETTLE: a remount
 *     (`mount -w`) or a filesystem service's restart comes back within
 *     that and says nothing, and while devmgr is being started again
 *     (all its mounts go and come back) nothing is said for up to HOLD.
 *     The boot stick's first mounts at boot are not news.
 *   /data not there SETTLE_DATA after /esp came (a damaged volume, or a
 *     partition fat can't read): nothing is saved, logs and settings
 *     included.
 *   /data full: logd's "no /data/logs (ERR_NO_SPACE)".
 *   a service that crashed and is being started again: init's "bin/x was
 *     killed" right after the kernel's "process "x" killed: <fault>" (a
 *     kill someone asked for says "killed by" instead and is no news), or
 *     devmgr's "drv/x crashed: restart"; and one that keeps crashing and
 *     is given up on (init's "not restarting it", devmgr's "giving up",
 *     but for the crash-test driver and other people's sticks).
 * A panic in the last boot is the shell's own first line (init's
 * lastboot.c), not a notice.
 *
 * These lines are matched by their text: a change of wording in init,
 * devmgr, logd or the kernel must change this file too (the QEMU tests of
 * notices catch it). Only lines from the kernel itself or from processes
 * named init, devmgr and logd count. The name in front of a process's
 * lines is the one its creator gave process_create, and any program can
 * start a process in its own job: one called "init" can make notices.
 * They are only words on the screen; nothing acts on them.
 *
 * Never a burst: a text said in the last REPEAT is not said again (of the
 * last RECENT), and at most BURST notices go on the screen in BURST_WINDOW,
 * the rest counted and said once in a line of their own. */
#include "console.h"

#define SETTLE       (3 * NS_PER_S)    /* mounts unchanged this long: announce */
#define HOLD         (20 * NS_PER_S)   /* devmgr restarting: its mounts may come back */
#define SETTLE_DATA  (15 * NS_PER_S)   /* /esp without /data this long: say so */
#define CRASH_PAIR   (2 * NS_PER_S)    /* the kernel's crash line, then init's */
#define REPEAT       (30 * NS_PER_S)
#define BURST        4
#define BURST_WINDOW (10 * NS_PER_S)
#define TEXT_MAX     160
#define RECENT       8                 /* notices remembered for REPEAT */

/* Mounts, as bits: /data, /esp, /usb0 .. /usb7. */
enum { M_DATA = 1u << 0, M_ESP = 1u << 1, M_USB0 = 2, M_USBS = 8 };
#define M_BOOT (M_DATA | M_ESP)

static uint32_t mounts;          /* what init has mounted now */
static uint32_t told;            /* what the screen was last told */
static uint64_t changed_at;      /* when `mounts` last changed (0: settled) */
static uint64_t hold_until;      /* devmgr is being restarted: say nothing before */
static bool     boot_known;      /* the boot stick's mounts were taken as they are */
static uint64_t esp_at;          /* /esp came without /data at this time (0: no) */
static bool     data_said;       /* "/data is not mounted" was said */
static char     crashed[32];     /* the last process the kernel said crashed */
static char     crash_why[48];   /* ... why ("page fault") */
static uint64_t crashed_at;

static char     said[TEXT_MAX];  /* the last notice on the screen (the selftest's) */
static bool     testing;         /* notice_reset was called: the selftest, no log lines */

/* The burst rule's state: the last RECENT notices, and the window. */
static char     recent[RECENT][TEXT_MAX];
static uint64_t recent_at[RECENT];
static unsigned recent_next;
static uint64_t window_start;
static unsigned in_window, swallowed;

/* ---- saying it ------------------------------------------------------------------ */

static void say_now(const char *text)
{
    snprintf(said, sizeof(said), "%s", text);
    notice_out(text, strlen(text));
    if (!testing)
        printf("console: notice: %s\n", text);
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char text[TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    uint64_t t = now();
    for (unsigned i = 0; i < RECENT; i++)
        if (recent_at[i] && t - recent_at[i] < REPEAT && !strcmp(text, recent[i]))
            return;
    if (t - window_start >= BURST_WINDOW) {
        if (swallowed) {
            char more[64];
            snprintf(more, sizeof(more), "(%u more notices: `log` has them)", swallowed);
            say_now(more);
        }
        window_start = t;
        in_window = swallowed = 0;
    }
    memcpy(recent[recent_next], text, TEXT_MAX);
    recent_at[recent_next] = t;
    recent_next = (recent_next + 1) % RECENT;
    if (in_window >= BURST) {
        swallowed++;
        if (!testing)
            printf("console: notice: %s\n", text);   /* the log has it all the same */
        return;
    }
    in_window++;
    say_now(text);
}

/* ---- mounts --------------------------------------------------------------------- */

/* "/data" -> M_DATA, "/usb3" -> its bit; 0 for anything else. */
static uint32_t mount_bit(const char *path, size_t n)
{
    if (n == 5 && !strncmp(path, "/data", 5))
        return M_DATA;
    if (n == 4 && !strncmp(path, "/esp", 4))
        return M_ESP;
    if (n == 5 && !strncmp(path, "/usb", 4) && path[4] >= '0' && path[4] < '0' + M_USBS)
        return 1u << (M_USB0 + (path[4] - '0'));
    return 0;
}

static void mounts_set(uint32_t now_mounts)
{
    if (now_mounts == mounts)
        return;
    if ((now_mounts & M_ESP) && !(now_mounts & M_DATA) && !(mounts & M_ESP))
        esp_at = now();
    if (now_mounts & M_DATA)
        esp_at = 0;
    mounts = now_mounts;
    changed_at = now();
}

/* The other sticks' mounts in bits, as "/usb0" or "/usb0 and /usb2". */
static void usb_names(uint32_t bits, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = '\0';
    unsigned n = 0, total = 0;
    for (unsigned i = 0; i < M_USBS; i++)
        total += (bits >> (M_USB0 + i)) & 1;
    for (unsigned i = 0; i < M_USBS && o < cap; i++) {
        if (!((bits >> (M_USB0 + i)) & 1))
            continue;
        n++;
        o += (size_t)snprintf(out + o, cap - o, "%s/usb%u", n == 1 ? "" : n == total ? " and " : ", ", i);
    }
}

/* Say what changed between `told` and `mounts`. */
static void announce_mounts(void)
{
    uint32_t gone = told & ~mounts, came = mounts & ~told;
    char names[64];
    if ((gone & M_BOOT) == M_BOOT)
        say("the Jam OS stick was pulled out: /data and /esp are gone, and nothing is saved "
            "until it is back");
    else if (gone & M_DATA)
        say("/data is gone: nothing is saved until it is back");
    if ((came & M_BOOT) == M_BOOT)
        say("the Jam OS stick is back: /data and /esp");
    else if ((came & M_DATA) && (told & M_ESP))
        say("/data is back");
    usb_names(gone, names, sizeof(names));
    if (names[0])
        say("the stick at %s was pulled out", names);
    usb_names(came, names, sizeof(names));
    if (names[0])
        say("a stick is at %s (read-only; `mount -w %s` to write to it)", names,
            strchr(names, ' ') ? "/usbN" : names);
    told = mounts;
}

/* ---- the lines ------------------------------------------------------------------ */

/* The process name of an init line ("bin/music" -> "music"). */
static const char *base_name(const char *path, size_t n, size_t *len)
{
    const char *b = path;
    for (size_t i = 0; i < n; i++)
        if (path[i] == '/')
            b = path + i + 1;
    *len = (size_t)(path + n - b);
    return b;
}

/* `text` (an init line, after "init: ") is about a mount or devmgr. */
static void init_mount_line(const char *text)
{
    const char *sp = strchr(text, ' ');
    if (text[0] == '/' && sp) {
        uint32_t bit = mount_bit(text, (size_t)(sp - text));
        if (bit && !strcmp(sp, " mounted"))
            mounts_set(mounts | bit);
        else if (bit && !strcmp(sp, " is gone"))
            mounts_set(mounts & ~bit);
        return;
    }
    if (!strncmp(text, "devmgr and its drivers are gone", 31))
        hold_until = now() + HOLD;
}

static void init_line(const char *text, bool announce)
{
    if (strncmp(text, "init: ", 6))
        return;
    text += 6;
    init_mount_line(text);
    const char *k = strstr(text, " was killed, code");
    if (k && !strncmp(text, "bin/", 4)) {
        size_t n;
        const char *name = base_name(text, (size_t)(k - text), &n);
        bool crash = crashed_at && now() - crashed_at < CRASH_PAIR && n == strlen(crashed) &&
                     !strncmp(name, crashed, n);
        if (crash && announce)
            say("%s crashed (%s): init is starting it again", crashed, crash_why);
        crashed_at = 0;
        return;
    }
    const char *g = strstr(text, " times in a minute: not restarting it");
    if (g && announce) {
        const char *e = strstr(text, " ended ");
        size_t n;
        const char *name = base_name(text, e ? (size_t)(e - text) : 0, &n);
        say("%.*s kept stopping: init gave up on it (`log` says more)", (int)n, name);
    }
}

/* devmgr: "devmgr: <where> drv/<name> crashed: restart ..." and the give-up. */
static void devmgr_line(const char *text, bool announce)
{
    const char *d = strstr(text, " drv/");
    if (strncmp(text, "devmgr: ", 8) || !d || !announce)
        return;
    const char *name = d + 5, *end = strchr(name, ' ');
    if (!end)
        return;
    int n = (int)(end - name);
    int wn = (int)(d - (text + 8));
    if (!strncmp(end, " crashed: restart", 17))
        say("the %.*s driver (%.*s) crashed: devmgr is starting it again", n, name, wn, text + 8);
    else if (strstr(end, ": giving up") && !strstr(end, "(the crash-test driver") &&
             !strstr(end, "(another stick's"))
        say("the %.*s driver (%.*s) kept failing: devmgr gave up on it", n, name, wn, text + 8);
}

/* The kernel's own "user: process "x" killed: <why> at rip ...". */
static void kernel_line_seen(const char *text)
{
    static const char pre[] = "user: process \"";
    if (strncmp(text, pre, sizeof(pre) - 1))
        return;
    const char *name = text + sizeof(pre) - 1, *q = strstr(name, "\" killed: ");
    if (!q)
        return;
    const char *why = q + 10, *at = strstr(why, " at rip ");
    snprintf(crashed, sizeof(crashed), "%.*s", (int)(q - name), name);
    snprintf(crash_why, sizeof(crash_why), "%.*s", at ? (int)(at - why) : 40, why);
    crashed_at = now();
}

void notice_take(const char *s, size_t n, bool announce)
{
    char line[512];
    if (n >= sizeof(line))
        n = sizeof(line) - 1;
    memcpy(line, s, n);
    line[n] = '\0';
    /* "[    1.234567] " (the stamp), then "[name] " for a process's line. */
    const char *p = line;
    if (*p == '[' && (p = strchr(p, ']')) && p[1] == ' ')
        p += 2;
    else
        return;
    if (*p != '[') {
        kernel_line_seen(p);
        return;
    }
    const char *e = strchr(p, ']');
    if (!e || e[1] != ' ')
        return;
    size_t nl = (size_t)(e - p - 1);
    const char *text = e + 2;
    if (nl == 4 && !strncmp(p + 1, "init", 4))
        init_line(text, announce);
    else if (nl == 6 && !strncmp(p + 1, "devmgr", 6))
        devmgr_line(text, announce);
    else if (nl == 4 && !strncmp(p + 1, "logd", 4) && announce &&
             strstr(text, "(ERR_NO_SPACE): the log is not being saved"))
        say("/data is full: the boot log is not being saved");
}

/* ---- time ----------------------------------------------------------------------- */

uint64_t notice_deadline(void)
{
    uint64_t d = DEADLINE_NEVER;
    if (changed_at)
        d = changed_at + SETTLE > hold_until ? changed_at + SETTLE : hold_until;
    if (esp_at && !data_said && esp_at + SETTLE_DATA < d)
        d = esp_at + SETTLE_DATA;
    return d;
}

void notice_tick(bool shown)
{
    uint64_t t = now();
    if (shown)
        return;   /* the log itself is on the screen: whatever is left waits */
    if (esp_at && !data_said && t >= esp_at + SETTLE_DATA) {
        data_said = true;
        say("/data is not mounted: nothing is saved, the boot log and settings included "
            "(`log` says why)");
    }
    if (!changed_at || t < changed_at + SETTLE)
        return;
    if (t < hold_until && mounts != told)
        return;   /* devmgr is restarting: its mounts may yet come back */
    changed_at = 0;
    hold_until = 0;
    if (!boot_known) {   /* the boot stick's own mounts at boot are no news */
        told = (told & ~M_BOOT) | (mounts & M_BOOT);
        boot_known = (mounts & M_BOOT) == M_BOOT;
        if (boot_known && data_said)
            say("/data is mounted now");
    }
    if (mounts != told)
        announce_mounts();
}

void notice_settle(void)
{
    told = mounts;
    boot_known = boot_known || (mounts & M_BOOT) == M_BOOT;
    changed_at = 0;
    crashed_at = 0;
}

const char *notice_last(void)
{
    return said;
}

void notice_reset(void)
{
    mounts = told = 0;
    changed_at = hold_until = esp_at = crashed_at = window_start = 0;
    boot_known = data_said = false;
    crashed[0] = crash_why[0] = said[0] = '\0';
    memset(recent_at, 0, sizeof(recent_at));
    in_window = swallowed = recent_next = 0;
    testing = true;
}
