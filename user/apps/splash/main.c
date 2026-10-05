/* splash: the boot splash. The owner's logo animation with its sound,
 * played while Jam OS starts (docs/history/AS-PLAN.md).
 *
 * init starts it first on a plain boot, right after the console (init's
 * splash.c), with a PROGRAM-level console channel (SR_CONSOLE), init's
 * channel (SR_USER + SPLASH_INIT_ROLE, <splash.h>) and a namespace with
 * the mixer's `audio` channel (/svc/audio, its list's one want). It:
 *   1. borrows the screen, and not the keys (gfx_open_screen: the screen
 *      stays the kernel's dark background, the video's own; what is typed
 *      meanwhile waits in the console for the shell);
 *   2. plays bootfs's splash.mpg (video.c, draw.c, sound.c): shows its
 *      first frame (dark, like the screen already is) and holds it until
 *      the sound's stream is open, at most SOUND_WAIT, so picture and
 *      sound start together and the sound plays as authored from its
 *      first sample; if the sound is later than that the animation starts
 *      silently and the sound joins where it is by then, faded in. Each
 *      frame is shown when the media clock reaches it (the sound's
 *      position once it plays): late frames are dropped, none is early;
 *   3. plays to its end: no key skips it;
 *   4. tells init SPLASH_PLAYED (init starts the shell), holds the last
 *      frame until init says SPLASH_GO (the shell is up) or closes the
 *      channel, at most HOLD_MAX, and in any case for LINGER after an
 *      animation that played to its end (the logo stays a moment);
 *   5. fades the picture into the console's background and gives the
 *      screen back: the console draws its text.
 * Without init's channel (`run splash` from the shell) it takes the keys
 * too: a key skips the rest (the sound fades out), and it holds the last
 * frame for a second or until a key.
 *
 * `run splash --selftest`: selftest.c; `run splash --alpha`: demo.c.
 * `--hang` (init, on the test word `splashhang`): borrow the screen and
 * never finish, so init's deadline for the splash is tested.
 * Anything that goes wrong on the way
 * ends it early (the console then draws at once); a panic draws over it
 * whatever it is doing (the kernel's). */
#include <wants.h>
#include "splash_int.h"

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("svc audio\n"
          "svc wayland\n");

#define HOLD_MAX   (30 * NS_PER_S)    /* a shell that never comes: give the screen back */
#define LINGER     (500 * NS_PER_MS)  /* the last frame stays at least this long after the end */
#define SOUND_WAIT (2 * NS_PER_S)     /* the first frame waits this long for the sound at most */
#define SHOW_EVERY (100 * NS_PER_MS)  /* behind: a frame shown at least this often (media time) */
#define HOLD_ALONE NS_PER_S           /* run from the shell: the last frame this long */
#define FADE_STEPS 8                  /* the fade out: steps of FADE_STEP */
#define FADE_STEP  (30 * NS_PER_MS)
#define CONSOLE_BG 0x101018u          /* the console's text background (its palette's 0) */

static handle_t init_ch;   /* init's channel (0: run from the shell) */
static bool keys;          /* the keys are ours (run from the shell): a key skips */

/* Wait until `deadline` (uptime); with the keys, watching them. True: a
 * key was pressed. */
static bool wait_key(uint64_t deadline)
{
    if (!keys) {
        jam_nanosleep(deadline);
        return false;
    }
    return gfx_key(deadline) != KEY_NONE;
}

/* Wait until media time `due`. True: a key was pressed. */
static bool wait_until(uint64_t due)
{
    for (;;) {
        uint64_t t = clock_now();
        if (t >= due)
            return false;
        if (wait_key(now() + (due - t)))
            return true;
    }
}

/* The first frame is up: wait for the sound's stream (SOUND_WAIT from
 * `since` at most) and start the clock. True: a key was pressed. */
static bool start_clock(uint64_t since)
{
    uint64_t t = now();
    while (sound_state() == SOUND_PENDING && now() < since + SOUND_WAIT)
        if (wait_key(now() + 5 * NS_PER_MS))
            return true;
    bool with_sound = sound_state() == SOUND_READY;
    clock_start(with_sound);
    printf("splash: waited %lu ms for the sound: %s\n", (unsigned long)((now() - t) / NS_PER_MS),
           with_sound ? "they start together" : "starting without it");
    return false;
}

