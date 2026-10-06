/* The calm panic screen's two cases (<jam/panicscreen.h>), timed by the
 * TSC with interrupts off and the other CPUs halted (debug/panic.c stops
 * them first).
 *
 * Case 1, the stored kernel will start: the title "Jam OS hit a problem
 * and is restarting" and the code under it, the ring turning for
 * PANIC_HOLD_MS (panichold=<ms>, a test aid, holds it longer for a
 * screenshot), then back to the panic for the jump.
 *
 * Case 2, it can't recover: the title, "Restarting the PC in N s" counting
 * down each second and the code; after PANIC_DETAILS_S the details panel
 * below them (the code and its kind, where, the address its digits come
 * from, the message, a short backtrace, why there is no restart, the boot
 * and the build); at PANIC_RESET_S the firmware reset, which leaves the
 * screen as it is (machine_reset_try). If it comes back, the ring stops
 * and the words say to hold the power button; the panel stays. The triple
 * fault, the reset's last way, comes after that screen is up: if it
 * resets the machine the screen goes with it, else it stays.
 *
 * Without a framebuffer the timing is the same and nothing is drawn.
 * Without a clock (a panic before the TSC was measured) nothing can be
 * timed: case 1 jumps at once, and case 2 shows the details and the power
 * button's words at once and halts, rather than reset a machine that
 * would panic again before it could say anything.
 *
 * Every line also goes to the log and the serial port (kprintf, which
 * the panic made synchronous), so a test can follow the screen. */
#include <jam/bootfs.h>
#include <jam/cmdline.h>
#include <jam/console_svc.h>
#include <jam/fbcon.h>
#include <jam/kexec.h>
#include <jam/kprintf.h>
#include <jam/ksyms.h>
#include <jam/panic.h>
#include <jam/panicscreen.h>
#include <jam/string.h>
#include <jam/sysinfo.h>
#include <jam/time.h>
#include <jam/x86.h>

#define PANEL_BG   0x1a1e27u   /* the details panel: a little lighter than PANIC_BG */
#define PANEL_EDGE 0x262a35u   /* its one-pixel outline (the ring's outline colour) */
#define LABEL_COLS 11          /* the panel's labels' column, characters */
#define FRAME_HZ   60          /* the ring's frames a second at most */
#define BUILD_MAX  32

_Static_assert(8 + 1 + PANIC_FRAMES == PANIC_PANEL_LINES,
               "the panel's lines: code, where, address, the message on two, the note, the "
               "frames, why, boot, build");

struct panic_report panic_report;

static struct panic_canvas cv;
static bool drawn;                  /* cv is the framebuffer (panic_screen_begin) */
static struct panic_layout lay;
static uint32_t turn;               /* the ring's last frame */
static char build[BUILD_MAX];       /* bootfs build.txt's first line ("git 2079f35") */
static uint64_t hold_ms = PANIC_HOLD_MS;

void panic_screen_init(void)
{
    const void *data;
    uint64_t size;
    if (bootfs_data("build.txt", &data, &size) == OK) {
        const char *p = data;
        size_t n = 0;
        for (; n < size && n + 1 < sizeof(build) && p[n] != '\n'; n++)
            build[n] = p[n] >= 0x20 && p[n] < 0x7f ? p[n] : '?';
        build[n] = '\0';
    }
    hold_ms = cmdline_get_u64("panichold", PANIC_HOLD_MS, PANIC_HOLD_MS);
    if (hold_ms > PANIC_HOLD_MAX_MS)
        hold_ms = PANIC_HOLD_MAX_MS;
    if (hold_ms != PANIC_HOLD_MS)
        kprintf("panic:       panichold: a panic's restart screen is held %lu ms (a test aid)\n",
                hold_ms);
}

const char *panic_screen_build(void)
{
    return build[0] ? build : "?";
}

void panic_screen_begin(void)
{
    struct boot_framebuffer fb;
    if (!fbcon_geometry(&fb))
        return;   /* no 32-bit framebuffer: nothing is drawn */
    cv = (struct panic_canvas){ (volatile uint32_t *)fb.virt, fb.width, fb.height, fb.pitch / 4,
                                fb.red_shift, fb.green_shift, fb.blue_shift };
    panic_layout(cv.w, cv.h, &lay);
    drawn = true;
    panic_fill(&cv, 0, 0, (int)cv.w, (int)cv.h, PANIC_BG);
}

/* ---- drawing ------------------------------------------------------------------------- */

static void centred(const struct panic_font *f, int y, uint32_t rgb, const char *s)
{
    panic_text(&cv, f, ((int)cv.w - panic_text_width(f, s)) / 2, y, rgb, s);
}

/* The lines under the ring: the title, then the small line if there is
 * one, then the code; what was there before cleared first. */
static void words(const char *title, const char *small)
{
    if (!drawn)
        return;
    int top = lay.ring_y + PANIC_RING_PX / 2 + 4, bottom = lay.code_y + 8;
    panic_fill(&cv, 0, top, (int)cv.w, bottom - top, PANIC_BG);
    centred(&panic_font_title, lay.title_y, PANIC_INK_TITLE, title);
    if (small)
        centred(&panic_font_small, lay.small_y, PANIC_INK_SMALL, small);
    centred(&panic_font_small, small ? lay.code_y : lay.small_y, PANIC_INK_SMALL,
            panic_report.cause.code);
}

static void ring(void)
{
    if (drawn)
        panic_ring(&cv, lay.ring_x, lay.ring_y, turn);
}

/* The ring turning, once a second from t0, until the TSC reaches end. */
static void spin(uint64_t t0, uint64_t end)
{
    uint64_t next = 0, t;
    while ((t = rdtsc()) < end) {
        if (t >= next) {
            turn = (uint32_t)((t - t0) % tsc_hz * 65536 / tsc_hz);
            ring();
            next = t + tsc_hz / FRAME_HZ;
        }
        cpu_relax();
    }
}