/* Play the video in time with the media clock. True: a key skipped it. */
static bool play(const uint8_t *mpg, size_t len, uint64_t since)
{
    plm_frame_t *f = video_next();
    if (!f)
        return false;
    video_draw(f);
    gfx_present();
    char mode[32];
    printf("splash: first frame at %lu ms of uptime: %dx%d at %s on %dx%d, %lu ms long\n",
           (unsigned long)(now() / NS_PER_MS), (int)f->width, (int)f->height,
           video_mode(mode, sizeof(mode)), scr.w, scr.h,
           (unsigned long)(video_seconds() * 1000));
    sound_start(mpg, len, svc_get(SVC_AUDIO));
    double fps = video_fps();
    unsigned shown = 1, dropped = 0, n = 1;
    bool skipped = start_clock(since);
    uint64_t last_shown = 0;   /* media time of the last present */
    for (; !skipped && (f = video_next()); n++) {
        uint64_t due = (uint64_t)(n * (double)NS_PER_S / fps);
        uint64_t next = (uint64_t)((n + 1) * (double)NS_PER_S / fps);
        uint64_t t = clock_now();
        if (t >= next && t - last_shown < SHOW_EVERY) {
            dropped++;   /* its time is over already: on to the next */
            skipped = keys && gfx_key(0) != KEY_NONE;
            continue;
        }
        /* On time, or so far behind (a machine too slow to decode every
         * frame in time: QEMU) that one is shown anyway now and then. */
        video_draw(f);
        skipped = wait_until(due);
        gfx_present();
        last_shown = clock_now();
        shown++;
    }
    printf("splash: %s at %lu ms of the animation: %u frames shown, %u dropped\n",
           skipped ? "skipped by a key" : "played", (unsigned long)(clock_now() / NS_PER_MS),
           shown, dropped);
    return skipped;
}

static void tell_init(uint32_t msg)
{
    if (init_ch)
        (void)jam_channel_write(init_ch, &msg, sizeof(msg), NULL, 0);   /* gone: no matter */
}

/* Hold the last frame until init says go (or is gone), or HOLD_MAX; and,
 * with `linger`, until LINGER after `ended` (uptime) whatever init says. */
static void hold(uint64_t ended, bool linger)
{
    if (!init_ch) {
        (void)gfx_key(now() + HOLD_ALONE);
        return;
    }
    signals_t seen = 0;
    status_t st = jam_object_wait_one(init_ch, SIG_READABLE | SIG_PEER_CLOSED, now() + HOLD_MAX,
                                      &seen);
    uint64_t t = now();
    if (linger && t < ended + LINGER)
        jam_nanosleep(ended + LINGER);
    printf("splash: %s %lu ms after the end; giving the screen back %lu ms after it\n",
           st == ERR_TIMED_OUT ? "no shell yet" : "the shell is up",
           (unsigned long)((t - ended) / NS_PER_MS), (unsigned long)((now() - ended) / NS_PER_MS));
}

/* The picture fades into the console's background (premultiplied alpha:
 * step k of n blends 1/(n - k + 1) of the rest, so each step is an even
 * share of the way and the last is the background exactly). */
static void fade_out(void)
{
    for (int k = 1; k <= FADE_STEPS; k++) {
        uint32_t a = 255u / (uint32_t)(FADE_STEPS - k + 1);
        fill_pm(&scr.s, 0, 0, scr.w, scr.h, argb_pm(CONSOLE_BG, a));
        gfx_present();
        jam_nanosleep(now() + FADE_STEP);
    }
}

static status_t find_video(const uint8_t **mpg, size_t *len)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    status_t st = bootfs_default(&fs);
    if (st == OK)
        st = bootfs_lookup(fs, SPLASH_FILE, &data, &size);
    if (st != OK)
        return st;
    *mpg = data;
    *len = (size_t)size;
    return OK;
}

int main(int argc, char **argv)
{
    const uint8_t *mpg = NULL;
    size_t len = 0;
    init_ch = startup_handle(SR_USER + SPLASH_INIT_ROLE);
    keys = !init_ch;
    pool_start(0);
    if (has_arg(argc, argv, "--selftest")) {
        if (find_video(&mpg, &len) != OK)
            mpg = NULL;
        return splash_selftest(mpg, len);
    }
    if (has_arg(argc, argv, "--alpha"))
        return splash_alpha_demo();
    /* The screen first: the console stays quiet until it gets it back, so
     * a missing or bad video gives it back at once. */
    uint64_t since = now();
    gfx_title("Splash");
    status_t st = keys ? gfx_open_fullscreen(SPLASH_BG) : gfx_open_screen(SPLASH_BG);
    if (st == OK && ((st = find_video(&mpg, &len)) != OK || (st = video_open(mpg, len)) != OK))
        gfx_close();
    if (st != OK) {
        printf("splash: not playing (%s)\n", status_str(st));
        tell_init(SPLASH_PLAYED);
        return 1;
    }
    if (init_ch && has_arg(argc, argv, "--hang")) {
        printf("splash: hanging, as asked (--hang)\n");
        for (;;)
            jam_nanosleep(DEADLINE_NEVER);
    }
    bool skipped = play(mpg, len, since);
    if (skipped)
        sound_stop();
    tell_init(SPLASH_PLAYED);
    hold(now(), !skipped);   /* a skip wants it gone: no linger */
    fade_out();
    sound_wait(now() + NS_PER_S);   /* the sound's tail, or its fade */
    gfx_close();
    video_close();
    return 0;
}