/* One line of the panel: a label, then the value cut to fit. */
static void panel_line(int *y, const char *label, const char *value)
{
    int x = lay.panel_x + 16, cols = lay.panel_cols;
    panic_mono(&cv, x, *y, PANIC_INK_SMALL, PANEL_BG, label, LABEL_COLS - 1);
    panic_mono(&cv, x + 8 * LABEL_COLS, *y, PANIC_INK_TITLE, PANEL_BG, value, cols - LABEL_COLS);
    *y += 16;
}

/* An address as name+offset, or the bare address. */
static void symbol(char *buf, size_t size, uint64_t addr)
{
    uint64_t off;
    const char *name = ksym_lookup(addr, &off);
    if (name)
        ksnprintf(buf, size, "%s+0x%lx", name, off);
    else
        ksnprintf(buf, size, "%016lx", addr);
}

static void panel_frames(int *y)
{
    const struct panic_report *r = &panic_report;
    char line[128];
    for (unsigned i = 0; i < r->nframes; i++) {
        symbol(line, sizeof(line), r->frames[i]);
        panel_line(y, i ? "" : "backtrace", line);
    }
}

static void panel(const char *why)
{
    if (!drawn)
        return;
    const struct panic_report *r = &panic_report;
    bool note = r->note && r->note[0];
    size_t room = (size_t)(lay.panel_cols - LABEL_COLS);   /* a value's characters a line */
    bool long_msg = strlen(r->message) > room;
    int lines = 7 + long_msg + note + (int)r->nframes, h = lines * 16 + 24;
    panic_fill(&cv, lay.panel_x, lay.panel_y, lay.panel_w, h, PANEL_EDGE);
    panic_fill(&cv, lay.panel_x + 1, lay.panel_y + 1, lay.panel_w - 2, h - 2, PANEL_BG);
    int y = lay.panel_y + 12;
    char line[160], sym[96];
    ksnprintf(line, sizeof(line), "%s, %s", r->cause.code, r->cause.what);
    panel_line(&y, "code", line);
    symbol(sym, sizeof(sym), r->at);
    ksnprintf(line, sizeof(line), "%s on cpu %u", sym, r->cpu);
    panel_line(&y, "where", line);
    ksnprintf(line, sizeof(line), "%016lx, %s", r->cause.addr, r->cause.hex_of);
    panel_line(&y, "address", line);
    panel_line(&y, "message", r->message);
    if (long_msg)   /* the message goes on, on a second line (cut there) */
        panel_line(&y, "", r->message + room);
    if (note)
        panel_line(&y, "note", r->note);
    panel_frames(&y);
    panel_line(&y, "no restart", why);
    const char *name = kexec_log_name();
    name = name[0] ? name : "(no log file yet)";
    uint64_t ms = tsc_hz ? uptime_ns() / 1000000 : 0;
    if (tsc_hz)
        ksnprintf(line, sizeof(line), "%s, after %lu.%03lu s", name, ms / 1000, ms % 1000);
    else
        ksnprintf(line, sizeof(line), "%s, before the clock was measured", name);
    panel_line(&y, "boot", line);
    ksnprintf(line, sizeof(line), "Jam OS %s, %s", jamos_version, panic_screen_build());
    panel_line(&y, "build", line);
}

/* ---- the two cases ------------------------------------------------------------------- */

void panic_screen_restarting(void)
{
    words(PANIC_TITLE_RESTARTING, NULL);
    ring();
    kprintf("panic screen: \"%s\", %s, for %lu ms\n", PANIC_TITLE_RESTARTING,
            panic_report.cause.code, tsc_hz ? hold_ms : 0);
    if (!tsc_hz)
        return;
    uint64_t t0 = rdtsc();
    spin(t0, t0 + hold_ms * (tsc_hz / 1000));
}

/* The firmware reset didn't happen (or reset=none tried none, or there
 * was no clock to count to it with). last_way: try the triple fault now
 * that the screen says so. */
_Noreturn static void no_reset(bool last_way)
{
    words(PANIC_TITLE_NORESET, PANIC_SMALL_POWER);
    kprintf("panic screen: \"%s\": hold the power button\n", PANIC_TITLE_NORESET);
    if (last_way)
        machine_reset_triple();   /* with reset=none, nothing */
    kprintf("system halted.\n");
    halt_forever();
}

_Noreturn void panic_screen_stuck(const char *why)
{
    if (!panic_report.cause.code[0])   /* not a panic's: a jump that failed */
        panic_cause_message(&panic_report.cause, why, 0);
    panic_screen_begin();   /* everything drawn again: a case 1 screen may be up */
    ring();
    kprintf("panic screen: \"%s\", %s: the details at %u s, the firmware reset at %u s\n",
            PANIC_TITLE_STUCK, panic_report.cause.code, PANIC_DETAILS_S, PANIC_RESET_S);
    if (!tsc_hz) {
        kprintf("panic screen: no clock to count with: no reset\n");
        panel(why);
        no_reset(false);
    }
    uint64_t t0 = rdtsc();
    char line[48];
    for (unsigned k = 0; k < PANIC_RESET_S; k++) {
        ksnprintf(line, sizeof(line), PANIC_SMALL_COUNTDOWN, PANIC_RESET_S - k);
        words(PANIC_TITLE_STUCK, line);
        if (k == PANIC_DETAILS_S) {
            panel(why);
            kprintf("panic screen: the details are up\n");
        }
        spin(t0, t0 + (k + 1) * tsc_hz);
    }
    machine_reset_try();   /* returns only if the machine is still here */
    no_reset(true);
}
